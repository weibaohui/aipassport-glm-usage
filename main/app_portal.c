// main/app_portal.c —— DNS 劫持 + HTTP 配置门户实现,REST 契约见 app_portal.h。
#include "app_portal.h"

#include <string.h>

#include "app_glm_client.h"
#include "app_net.h"
#include "app_storage.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "app_portal";

// --------------------------------------------------------------- DNS 劫持

#define DNS_PORT 53
#define DNS_BUF_LEN 512

static TaskHandle_t s_dns_task;
static volatile bool s_dns_quit;
static int s_dns_sock = -1;
static httpd_handle_t s_http;
static volatile bool s_running;

// 极简 DNS 应答:任何 A 查询都回 192.168.4.1,把 captive portal 探测引到设备。
// 只处理 QD=1 的标准查询;畸形包直接丢弃(攻击面只有配网内网,风险可忽略)。
static void dns_task(void *arg)
{
    (void)arg;
    uint8_t buf[DNS_BUF_LEN];
    struct sockaddr_storage from;
    socklen_t from_len;

    while (!s_dns_quit) {
        from_len = sizeof(from);
        int n = recvfrom(s_dns_sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&from, &from_len);
        if (n < 12) continue; // 过短,连 DNS 头都不全
        // 头部:ID(2) 标志(2) QDCount(2)…;标志第 15 位=QR,这里只应答查询。
        uint16_t flags = (uint16_t)((buf[2] << 8) | buf[3]);
        if (flags & 0x8000) continue;             // 是应答不是查询
        uint16_t qd = (uint16_t)((buf[4] << 8) | buf[5]);
        if (qd != 1) continue;

        // 构造响应:原样回 ID/问题段 + 标志改为标准应答 + 一条 A 记录 192.168.4.1。
        uint8_t resp[DNS_BUF_LEN];
        size_t qlen = (size_t)n; // 问题段(含 QNAME/QTYPE/QCLASS)在请求尾部,原样回传
        if (qlen + 16 > sizeof(resp)) continue;
        memcpy(resp, buf, qlen);
        resp[2] = 0x81; // QR=1(应答) RD=1(递归期望,直接复述)
        resp[3] = 0x80; // RA=1
        resp[6] = 0; resp[7] = 0; // ANCOUNT 放最后
        resp[8] = 0; resp[9] = 0; // NSCOUNT
        resp[10] = 0; resp[11] = 0; // ARCOUNT
        // 压缩指针 0xC00C 指向偏移 12 的问题名;TYPE=1(A) CLASS=1(IN) TTL=60 RDLENGTH=4
        static const uint8_t tail[12] = { 0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01,
                                          0x00, 0x00, 0x00, 0x3C, 0x00, 0x04 };
        memcpy(resp + qlen, tail, sizeof(tail));
        static const uint8_t ip4[4] = { 192, 168, 4, 1 };
        memcpy(resp + qlen + sizeof(tail), ip4, 4);
        resp[6] = 0; resp[7] = 1; // ANCOUNT=1
        (void)sendto(s_dns_sock, resp, qlen + sizeof(tail) + 4, 0,
                     (struct sockaddr *)&from, from_len);
    }
    vTaskDelete(NULL);
}

// --------------------------------------------------------------- HTTP 处理

// 快照取状态,拼 JSON。每次轮询都会调用,注意锁内只拷贝一次。
static esp_err_t handler_status(httpd_req_t *req)
{
    app_net_status_t st;
    app_net_get_status(&st);
    cJSON *root = cJSON_CreateObject();
    if (!root) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    cJSON_AddNumberToObject(root, "state", st.state);
    cJSON_AddBoolToObject(root, "portal", st.portal_active);
    cJSON_AddBoolToObject(root, "has_config", st.has_config);
    cJSON_AddStringToObject(root, "cur_ssid", st.cur_ssid);
    cJSON_AddStringToObject(root, "ip", st.ip);
    cJSON_AddStringToObject(root, "ap_ssid", st.ap_ssid);
    cJSON_AddNumberToObject(root, "close_s", st.portal_close_s);
    char org[64] = { 0 }, proj[64] = { 0 };
    app_storage_load_org(org, sizeof(org));
    app_storage_load_project(proj, sizeof(proj));
    cJSON_AddStringToObject(root, "org", org);
    cJSON_AddStringToObject(root, "project", proj);
    uint16_t period_s = GLM_API_PERIOD_S;
    app_storage_load_period(&period_s);
    cJSON_AddNumberToObject(root, "period_s", period_s);
    uint16_t soff = 300;
    app_storage_load_screen_off(&soff);
    cJSON_AddNumberToObject(root, "screen_off_s", soff);
    // 用户要求全部回显:Key 也带回(管理页在用户自己的局域网内,可接受)。
    char key_echo[APP_STORAGE_API_KEY_MAX];
    cJSON_AddStringToObject(root, "key",
        app_storage_load_api_key(key_echo, sizeof(key_echo)) ? key_echo : "");
    const char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, txt, HTTPD_RESP_USE_STRLEN);
    cJSON_free((void *)txt);
    return ret;
}

static esp_err_t handler_scan_trigger(httpd_req_t *req)
{
    (void)req;
    app_net_scan(); // 异步:结果由 /api/scan 按 scan_seq 变化轮询取得
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handler_scan_result(httpd_req_t *req)
{
    app_net_status_t st;
    app_net_get_status(&st);
    cJSON *root = cJSON_CreateObject();
    if (!root) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    cJSON_AddNumberToObject(root, "seq", st.scan_seq);
    cJSON_AddNumberToObject(root, "count", st.scan_count);
    cJSON *items = cJSON_AddArrayToObject(root, "items");
    for (uint8_t i = 0; i < st.scan_count && items; i++) {
        cJSON *it = cJSON_CreateObject();
        cJSON_AddStringToObject(it, "ssid", st.scan[i].ssid);
        cJSON_AddNumberToObject(it, "rssi", st.scan[i].rssi);
        cJSON_AddBoolToObject(it, "auth", st.scan[i].auth);
        cJSON_AddItemToArray(items, it);
    }
    const char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, txt, HTTPD_RESP_USE_STRLEN);
    cJSON_free((void *)txt);
    return ret;
}

static esp_err_t handler_saved(httpd_req_t *req)
{
    // 直接读 NVS(httpd 任务上下文,NVS 调用短暂);密码永不下发到浏览器。
    app_netlist_t list;
    bool have = app_storage_load_netlist(&list);
    cJSON *root = cJSON_CreateObject();
    if (!root) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    cJSON_AddNumberToObject(root, "count", have ? list.count : 0);
    cJSON *items = cJSON_AddArrayToObject(root, "items");
    if (have && items) {
        for (uint8_t i = 0; i < list.count; i++) {
            cJSON *it = cJSON_CreateObject();
            cJSON_AddStringToObject(it, "ssid", list.items[i].ssid);
            cJSON_AddBoolToObject(it, "selected", (list.selected == (int8_t)i));
            cJSON_AddItemToArray(items, it);
        }
    }
    const char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, txt, HTTPD_RESP_USE_STRLEN);
    cJSON_free((void *)txt);
    return ret;
}

// 读请求体为 JSON 对象;失败返回 NULL(响应已发送 400)。
static cJSON *read_json_body(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 2048) { // 上限:8 热点 × (32+64+开销) 远小于此
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
        return NULL;
    }
    char *buf = malloc((size_t)total + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return NULL;
    }
    int received = 0;
    while (received < total) {
        int n = httpd_req_recv(req, buf + received, (size_t)(total - received));
        if (n <= 0) {
            free(buf);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
            return NULL;
        }
        received += n;
    }
    buf[total] = '\0';
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    return root;
}

#define APP_ORG_ID_MAX 64   // org-xxx / proj-xxx 形式,64 足够
#define APP_PROJ_ID_MAX 64

// 保存 API Key 与可选的团队套餐上下文(组织/项目 ID)。org/project 成对填写
// 才启用团队套餐接口;任一为空即整体清除团队上下文。
static esp_err_t handler_key(httpd_req_t *req)
{
    cJSON *root = read_json_body(req);
    if (!root) return ESP_FAIL;
    cJSON *key = cJSON_GetObjectItemCaseSensitive(root, "key");
    if (!cJSON_IsString(key) || !key->valuestring || strlen(key->valuestring) >= APP_STORAGE_API_KEY_MAX) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "key invalid");
        return ESP_FAIL;
    }
    cJSON *org = cJSON_GetObjectItemCaseSensitive(root, "org");
    cJSON *proj = cJSON_GetObjectItemCaseSensitive(root, "project");
    const char *org_s = (cJSON_IsString(org) && org->valuestring) ? org->valuestring : "";
    const char *proj_s = (cJSON_IsString(proj) && proj->valuestring) ? proj->valuestring : "";
    if (strlen(org_s) >= APP_ORG_ID_MAX || strlen(proj_s) >= APP_PROJ_ID_MAX) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "org/project too long");
        return ESP_FAIL;
    }
    // 刷新周期:仅在合法值集合内接受(门户下拉的六档);缺省不改动。
    cJSON *period = cJSON_GetObjectItemCaseSensitive(root, "period");
    static const uint16_t allowed[] = { 60, 300, 600, 900, 1800, 3600 };
    bool ok = true;
    if (cJSON_IsNumber(period)) {
        uint16_t v = (uint16_t)period->valueint;
        bool legal = false;
        for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++)
            if (v == allowed[i]) legal = true;
        if (legal) ok = app_storage_save_period(v);
        else {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "period invalid");
            return ESP_FAIL;
        }
    }
    // 熄屏超时:0=永不;其余四档同款校验;缺省不改动。
    cJSON *soff = cJSON_GetObjectItemCaseSensitive(root, "screen_off");
    static const uint16_t soff_allowed[] = { 0, 60, 300, 600, 900, 1800 };
    if (cJSON_IsNumber(soff)) {
        uint16_t v = (uint16_t)soff->valueint;
        bool legal = false;
        for (size_t i = 0; i < sizeof(soff_allowed) / sizeof(soff_allowed[0]); i++)
            if (v == soff_allowed[i]) legal = true;
        if (legal) ok = ok && app_storage_save_screen_off(v);
        else {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "screen_off invalid");
            return ESP_FAIL;
        }
    }
    // Key 为空串 = 不改动已存的 Key(用于只改团队上下文/周期);非空才覆盖。
    if (key->valuestring[0] != '\0') ok = ok && app_storage_save_api_key(key->valuestring);
    ok = ok && app_storage_save_org_project(org_s, proj_s);
    if (ok) {
        // 保存即生效:唤醒查询任务立刻用新 Key/组织/周期重试。
        app_glm_client_refresh_now();
    }
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, ok ? "{\"ok\":true}" : "{\"ok\":false}",
                           HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handler_networks(httpd_req_t *req)
{
    cJSON *root = read_json_body(req);
    if (!root) return ESP_FAIL;
    cJSON *nets = cJSON_GetObjectItemCaseSensitive(root, "networks");
    if (!cJSON_IsArray(nets)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "networks required");
        return ESP_FAIL;
    }
    // 整表替换:网页端每次都带全量勾选。替换前先读旧表,若旧点选的 SSID 仍在
    // 新表中则保持点选 —— 避免"只是加一个热点"就把之前的点选冲掉。
    app_netlist_t list;
    app_netlist_reset(&list);
    app_netlist_t old;
    bool had_old = app_storage_load_netlist(&old);
    char old_sel[APP_NETLIST_SSID_MAX] = { 0 };
    if (had_old && old.selected >= 0 && old.selected < (int8_t)old.count) {
        snprintf(old_sel, sizeof(old_sel), "%s", old.items[old.selected].ssid);
    }
    cJSON *it;
    cJSON_ArrayForEach(it, nets) {
        cJSON *ssid = cJSON_GetObjectItemCaseSensitive(it, "ssid");
        cJSON *pwd = cJSON_GetObjectItemCaseSensitive(it, "pwd");
        if (!cJSON_IsString(ssid) || !ssid->valuestring) continue;
        // 超长/超量条目由 app_netlist_add 拒绝;遇满即停,已收集部分仍保存,
        // 避免用户因一条超长 SSID 丢掉全部选择。
        if (!app_netlist_add(&list, ssid->valuestring,
                             cJSON_IsString(pwd) && pwd->valuestring ? pwd->valuestring : "")) {
            break;
        }
    }
    bool ok = list.count > 0 && app_storage_save_netlist(&list);
    if (ok && old_sel[0] != '\0') {
        (void)app_netlist_select(&list, old_sel);
        (void)app_storage_save_netlist(&list); // 二次写回带点选态,量小可接受
    }
    if (ok) {
        // 保存即生效:任务副本重载 + 立刻按列表自动连接(点选优先)。
        // 不再依赖"重启后生效"——副本与 NVS 不同步正是点连接无效的根因。
        app_net_reload_config();
        app_net_connect_saved();
    }
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, ok ? "{\"ok\":true}" : "{\"ok\":false}",
                           HTTPD_RESP_USE_STRLEN);
}

#define APP_JWT_MAX 1024        // 网页 JWT ~400 字符,留裕量
#define APP_DISCOVER_OUT_MAX 2048

// 用粘贴的一次性网页 JWT 发现组织/项目清单。JWT 只用于本次请求,不落盘;
// 查询用量仍用 API Key(长期有效)。阻塞数秒(设备侧 HTTPS),门户页等待即可。
static esp_err_t handler_discover(httpd_req_t *req)
{
    // 发现需要设备自身有外网(拿网页 Token 调控制台接口)。配网热点模式下
    // 设备尚未联网,提前给出明确指引而不是让用户等超时。
    app_net_status_t st;
    app_net_get_status(&st);
    if (st.state != APP_NET_ONLINE) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req,
            "{\"error\":\"设备还未联网:请先完成热点配网并连接,之后用屏幕上显示的局域网 IP 重新打开本页再试\"}",
            HTTPD_RESP_USE_STRLEN);
    }
    cJSON *root = read_json_body(req);
    if (!root) return ESP_FAIL;
    cJSON *jwt = cJSON_GetObjectItemCaseSensitive(root, "jwt");
    if (!cJSON_IsString(jwt) || !jwt->valuestring || strlen(jwt->valuestring) >= APP_JWT_MAX) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "jwt invalid");
        return ESP_FAIL;
    }
    char *out = malloc(APP_DISCOVER_OUT_MAX);
    if (!out) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    int rc = app_glm_client_discover_projects(jwt->valuestring, out, APP_DISCOVER_OUT_MAX);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret;
    if (rc == ESP_OK) {
        // 正常:直接回传项目清单数组。
        ret = httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    } else if (rc == 1) {
        ret = httpd_resp_send(req, "{\"error\":\"登录 Token 已过期,请在浏览器重新复制\"}",
                              HTTPD_RESP_USE_STRLEN);
    } else {
        ret = httpd_resp_send(req, "{\"error\":\"获取失败,请检查网络后重试\"}",
                              HTTPD_RESP_USE_STRLEN);
    }
    free(out);
    return ret;
}

// 导出全部配置为 JSON(含 WiFi 密码与 API Key —— 本就是用户自己的设备与
// 网络,门户只在设备自身网络可达;导出文件请用户自行保管)。格式:
//   {"v":1,"api_key":…,"org":…,"project":…,"period_s":…,"screen_off_s":…,
//    "networks":[{"ssid":…,"pwd":…}],"selected":…}
static esp_err_t handler_export(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    cJSON_AddNumberToObject(root, "v", 1);

    char key[APP_STORAGE_API_KEY_MAX];
    cJSON_AddStringToObject(root, "api_key",
                            app_storage_load_api_key(key, sizeof(key)) ? key : "");
    char org[64] = { 0 }, proj[64] = { 0 };
    app_storage_load_org(org, sizeof(org));
    app_storage_load_project(proj, sizeof(proj));
    cJSON_AddStringToObject(root, "org", org);
    cJSON_AddStringToObject(root, "project", proj);
    uint16_t period_s = GLM_API_PERIOD_S, soff = 300;
    app_storage_load_period(&period_s);
    app_storage_load_screen_off(&soff);
    cJSON_AddNumberToObject(root, "period_s", period_s);
    cJSON_AddNumberToObject(root, "screen_off_s", soff);

    app_netlist_t list;
    bool have = app_storage_load_netlist(&list);
    cJSON_AddNumberToObject(root, "count", have ? list.count : 0);
    cJSON *nets = cJSON_AddArrayToObject(root, "networks");
    const char *selected = "";
    if (have && nets) {
        for (uint8_t i = 0; i < list.count; i++) {
            cJSON *it = cJSON_CreateObject();
            cJSON_AddStringToObject(it, "ssid", list.items[i].ssid);
            cJSON_AddStringToObject(it, "pwd", list.items[i].pwd);
            cJSON_AddItemToArray(nets, it);
            if (list.selected == (int8_t)i) selected = list.items[i].ssid;
        }
    }
    cJSON_AddStringToObject(root, "selected", have ? selected : "");

    const char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=glm-meter-config.json");
    esp_err_t ret = httpd_resp_send(req, txt, HTTPD_RESP_USE_STRLEN);
    cJSON_free((void *)txt);
    return ret;
}

// 导入导出接口返回的配置 JSON:逐项校验后写回 NVS,恢复点选,重载网络任务并
// 立即触发一次查询。v 字段当前只认 1(向前不兼容时由网页端提示)。
static esp_err_t handler_import(httpd_req_t *req)
{
    cJSON *root = read_json_body(req);
    if (!root) return ESP_FAIL;
    cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "v");
    if (!cJSON_IsNumber(v) || v->valueint != 1) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unsupported version");
        return ESP_FAIL;
    }

    bool ok = true;
    cJSON *key = cJSON_GetObjectItemCaseSensitive(root, "api_key");
    ok = ok && app_storage_save_api_key(cJSON_IsString(key) && key->valuestring ? key->valuestring : "");
    cJSON *org = cJSON_GetObjectItemCaseSensitive(root, "org");
    cJSON *proj = cJSON_GetObjectItemCaseSensitive(root, "project");
    ok = ok && app_storage_save_org_project(
                    cJSON_IsString(org) && org->valuestring ? org->valuestring : "",
                    cJSON_IsString(proj) && proj->valuestring ? proj->valuestring : "");
    cJSON *period = cJSON_GetObjectItemCaseSensitive(root, "period_s");
    if (cJSON_IsNumber(period)) ok = ok && app_storage_save_period((uint16_t)period->valueint);
    cJSON *soff = cJSON_GetObjectItemCaseSensitive(root, "screen_off_s");
    if (cJSON_IsNumber(soff)) ok = ok && app_storage_save_screen_off((uint16_t)soff->valueint);

    // 热点表逐条经 app_netlist_add 校验(超长/超量自动拒绝),再按 selected 恢复点选。
    app_netlist_t list;
    app_netlist_reset(&list);
    cJSON *nets = cJSON_GetObjectItemCaseSensitive(root, "networks");
    if (cJSON_IsArray(nets)) {
        cJSON *it;
        cJSON_ArrayForEach(it, nets) {
            cJSON *s = cJSON_GetObjectItemCaseSensitive(it, "ssid");
            cJSON *w = cJSON_GetObjectItemCaseSensitive(it, "pwd");
            if (!cJSON_IsString(s) || !s->valuestring) continue;
            if (!app_netlist_add(&list, s->valuestring,
                                 cJSON_IsString(w) && w->valuestring ? w->valuestring : "")) break;
        }
    }
    cJSON *sel = cJSON_GetObjectItemCaseSensitive(root, "selected");
    if (cJSON_IsString(sel) && sel->valuestring && sel->valuestring[0]) {
        (void)app_netlist_select(&list, sel->valuestring);
    }
    ok = ok && app_storage_save_netlist(&list);
    cJSON_Delete(root);

    if (ok) {
        // 导入即生效:网络任务重载列表,查询任务立即用新配置。
        app_net_reload_config();
        app_glm_client_refresh_now();
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, ok ? "{\"ok\":true}" : "{\"ok\":false}",
                           HTTPD_RESP_USE_STRLEN);
}

// 新增热点(免扫描):网页只提交 SSID/密码,设备端读旧表合并保存 —— 已存密码
// 不回显到浏览器,所以合并必须在设备端做。同名条目=覆盖密码,新 SSID=追加;
// 保存后任务副本重载,新热点自动参与断线回退(不主动改点选,避免断开当前连接)。
static esp_err_t handler_networks_add(httpd_req_t *req)
{
    cJSON *root = read_json_body(req);
    if (!root) return ESP_FAIL;
    cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    cJSON *pwd = cJSON_GetObjectItemCaseSensitive(root, "pwd");
    if (!cJSON_IsString(ssid) || !ssid->valuestring || ssid->valuestring[0] == '\0') {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_FAIL;
    }
    const char *pwd_s = (cJSON_IsString(pwd) && pwd->valuestring) ? pwd->valuestring : "";
    app_netlist_t list;
    app_netlist_reset(&list);
    app_storage_load_netlist(&list); // 没有旧表也没关系:从空表开始加
    bool ok = app_netlist_add(&list, ssid->valuestring, pwd_s) &&
              app_storage_save_netlist(&list);
    if (ok) {
        app_net_reload_config(); // 新热点立即参与自动回退
    }
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, ok ? "{\"ok\":true}" : "{\"ok\":false}",
                           HTTPD_RESP_USE_STRLEN);
}

// 删除单个热点:读旧表 → app_netlist_remove → 存回(点选态由 remove 逻辑维护)。
static esp_err_t handler_delete(httpd_req_t *req)
{
    cJSON *root = read_json_body(req);
    if (!root) return ESP_FAIL;
    cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    if (!cJSON_IsString(ssid) || !ssid->valuestring) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_FAIL;
    }
    app_netlist_t list;
    bool ok = false;
    if (app_storage_load_netlist(&list)) {
        for (uint8_t i = 0; i < list.count; i++) {
            if (strcmp(list.items[i].ssid, ssid->valuestring) == 0) {
                ok = app_netlist_remove(&list, i) && app_storage_save_netlist(&list);
                if (ok) app_net_reload_config();
                break;
            }
        }
    }
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, ok ? "{\"ok\":true}" : "{\"ok\":false}",
                           HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handler_connect(httpd_req_t *req)
{
    cJSON *root = read_json_body(req);
    if (!root) return ESP_FAIL;
    cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    if (!cJSON_IsString(ssid) || !ssid->valuestring) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_FAIL;
    }
    // 校验目标在已存列表里(net 任务只认已存热点);不存在直接报错,
    // 不做静默无效 —— 否则页面点了连接却毫无反馈。
    app_netlist_t list;
    bool known = app_storage_load_netlist(&list) &&
                 app_netlist_select(&list, ssid->valuestring);
    if (!known) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid not saved");
        return ESP_FAIL;
    }
    app_net_connect_ssid(ssid->valuestring);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handler_clear(httpd_req_t *req)
{
    (void)req;
    bool ok = app_storage_clear_all();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, ok ? "{\"ok\":true}" : "{\"ok\":false}",
                           HTTPD_RESP_USE_STRLEN);
}

// 配置主页:自包含 HTML(内联 CSS/JS,无外部资源),浏览器自带中文字体。
// 以标准 C 字符串拼接保存(原始字符串 R"(...)" 是 C++ 语法,C 编译器不支持);
static const char PAGE_HTML[] =
    "<!DOCTYPE html>\n"
    "<html lang=\"zh-CN\"><head><meta charset=\"utf-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
    "<title>GLM 用量宝 · 管理</title>\n"
    "<style>\n"
    "body{font-family:-apple-system,\"PingFang SC\",\"Microsoft YaHei\",sans-serif;background:#0e1116;color:#e6e6e6;margin:0;padding:16px;max-width:520px;margin:0 auto}\n"
    "h1{font-size:20px;margin:8px 0 16px}\n"
    ".card{background:#171c24;border-radius:12px;padding:14px;margin-bottom:14px}\n"
    ".card h2{font-size:15px;margin:0 0 10px;color:#7fd4a0}\n"
    "input[type=text],input[type=password]{width:100%;box-sizing:border-box;padding:9px;border-radius:8px;border:1px solid #2c3440;background:#0e1116;color:#e6e6e6;margin:4px 0}\n"
    "select{width:100%;padding:9px;border-radius:8px;border:1px solid #2c3440;background:#0e1116;color:#e6e6e6;margin:4px 0}\n"
    "button{background:#2f9e5f;border:0;color:#fff;border-radius:8px;padding:9px 14px;font-size:14px;margin:4px 4px 0 0}\n"
    "button.ghost{background:#2c3440}\n"
    "button.danger{background:#b03a3a}\n"
    ".ap{display:flex;align-items:center;gap:8px;padding:7px 0;border-bottom:1px solid #232a33}\n"
    ".ap:last-child{border-bottom:0}\n"
    ".ap .name{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}\n"
    ".ap .rssi{color:#8b98a5;font-size:12px}\n"
    ".pwd{margin:2px 0 8px 26px;width:calc(100% - 34px)!important}\n"
    "#status{font-size:13px;color:#9fb0bf;margin-bottom:12px}\n"
    ".ok{color:#7fd4a0}.err{color:#e58f8f}\n"
    "small{color:#8b98a5}\n"
    ".badge{font-size:11px;border-radius:6px;padding:2px 7px;vertical-align:2px;background:#2c3440;color:#9fb0bf}\n"
    ".badge.on{background:#1d4030;color:#7fd4a0}\n"
    "summary{cursor:pointer;margin-top:6px;color:#9fb0bf}\n"
    "</style></head><body>\n"
    "<h1>GLM 用量宝 · 管理</h1>\n"
    "<div id=\"status\">正在获取状态…</div>\n"
    "\n"
    "<div id=\"phase1\">\n"
    "<div class=\"card\"><h2>连接 WiFi</h2>\n"
    "<button onclick=\"scan()\">扫描热点</button>\n"
    "<div id=\"aps\"><small>尚未扫描</small></div>\n"
    "<button onclick=\"saveNets()\">保存并连接</button>\n"
    "<small>可勾选多个热点并分别填写密码;保存后设备自动连接,本热点将关闭</small></div>\n"
    "<div class=\"card\" id=\"saved1wrap\"><h2>已保存热点</h2>\n"
    "<div id=\"saved1\"></div></div>\n"
    "</div>\n"
    "\n"
    "<div id=\"phase2\" style=\"display:none\">\n"
    "<div class=\"card\"><h2>1 · API Key <span id=\"kbadge\" class=\"badge\">…</span></h2>\n"
    "<input type=\"text\" id=\"key\" placeholder=\"粘贴智谱 API Key(bigmodel.cn)\">\n"
    "<button onclick=\"saveKey()\">保存</button>\n"
    "<small>保存后设备数秒内开始查询并显示在屏幕上;个人套餐填到这里即可</small></div>\n"
    "\n"
    "<div class=\"card\"><h2>2 · 刷新与熄屏</h2>\n"
    "<div style=\"margin:2px 0;color:#9fb0bf;font-size:13px\">刷新周期</div>\n"
    "<select id=\"period\" onchange=\"savePeriod()\">\n"
    "<option value=60>1 分钟</option><option value=300>5 分钟</option>\n"
    "<option value=600>10 分钟</option><option value=900>15 分钟</option>\n"
    "<option value=1800>30 分钟</option><option value=3600>1 小时</option>\n"
    "</select>\n"
    "<div style=\"margin:8px 0 2px;color:#9fb0bf;font-size:13px\">无操作熄屏(按任意键唤醒)</div>\n"
    "<select id=\"soff\" onchange=\"savePeriod()\">\n"
    "<option value=60>1 分钟</option><option value=300>5 分钟</option>\n"
    "<option value=600>10 分钟</option><option value=900>15 分钟</option>\n"
    "<option value=1800>30 分钟</option><option value=0>永不</option>\n"
    "</select>\n"
    "<small>熄屏后屏幕全黑(背光+面板休眠),查询照常在后台运行;任意按键唤醒</small></div>\n"
    "\n"
    "<details class=\"card\"><summary style=\"font-size:15px;color:#7fd4a0\">3 · 团队上下文(选填) <span id=\"tbadge\" class=\"badge\">…</span></summary>\n"
    "<input type=\"text\" id=\"org\" placeholder=\"组织 ID org-…\">\n"
    "<input type=\"text\" id=\"project\" placeholder=\"项目 ID proj-…\">\n"
    "<button onclick=\"saveKey()\">保存</button>\n"
    "<small>个人套餐跳过;两个 ID 可手动填写,也可粘贴登录 Token 自动填入</small>\n"
    "<details><summary>粘贴登录 Token 自动填入(团队套餐推荐)</summary>\n"
    "<input type=\"password\" id=\"jwt\" placeholder=\"粘贴 bigmodel.cn 网页登录 Token(F12 → authorization 头)\">\n"
    "<button class=\"ghost\" onclick=\"discover()\">获取组织 / 项目</button>\n"
    "<div id=\"orgsel\"></div><div id=\"projsel\"></div>\n"
    "<small>Token 不存储,仅用于获取团队 ID、项目 ID</small></details></details>\n"
    "\n"
    "<details class=\"card\"><summary style=\"font-size:15px;color:#7fd4a0\">4 · WiFi 设置(改连其他热点)</summary>\n"
    "<div style=\"margin:6px 0 2px;color:#9fb0bf;font-size:13px\">新增热点(免扫描)</div>\n"
    "<input type=\"text\" id=\"addssid\" placeholder=\"WiFi 名称\">\n"
    "<input type=\"password\" id=\"addpwd\" placeholder=\"密码(开放网络留空)\">\n"
    "<button onclick=\"addNet()\">新增热点</button>\n"
    "<small>加入已存列表后,设备会在断线时自动按列表尝试切换连接</small>\n"
    "<div style=\"margin:10px 0 2px;color:#9fb0bf;font-size:13px\">或扫描勾选</div>\n"
    "<button onclick=\"scan()\">扫描热点</button>\n"
    "<div id=\"aps2\"></div>\n"
    "<button onclick=\"saveNets()\">保存并连接</button>\n"
    "<div id=\"saved2\"></div></details>\n"
    "\n"
    "<div class=\"card\"><h2>5 · 其他</h2>\n"
    "<button onclick=\"exportCfg()\">导出配置</button>\n"
    "<input type=\"file\" id=\"impfile\" accept=\"application/json,.json\" style=\"display:none\" onchange=\"importFile(this)\">\n"
    "<button class=\"ghost\" onclick=\"document.getElementById('impfile').click()\">导入配置</button>\n"
    "<button class=\"danger\" onclick=\"clearAll()\">清除全部配置</button>\n"
    "<small>导出/导入含 WiFi 密码与 API Key 的 JSON 文件,请妥善保管;导入后立即生效</small></div>\n"
    "</div>\n"
    "\n"
    "<script>\n"
    "let seq=0, apList=[], orgList=[], cur={}, lastKnown=null, failCnt=0;\n"
    "function showIfSteady(){\n"
    "  // 单次请求失败很常见(设备忙于 TLS/WiFi 省电抖动),连续 3 次才提示,避免闪烁误导\n"
    "  failCnt++;\n"
    "  if(failCnt>=3)$('status').innerHTML='<span class=err>设备暂时无响应,正在重试…</span>';\n"
    "}\n"
    "function badge(id,on){const e=$(id);if(!e)return;e.textContent=on?'已配置':'未配置';e.className='badge'+(on?' on':'')}\n"
    "const $=id=>document.getElementById(id);\n"
    "const esc=s=>{const d=document.createElement('div');d.textContent=(s==null?'':s);return d.innerHTML};\n"
    "async function jget(u){const r=await fetch(u);return r.json()}\n"
    "async function jpost(u,b){const r=await fetch(u,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)});return r.json()}\n"
    "\n"
    "function render(s){\n"
    "  const online=(s.state===3);\n"
    "  $('phase1').style.display=online?'none':'block';\n"
    "  $('phase2').style.display=online?'block':'none';\n"
    "}\n"
    "\n"
    "async function tick(){\n"
    "  try{\n"
    "    const s=await jget('/api/status');\n"
    "    render(s);cur=s;lastKnown=s;failCnt=0;\n"
    "    const st=['空闲','扫描中','连接中','已联网','离线重试'][s.state]||('状态'+s.state);\n"
    "    let t='设备:'+st+(s.cur_ssid?(' · '+esc(s.cur_ssid)):'')+(s.ip?(' · <span class=ok>'+esc(s.ip)+'</span>'):'');\n"
    "    if(s.portal)t+='<br>配网热点 '+esc(s.ap_ssid)+' — 电脑访问 http://192.168.4.1';\n"
    "    if(s.close_s>0)t+='<br><span class=ok>已连上 '+esc(s.cur_ssid)+'!热点即将关闭。完成第二步(Token 和 API Key):让电脑改连设备所在的 WiFi,按设备 DOWN 键从屏幕上查看管理页地址并访问</span>';\n"
    "    if(!$('org').value&&s.org)$('org').value=s.org;\n"
    "    if(!$('project').value&&s.project)$('project').value=s.project;\n"
    "    if(!$('key').value&&s.key)$('key').value=s.key;\n"
    "    badge('kbadge',!!s.key);\n"
    "    badge('tbadge',!!(s.org&&s.project));\n"
    "    if(s.period_s&&!$('period').dataset.done){$('period').value=String(s.period_s);$('period').dataset.done=1}\n"
    "    if(s.screen_off_s!=null&&!$('soff').dataset.done){$('soff').value=String(s.screen_off_s);$('soff').dataset.done=1}\n"
    "    $('status').innerHTML=t;\n"
    "    if(online)loadSaved();\n"
    "  }catch(e){\n"
    "    if(lastKnown&&lastKnown.state===3){\n"
    "      // 已在管理页(阶段二):偶发失联静默重试;热点切走则用记住的 IP 指引新地址。\n"
    "      if(lastKnown.portal){\n"
    "        $('phase1').style.display='none';$('phase2').style.display='none';\n"
    "        $('status').innerHTML='<span class=ok>设备已连上 '+esc(lastKnown.cur_ssid||'网络')+',本热点已关闭。请按设备 DOWN 键,从屏幕上查看管理页地址,让电脑改连设备所在的 WiFi 后访问</span>';\n"
    "      }else{\n"
    "        showIfSteady();\n"
    "      }\n"
    "    }else{\n"
    "      showIfSteady();\n"
    "    }\n"
    "  }\n"
    "}\n"
    "\n"
    "async function scan(){\n"
    "  const box=document.full&&$('aps2')?$('aps2'):$('aps');\n"
    "  $('aps').innerHTML='<small>扫描中…</small>';\n"
    "  if($('aps2'))$('aps2').innerHTML='<small>扫描中…</small>';\n"
    "  await jpost('/api/scan',{});\n"
    "  const oldSeq=seq;\n"
    "  for(let i=0;i<40;i++){\n"
    "    await new Promise(r=>setTimeout(r,500));\n"
    "    const r=await jget('/api/scan');\n"
    "    if(r.seq!==oldSeq){apList=r.items||[];renderAps();return}\n"
    "  }\n"
    "  $('aps').innerHTML='<span class=err>扫描超时,请重试</span>';\n"
    "}\n"
    "function renderAps(){\n"
    "  const html=apList.map((a,i)=>\n"
    "    '<div class=ap><input type=checkbox id=cb'+i+' onchange=pwdBox('+i+')>'+\n"
    "    '<span class=name>'+esc(a.ssid)+(a.auth?'':'(开放)')+'</span>'+\n"
    "    '<span class=rssi>'+a.rssi+'dBm</span></div>'+\n"
    "    '<div id=pw'+i+'></div>').join('');\n"
    "  $('aps').innerHTML=html;\n"
    "  if($('aps2'))$('aps2').innerHTML=html;\n"
    "}\n"
    "function pwdBox(i){\n"
    "  const box=$('pw'+i);\n"
    "  if($('cb'+i).checked){\n"
    "    box.innerHTML='<input class=pwd type=password id=pd'+i+' placeholder=\"'+esc(apList[i].ssid)+' 的密码(开放网络留空)\">';\n"
    "  }else box.innerHTML='';\n"
    "}\n"
    "async function saveNets(){\n"
    "  const nets=[];\n"
    "  for(let i=0;i<apList.length;i++){\n"
    "    if($('cb'+i)&&$('cb'+i).checked){\n"
    "      nets.push({ssid:apList[i].ssid,pwd:$('pd'+i)?$('pd'+i).value:''});\n"
    "    }\n"
    "  }\n"
    "  if(!nets.length){alert('请先勾选热点');return}\n"
    "  const r=await jpost('/api/networks',{networks:nets});\n"
    "  if(r.ok)alert('已保存 '+nets.length+' 个热点,设备正在连接…');else alert('保存失败');\n"
    "}\n"
    "\n"
    "async function loadSaved(){\n"
    "  try{\n"
    "    const r=await jget('/api/saved');\n"
    "    let html='';\n"
    "    (r.items||[]).forEach((it,i)=>{\n"
    "      html+='<div class=ap><span class=name>'+(it.selected?'<span class=ok>[当前] </span>':'')+esc(it.ssid)+'</span>'+\n"
    "        '<button class=ghost onclick=conn('+i+')>连接</button>'+\n"
    "        '<button class=ghost onclick=del('+i+')>删除</button></div>';\n"
    "    });\n"
    "    const h=html||'<small>暂无,请先扫描并保存</small>';\n"
    "    if($('saved1'))$('saved1').innerHTML=h;\n"
    "    if($('saved2'))$('saved2').innerHTML=h;\n"
    "    window._saved=r.items||[];\n"
    "  }catch(e){}\n"
    "}\n"
    "async function conn(i){\n"
    "  const it=window._saved[i];\n"
    "  if(!confirm('让设备连接「'+it.ssid+'」?'))return;\n"
    "  const r=await jpost('/api/connect',{ssid:it.ssid});\n"
    "  if(!r.ok){alert('连接失败:请先保存该热点');return}\n"
    "  // 点击连接即引导:不依赖后续状态判断(热点马上会关,页面留在这里没有意义)\n"
    "  $('phase1').style.display='none';$('phase2').style.display='none';\n"
    "  $('status').innerHTML='<span class=ok>设备正在连接 '+esc(it.ssid)+',本热点即将关闭。</span><br>请让电脑改连设备所在的 WiFi;按设备 DOWN 键,屏幕会显示管理页地址,用浏览器打开它继续设置(API Key、团队项目、刷新周期)。';\n"
    "}\n"
    "async function addNet(){\n"
    "  const s=$('addssid').value.trim();\n"
    "  if(!s){alert('请填写 WiFi 名称');return}\n"
    "  const r=await jpost('/api/networks/add',{ssid:s,pwd:$('addpwd').value});\n"
    "  if(r.ok){$('addssid').value='';$('addpwd').value='';alert('已新增并保存,设备断线时会自动尝试该热点');loadSaved();}\n"
    "  else alert('新增失败(名称超长或列表已满 8 个?)');\n"
    "}\n"
    "async function del(i){\n"
    "  const it=window._saved[i];\n"
    "  if(!confirm('删除已保存的「'+it.ssid+'」?'))return;\n"
    "  await jpost('/api/delete',{ssid:it.ssid});\n"
    "  loadSaved();\n"
    "}\n"
    "\n"
    "async function discover(){\n"
    "  const j=$('jwt').value.trim();\n"
    "  if(!j){alert('请先粘贴登录 Token');return}\n"
    "  $('orgsel').innerHTML='<small>获取中…</small>';$('projsel').innerHTML='';\n"
    "  const r=await jpost('/api/discover',{jwt:j});\n"
    "  if(r.error){$('orgsel').innerHTML='<span class=err>'+esc(r.error)+'</span>';return}\n"
    "  orgList=r||[];\n"
    "  if(!orgList.length){$('orgsel').innerHTML='<span class=err>未发现组织</span>';return}\n"
    "  let oh='<select id=\"orgpick\" onchange=\"orgPicked()\">';\n"
    "  orgList.forEach((o,i)=>{oh+='<option value='+i+'>'+esc(o.orgName||o.orgId)+'</option>'});\n"
    "  oh+='</select>';\n"
    "  $('orgsel').innerHTML=oh;orgPicked();\n"
    "}\n"
    "function orgPicked(){\n"
    "  const o=orgList[$('orgpick').value]||{projects:[]};\n"
    "  let ph='<select id=\"projpick\" onchange=\"projPicked()\">';\n"
    "  (o.projects||[]).forEach((pj,i)=>{ph+='<option value='+i+'>'+esc(pj.name||pj.id)+'</option>'});\n"
    "  ph+='</select>';\n"
    "  $('projsel').innerHTML=ph;projPicked();\n"
    "}\n"
    "function projPicked(){\n"
    "  const o=orgList[$('orgpick').value];const pj=(o.projects||[])[$('projpick').value];\n"
    "  if(!o||!pj)return;\n"
    "  $('org').value=o.orgId;$('project').value=pj.id;\n"
    "  $('status').innerHTML='<span class=ok>已选择 '+esc(o.orgName)+' / '+esc(pj.name)+',填好 API Key 后点保存</span>';\n"
    "}\n"
    "\n"
    "async function saveKey(){\n"
    "  const r=await jpost('/api/key',{key:$('key').value.trim(),org:$('org').value.trim(),project:$('project').value.trim(),period:+($('period')?.value||0)});\n"
    "  if(r.ok)alert('已保存。设备数秒内开始查询用量,请看屏幕');else alert('保存失败(长度超限?)');\n"
    "}\n"
    "async function savePeriod(){\n"
    "  const r=await jpost('/api/key',{key:'',org:$('org').value.trim(),project:$('project').value.trim(),period:+$('period').value,screen_off:+$('soff').value});\n"
    "  if(r.ok)$('status').innerHTML='<span class=ok>已保存并立即生效</span>';else alert('保存失败');\n"
    "}\n"
    "async function exportCfg(){\n"
    "  try{\n"
    "    const r=await fetch('/api/config/export');\n"
    "    const txt=await r.text();\n"
    "    const b=new Blob([txt],{type:'application/json'});\n"
    "    const a=document.createElement('a');a.href=URL.createObjectURL(b);a.download='glm-meter-config.json';a.click();URL.revokeObjectURL(a.href);\n"
    "    $('status').innerHTML='<span class=ok>配置已导出为 JSON 文件</span>';\n"
    "  }catch(e){alert('导出失败')}\n"
    "}\n"
    "async function importFile(input){\n"
    "  const f=input.files&&input.files[0];if(!f)return;\n"
    "  if(!confirm('导入将覆盖设备上的全部配置,继续?')){input.value='';return}\n"
    "  try{\n"
    "    const txt=await f.text();\n"
    "    JSON.parse(txt);\n"
    "    const r=await jpost('/api/config/import',JSON.parse(txt));\n"
    "    if(r.ok){alert('导入成功,配置已生效;若更换了 WiFi,设备会按新列表自动连接');setTimeout(()=>location.reload(),800);}\n"
    "    else alert('导入失败(内容无效或写入失败)');\n"
    "  }catch(e){alert('文件不是有效的 JSON')}\n"
    "  input.value='';\n"
    "}\n"
    "async function clearAll(){\n"
    "  if(!confirm('确定清除 API Key 与所有热点?'))return;\n"
    "  await jpost('/api/clear',{});\n"
    "  alert('已清除。设备将回到配网模式');\n"
    "}\n"
    "window._saved=[];\n"
    "setInterval(tick,1000);tick();\n"
    "</script></body></html>\n"
    ""
;

static esp_err_t handler_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PAGE_HTML, HTTPD_RESP_USE_STRLEN);
}

// captive portal 探测路径:302 到主页,让手机/电脑自动弹窗。
static esp_err_t handler_captive(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

// 未匹配路径一律 302 回主页(生成 404 页面不如直接引导)。
static esp_err_t err_404(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

// --------------------------------------------------------------- 启停

bool app_portal_start(void)
{
    if (s_running) return true;
    if (s_dns_sock < 0) {
        s_dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s_dns_sock < 0) {
            ESP_LOGE(TAG, "DNS socket 创建失败");
            return false;
        }
        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_port = htons(DNS_PORT),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };
        if (bind(s_dns_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            ESP_LOGE(TAG, "DNS bind 失败(53 端口被占?)");
            close(s_dns_sock);
            s_dns_sock = -1;
            return false;
        }
        // 短超时轮询退出标志,避免 stop 时任务卡死在 recvfrom。
        struct timeval tv = { .tv_sec = 0, .tv_usec = 300 * 1000 };
        setsockopt(s_dns_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    s_dns_quit = false;
    if (!s_dns_task &&
        xTaskCreate(dns_task, "app_dns", 3072, NULL, 4, &s_dns_task) != pdPASS) {
        ESP_LOGE(TAG, "DNS 任务创建失败");
        return false;
    }

    if (!s_http) {
        httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
        cfg.max_uri_handlers = 18;
        cfg.stack_size = 6144; // 默认 4096 对 JSON 拼装略紧,加到 6KB
        if (httpd_start(&s_http, &cfg) != ESP_OK) {
            ESP_LOGE(TAG, "HTTP 服务启动失败");
            return false;
        }
        // URI 注册:页面 + REST + 各家 captive 探测路径。
        static const httpd_uri_t routes[] = {
            { .uri = "/",              .method = HTTP_GET,  .handler = handler_index },
            { .uri = "/api/status",    .method = HTTP_GET,  .handler = handler_status },
            { .uri = "/api/scan",      .method = HTTP_GET,  .handler = handler_scan_result },
            { .uri = "/api/scan",      .method = HTTP_POST, .handler = handler_scan_trigger },
            { .uri = "/api/saved",     .method = HTTP_GET,  .handler = handler_saved },
            { .uri = "/api/key",       .method = HTTP_POST, .handler = handler_key },
            { .uri = "/api/networks",  .method = HTTP_POST, .handler = handler_networks },
            { .uri = "/api/networks/add", .method = HTTP_POST, .handler = handler_networks_add },
            { .uri = "/api/config/export", .method = HTTP_GET,  .handler = handler_export },
            { .uri = "/api/config/import", .method = HTTP_POST, .handler = handler_import },
            { .uri = "/api/delete",    .method = HTTP_POST, .handler = handler_delete },
            { .uri = "/api/discover",  .method = HTTP_POST, .handler = handler_discover },
            { .uri = "/api/connect",   .method = HTTP_POST, .handler = handler_connect },
            { .uri = "/api/clear",     .method = HTTP_POST, .handler = handler_clear },
            { .uri = "/generate_204",  .method = HTTP_GET,  .handler = handler_captive },
            { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = handler_captive },
            { .uri = "/connecttest.txt", .method = HTTP_GET, .handler = handler_captive },
        };
        for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
            if (httpd_register_uri_handler(s_http, &routes[i]) != ESP_OK) {
                ESP_LOGE(TAG, "注册路由失败:%s", routes[i].uri);
                httpd_stop(s_http);
                s_http = NULL;
                return false;
            }
        }
        httpd_register_err_handler(s_http, HTTPD_404_NOT_FOUND, err_404);
    }
    s_running = true;
    ESP_LOGI(TAG, "管理门户已就绪:配网期 http://192.168.4.1,联网后 http://<设备IP>");
    return true;
}

void app_portal_stop_dns(void)
{
    if (s_dns_sock >= 0) {
        s_dns_quit = true;
        // 等一个轮询周期让任务自然退出,再关 socket(任务可能正在用)。
        vTaskDelay(pdMS_TO_TICKS(400));
        close(s_dns_sock);
        s_dns_sock = -1;
        s_dns_task = NULL;
        ESP_LOGI(TAG, "DNS 劫持已停止(联网模式)");
    }
}

bool app_portal_running(void)
{
    return s_running;
}
