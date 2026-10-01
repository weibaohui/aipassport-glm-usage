// main/app_glm_client.c —— 用量查询任务实现,契约见 app_glm_client.h。
#include "app_glm_client.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_glm_usage.h"
#include "app_net.h"
#include "app_storage.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

static const char *TAG = "app_glm";

#define EV_REFRESH BIT0
#define RESPONSE_MAX (2 * 1024)  // 官方响应 <1KB;超限视为异常并截断

// ---- 共享快照(自旋锁保护;UI 通过 get_snapshot 读取) ----
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static glm_usage_t s_usage;
static glm_err_t s_err = GLM_ERR_WAIT_NET;
static int64_t s_fetch_epoch_s; // 最近一次完成 HTTP 交互的时间(Unix 秒,可能未对时)
static bool s_last_ok;
static int s_transport_err;     // 最近一次 fetch_once 的 esp_err_t(0=无;诊断用)

static EventGroupHandle_t s_events;

static void publish(const glm_usage_t *u, glm_err_t err, int last_esp_err)
{
    time_t now = time(NULL); // 锁外取时间
    portENTER_CRITICAL(&s_lock);
    if (u) {
        s_usage = *u;
        s_last_ok = (err == GLM_ERR_NONE);
    } else {
        // 传输层失败没有新业务消息:清掉上一轮的 msg,避免界面显示过期原因。
        s_usage.msg[0] = '\0';
    }
    s_err = err;
    if (err == GLM_ERR_HTTP) s_transport_err = last_esp_err;
    s_fetch_epoch_s = (int64_t)now; // 未对时前是小值,UI 仅展示相对量,不受影响
    portEXIT_CRITICAL(&s_lock);
}

int app_glm_client_transport_err(void)
{
    return s_transport_err;
}

void app_glm_client_get_snapshot(glm_usage_t *usage, glm_err_t *err,
                                 int64_t *fetch_epoch_s, bool *last_ok)
{
    portENTER_CRITICAL(&s_lock);
    if (usage) *usage = s_usage;
    if (err) *err = s_err;
    if (fetch_epoch_s) *fetch_epoch_s = s_fetch_epoch_s;
    if (last_ok) *last_ok = s_last_ok;
    portEXIT_CRITICAL(&s_lock);
}

void app_glm_client_refresh_now(void)
{
    if (s_events) xEventGroupSetBits(s_events, EV_REFRESH);
}

// HTTP 事件收集器:把 ON_DATA 分片累积进缓冲(perform 模式读响应体的标准姿势)。
typedef struct {
    char *body;   // 目标缓冲(调用方持有)
    size_t max;   // 缓冲容量
    size_t len;   // 已写入字节数
} body_ctx_t;

static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    body_ctx_t *ctx = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && ctx && evt->data_len > 0) {
        size_t copy = (size_t)evt->data_len;
        if (copy > ctx->max - ctx->len) copy = ctx->max - ctx->len; // 截断保护
        memcpy(ctx->body + ctx->len, evt->data, copy);
        ctx->len += copy;
    }
    return ESP_OK;
}

// 单次 HTTPS 请求。成功(HTTP 交互完成)返回 ESP_OK;http_status/body/body_len
// 带出结果。认证头格式由 bearer 决定(先无前缀,401 后带 Bearer 重试一次)。
// org/project 非空时附带组织/项目请求头;url 为完整请求地址(配额与重置列表两个端点共用)。
static esp_err_t fetch_url_once(const char *url, const char *api_key, bool bearer,
                                const char *org, const char *project,
                                int *http_status, char *body, size_t body_max,
                                size_t *body_len)
{
    char auth[APP_STORAGE_API_KEY_MAX + 16];
    snprintf(auth, sizeof(auth), bearer ? "Bearer %s" : "%s", api_key);

    body_ctx_t ctx = { .body = body, .max = body_max, .len = 0 };
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 10000,
        .buffer_size = 2048,       // TLS 握手缓冲;太小会分段搬移
        .crt_bundle_attach = esp_crt_bundle_attach, // 系统证书包校验 bigmodel 证书链
        .keep_alive_enable = false, // 一分钟一次,无复用价值
        .event_handler = on_http_event,
        .user_data = &ctx,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Accept", "application/json");
    if (org && org[0] && project && project[0]) {
        // 团队套餐上下文:两个头缺一不可(实测只带其一返回空 data)。
        esp_http_client_set_header(client, "bigmodel-organization", org);
        esp_http_client_set_header(client, "bigmodel-project", project);
    }
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        *http_status = esp_http_client_get_status_code(client);
        body[ctx.len] = '\0';
        *body_len = ctx.len;
    }
    esp_http_client_cleanup(client);
    return err;
}

// SNTP:一次初始化,反复等同步。服务器用阿里云 NTP(国内可达性最好)。
static bool s_time_synced;
static void time_sync_cb(struct timeval *tv)
{
    (void)tv;
    s_time_synced = true;
}

static bool ensure_time_synced(void)
{
    if (s_time_synced) return true;
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "ntp.tencent.com");
    esp_sntp_set_time_sync_notification_cb(time_sync_cb);
    esp_sntp_init();
    for (int i = 0; i < 30 && !s_time_synced; i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (!s_time_synced) {
        esp_sntp_stop();
        return false;
    }
    return true;
}

// 网页端控制台域名(注意:用量查询走 open.bigmodel.cn,客户信息走主站)。
#define GLM_CUSTOMER_URL "https://bigmodel.cn/api/biz/customer/getCustomerInfo"
#define DISCOVER_BUF_MAX (6 * 1024)  // 完整响应含头像等字段,留足截断空间

int app_glm_client_discover_projects(const char *jwt, char *out, size_t out_len)
{
    if (!jwt || !jwt[0]) return ESP_ERR_INVALID_ARG;
    char *body = malloc(DISCOVER_BUF_MAX);
    if (!body) return ESP_ERR_NO_MEM;

    body_ctx_t ctx = { .body = body, .max = DISCOVER_BUF_MAX, .len = 0 };
    esp_http_client_config_t cfg = {
        .url = GLM_CUSTOMER_URL,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 10000,
        .buffer_size = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
        .event_handler = on_http_event,
        .user_data = &ctx,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(body);
        return ESP_FAIL;
    }
    // 控制台接口认网页 JWT(不接受 API Key,已实测);头形式与浏览器一致。
    esp_http_client_set_header(client, "Authorization", jwt);
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_err_t err = esp_http_client_perform(client);
    int status = err == ESP_OK ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        free(body);
        return err;
    }
    if (status == 401 || status == 403) {
        free(body);
        return 1; // Token 无效/过期
    }
    if (status != 200) {
        free(body);
        return ESP_FAIL;
    }
    body[ctx.len] = '\0';
    bool parsed = glm_customer_parse_projects(body, ctx.len, out, out_len);
    free(body);
    return parsed ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static void glm_task(void *arg)
{
    (void)arg;
    static char body[RESPONSE_MAX]; // 2KB 静态缓冲,任务栈不放大缓冲

    for (;;) {
        // 1) 等联网(最多一个周期,再不行就发布等待态并重试)。
        app_net_status_t st;
        bool online = false;
        for (int i = 0; i < 30; i++) {
            app_net_get_status(&st);
            if (st.state == APP_NET_ONLINE) { online = true; break; }
            vTaskDelay(pdMS_TO_TICKS(2000));
            EventBits_t bits = xEventGroupGetBits(s_events);
            if (bits & EV_REFRESH) break;
        }
        publish(NULL, online ? GLM_ERR_NONE : GLM_ERR_WAIT_NET, 0);

        // 2) TLS 需要正确时间:未对时先对时(失败则下一周期重试)。
        if (online && !ensure_time_synced()) {
            ESP_LOGW(TAG, "SNTP 对时失败");
            publish(NULL, GLM_ERR_WAIT_TIME, 0);
            vTaskDelay(pdMS_TO_TICKS(GLM_API_PERIOD_S * 1000));
            continue;
        }

        // 3) 读当前刷新周期(门户可改,即改即生效)。
        uint16_t period_s = GLM_API_PERIOD_S;
        app_storage_load_period(&period_s);

        // 4) 读当前 API Key 与团队上下文(门户可能随时改),请求用量。
        if (online) {
            char key[APP_STORAGE_API_KEY_MAX];
            if (!app_storage_load_api_key(key, sizeof(key))) {
                publish(NULL, GLM_ERR_AUTH, 0); // 没 Key 等同密钥无效态,引导用户配网
                vTaskDelay(pdMS_TO_TICKS(GLM_API_PERIOD_S * 1000));
                continue;
            }
            char org[64], project[64];
            bool team = false;
            if (app_storage_load_org(org, sizeof(org)) &&
                app_storage_load_project(project, sizeof(project))) {
                team = true; // 组织/项目成对配置 → 团队套餐接口
            }
            char quota_url[256];
            snprintf(quota_url, sizeof(quota_url), "%s%s", GLM_API_BASE, team ? "?type=2" : "");
            int status = 0;
            size_t blen = 0;
            esp_err_t err = fetch_url_once(quota_url, key, false,
                                           team ? org : NULL, team ? project : NULL,
                                           &status, body, sizeof(body), &blen);
            glm_usage_t parsed = { 0 }; // 传输/状态失败时不进解析:全零可安全判 msg 是否有效
            glm_err_t report = GLM_ERR_NONE;
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "请求失败:%s", esp_err_to_name(err));
                report = GLM_ERR_HTTP;
            } else if (status == 401) {
                // 主口径(无 Bearer)被拒时,试一次 Bearer 口径再定论。
                err = fetch_url_once(quota_url, key, true,
                                     team ? org : NULL, team ? project : NULL,
                                     &status, body, sizeof(body), &blen);
                if (err != ESP_OK) report = GLM_ERR_HTTP;
                else if (status == 401 || status == 403) report = GLM_ERR_AUTH;
            } else if (status == 401 || status == 403) {
                report = GLM_ERR_AUTH;
            } else if (status != 200) {
                ESP_LOGW(TAG, "HTTP %d", status);
                report = GLM_ERR_HTTP;
            }
            if (report == GLM_ERR_NONE) {
                if (!glm_usage_parse(body, blen, &parsed)) {
                    report = GLM_ERR_PARSE;
                } else if (!parsed.ok) {
                    // 业务码非 2xx:按是否有鉴权问题细分。
                    report = glm_usage_is_auth_error(&parsed) ? GLM_ERR_AUTH : GLM_ERR_HTTP;
                }
            }
            int diag = 0;
            if (report == GLM_ERR_HTTP) diag = (err != ESP_OK) ? (int)err : status;
            // 团队模式:追加拉取"剩余重置次数"(独立端点;失败仅置 -1,不影响配额)。
            if (report == GLM_ERR_NONE && team) {
                static const char RESETS_URL[] =
                    "https://open.bigmodel.cn/api/biz/customer-package-reset/list?targetType=TEAM";
                static char rbody[2 * 1024]; // 记录数最多十几条,2KB 足够
                int rstatus = 0;
                size_t rlen = 0;
                if (fetch_url_once(RESETS_URL, key, false, org, project,
                                   &rstatus, rbody, sizeof(rbody), &rlen) == ESP_OK &&
                    rstatus == 200) {
                    glm_resets_parse(rbody, rlen,
                                     &parsed.five_hour_resets_left,
                                     &parsed.week_resets_left);
                }
            }
            // 业务失败(如"当前用户不存在coding plan")也把 parsed 发布出去:
            // 界面要用 msg 说明原因,条目数值保持解析结果即可。
            publish(parsed.msg[0] != '\0' ? &parsed : NULL, report, diag);
            if (report == GLM_ERR_NONE) {
                ESP_LOGI(TAG, "用量已更新:5h=%d%% 周=%d%% MCP=%d/%d level=%s",
                         parsed.tokens_5h_used_pct, parsed.tokens_week_used_pct,
                         parsed.mcp_used, parsed.mcp_total, parsed.level);
            }
        }

        // 5) 睡满一个周期;期间允许手动刷新提前唤醒。
        EventBits_t bits = xEventGroupWaitBits(s_events, EV_REFRESH, pdTRUE, pdFALSE,
                                               pdMS_TO_TICKS((uint32_t)period_s * 1000));
        (void)bits;
    }
    vTaskDelete(NULL);
}

int app_glm_client_start(void)
{
    // 首屏数据置"未知":进度与百分比显示 --,而不是误导性的 0%。
    memset(&s_usage, 0, sizeof(s_usage));
    s_usage.http_code = -1;
    s_usage.tokens_5h_used_pct = -1;
    s_usage.tokens_week_used_pct = -1;
    s_usage.tokens_5h_reset_ms = -1;
    s_usage.tokens_week_reset_ms = -1;
    s_usage.mcp_used = -1;
    s_usage.mcp_total = -1;
    s_usage.mcp_remaining = -1;
    s_events = xEventGroupCreate();
    if (!s_events) return ESP_ERR_NO_MEM;
    if (xTaskCreate(glm_task, "app_glm", 8192, NULL, 4, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}
