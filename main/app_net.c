// main/app_net.c —— WiFi 引擎实现,契约见 app_net.h。
#include "app_net.h"

#include <stdio.h>
#include <string.h>

#include "app_storage.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "app_net";

#define CONNECT_TIMEOUT_MS 20000 // 单次连接超时:WPA2 握手+DHCP 一般 <8s,留一倍裕量
#define RETRY_GAP_MS 5000        // 整轮列表失败后的下一轮间隔
#define PORTAL_CLOSE_DELAY_S 8   // 门户里点选连接成功后,AP 保持的秒数(让用户看到结果)

typedef enum {
    NET_CMD_SCAN = 0,
    NET_CMD_CONNECT_SAVED, // 按列表自动连(启动/掉线重试)
    NET_CMD_CONNECT_SSID,  // 门户点选:连指定 SSID(密码查列表)
    NET_CMD_PORTAL_ON,
    NET_CMD_PORTAL_OFF,
    NET_CMD_RELOAD_CONFIG, // 门户保存/删除热点后,重载 NVS 列表到任务副本
    NET_CMD_STA_DROPPED,   // 预期在线时收到断开事件:清状态并立即重连
} net_cmd_id_t;

typedef struct {
    net_cmd_id_t id;
    char ssid[APP_NET_SSID_LEN]; // NET_CMD_CONNECT_SSID 用
} net_msg_t;

// ---- 共享状态(自旋锁保护;LVGL 定时器/HTTP 处理器只读快照) ----
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static app_net_status_t s_status;                 // 快照主体,含扫描结果
static app_netlist_t s_list;                      // 已保存热点(任务内可变,读侧取快照不直接用)

// ---- 任务与同步原语 ----
static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static EventGroupHandle_t s_events;
#define EV_GOT_IP BIT0
#define EV_STA_FAIL BIT1
#define EV_SCAN_DONE BIT2

static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static esp_event_handler_instance_t s_evt_any;
static bool s_wifi_init;
static volatile bool s_quit;        // 预留:本应用常驻,暂无退出路径
static volatile bool s_expected_up; // 预期 STA 在线(connect 成功置位,断开/重连清零)。
                                    // 断开事件只在预期在线时才算"意外掉线",否则会把
                                    // 主动 disconnect/连接尝试中的失败误判成掉线。

// ------------------------------------------------------------------ 状态发布

static void set_state(app_net_state_t st)
{
    portENTER_CRITICAL(&s_lock);
    s_status.state = st;
    portEXIT_CRITICAL(&s_lock);
}

static void set_cur_ssid(const char *ssid)
{
    char tmp[APP_NET_SSID_LEN];
    snprintf(tmp, sizeof(tmp), "%s", ssid ? ssid : ""); // 锁外格式化,锁内只拷贝
    portENTER_CRITICAL(&s_lock);
    strlcpy(s_status.cur_ssid, tmp, sizeof(s_status.cur_ssid));
    portEXIT_CRITICAL(&s_lock);
}

static void set_ip(const char *ip)
{
    char tmp[APP_NET_IP_LEN];
    snprintf(tmp, sizeof(tmp), "%s", ip ? ip : "");
    portENTER_CRITICAL(&s_lock);
    strlcpy(s_status.ip, tmp, sizeof(s_status.ip));
    portEXIT_CRITICAL(&s_lock);
}

// ------------------------------------------------------------------ 事件

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    switch (id) {
    case WIFI_EVENT_SCAN_DONE:
        xEventGroupSetBits(s_events, EV_SCAN_DONE);
        break;
    case WIFI_EVENT_STA_START:
    case WIFI_EVENT_STA_CONNECTED:
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        // 连接失败与掉线共用此事件;等待方按位区分超时/失败。
        xEventGroupSetBits(s_events, EV_STA_FAIL);
        // 空闲期意外掉线:通知任务立刻清状态并按已存列表重连(路由器重启/
        // 信号丢失等场景)。连接过程中的断开由 connect_one 自己消费,不进这里。
        if (s_expected_up) {
            s_expected_up = false;
            net_msg_t m = { .id = NET_CMD_STA_DROPPED };
            xQueueSend(s_queue, &m, 0);
        }
        break;
    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_events, EV_GOT_IP);
    }
}

// ------------------------------------------------------------------ 内部动作

// 连接单个热点,阻塞直至成功/失败/超时。成功返回 0 并发布 ONLINE+IP。
static esp_err_t connect_one(const char *ssid, const char *pwd)
{
    wifi_config_t cfg = { 0 };
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    if (pwd) strncpy((char *)cfg.sta.password, pwd, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN; // 开放网络也允许(密码为空即连开放)
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) return err;

    xEventGroupClearBits(s_events, EV_GOT_IP | EV_STA_FAIL);
    set_cur_ssid(ssid);
    set_state(APP_NET_CONNECTING);
    err = esp_wifi_connect();
    if (err != ESP_OK) return err;

    EventBits_t bits = xEventGroupWaitBits(s_events, EV_GOT_IP | EV_STA_FAIL,
                                           pdTRUE /*清零以便重试*/, pdFALSE /*任一*/,
                                           pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));
    if (bits & EV_GOT_IP) {
        esp_netif_ip_info_t info;
        char ip[APP_NET_IP_LEN] = "";
        if (esp_netif_get_ip_info(s_sta_netif, &info) == ESP_OK) {
            snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
        }
        set_ip(ip);
        set_state(APP_NET_ONLINE);
        s_expected_up = true;
        ESP_LOGI(TAG, "已连接 %s,IP %s", ssid, ip);
        return ESP_OK;
    }
    if (bits & EV_STA_FAIL) {
        ESP_LOGW(TAG, "连接失败或被拒:%s", ssid);
        return ESP_FAIL;
    }
    ESP_LOGW(TAG, "连接超时:%s", ssid);
    s_expected_up = false; // 主动断开,别让随后的 DISCONNECTED 事件误报掉线
    esp_wifi_disconnect();
    return ESP_ERR_TIMEOUT;
}

// 扫描并发布结果(拷入快照时统一截断 SSID,注意中文 SSID 是多字节)。
static void do_scan(void)
{
    set_state(APP_NET_SCANNING);
    xEventGroupClearBits(s_events, EV_SCAN_DONE);
    esp_err_t err = esp_wifi_scan_start(NULL, false); // 阻塞式交给事件位;NULL=默认全信道
    if (err == ESP_OK) {
        EventBits_t bits = xEventGroupWaitBits(s_events, EV_SCAN_DONE, pdTRUE, pdFALSE,
                                               pdMS_TO_TICKS(8000));
        if (bits & EV_SCAN_DONE) {
            uint16_t count = APP_NET_SCAN_MAX;
            wifi_ap_record_t records[APP_NET_SCAN_MAX] = { 0 };
            // 先在锁外取完整扫描结果并整理,再短暂加锁拷入快照
            // (esp_wifi_scan_get_ap_records 内部会加自己的锁,不能嵌在自旋锁里)。
            if (esp_wifi_scan_get_ap_records(&count, records) == ESP_OK) {
                app_net_scan_item_t items[APP_NET_SCAN_MAX];
                uint8_t kept = 0;
                for (uint16_t i = 0; i < count; i++) {
                    snprintf(items[kept].ssid, APP_NET_SSID_LEN, "%s",
                             (const char *)records[i].ssid);
                    items[kept].rssi = records[i].rssi;
                    items[kept].auth = records[i].authmode != WIFI_AUTH_OPEN;
                    // 隐藏网络(空 SSID)对配网没意义,跳过。
                    if (items[kept].ssid[0] != '\0') kept++;
                }
                portENTER_CRITICAL(&s_lock);
                memcpy(s_status.scan, items, sizeof(items));
                s_status.scan_count = kept;
                s_status.scan_seq++;
                portEXIT_CRITICAL(&s_lock);
            }
        } else {
            ESP_LOGW(TAG, "扫描等待超时");
        }
    } else {
        ESP_LOGW(TAG, "扫描启动失败:%s", esp_err_to_name(err));
    }
    // 扫描不影响连接状态:若此前已在线,回到 ONLINE;否则回 IDLE。
    set_state(s_expected_up ? APP_NET_ONLINE : APP_NET_IDLE);
}

// 整轮尝试已保存列表(点选优先)。全部失败返回 false。
static bool try_saved_round(void)
{
    for (uint8_t attempt = 0; attempt < s_list.count; attempt++) {
        app_netlist_entry_t target;
        if (!app_netlist_next_target(&s_list, attempt, &target)) break;
        esp_err_t err = connect_one(target.ssid, target.pwd);
        if (err == ESP_OK) return true;
    }
    return false;
}

// ------------------------------------------------------------------ 配网 AP

static void portal_ap_start(void)
{
    wifi_config_t ap_cfg = { 0 };
    char ssid[APP_NET_SSID_LEN];
    portENTER_CRITICAL(&s_lock);
    strlcpy(ssid, s_status.ap_ssid, sizeof(ssid));
    portEXIT_CRITICAL(&s_lock);
    strlcpy((char *)ap_cfg.ap.ssid, ssid, sizeof(ap_cfg.ap.ssid)); // wifi ssid 字段只有 32 字节
    ap_cfg.ap.ssid_len = strlen((const char *)ap_cfg.ap.ssid);
    ap_cfg.ap.channel = 6;                 // 1/6/11 任选;6 居中,家庭环境干扰通常最小
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;   // 开放配网:门户只在内网短暂可用,见交付说明

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA); // AP+STA 并存:配网时仍可试连
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "切换 APSTA 失败:%s", esp_err_to_name(err));
        return;
    }
    err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "配置 AP 失败:%s", esp_err_to_name(err));
        return;
    }
    portENTER_CRITICAL(&s_lock);
    s_status.portal_active = true;
    s_status.portal_close_s = -1;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "配网 AP 已开启:%s(网关 192.168.4.1)", ap_cfg.ap.ssid);
}

static void portal_ap_stop(void)
{
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA); // 关 AP 侧,保留 STA
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "切回 STA 失败:%s", esp_err_to_name(err));
        return;
    }
    portENTER_CRITICAL(&s_lock);
    s_status.portal_active = false;
    s_status.portal_close_s = -1;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "配网 AP 已关闭");
}

// 门户请求的点选连接:成功后给 AP 一个倒计时(用户能看到"已连接"),随后自动关闭。
static void connect_from_portal(const char *ssid)
{
    const app_netlist_entry_t *found = NULL;
    for (uint8_t i = 0; i < s_list.count; i++) {
        if (strcmp(s_list.items[i].ssid, ssid) == 0) {
            found = &s_list.items[i];
            break;
        }
    }
    if (!found) {
        ESP_LOGW(TAG, "点选的热点不在已保存列表:%s", ssid);
        return;
    }
    (void)app_netlist_select(&s_list, ssid);
    (void)app_storage_save_netlist(&s_list); // 点选即持久化,重启后仍指向它
    if (connect_one(found->ssid, found->pwd) == ESP_OK && s_status.portal_active) {
        portENTER_CRITICAL(&s_lock);
        s_status.portal_close_s = PORTAL_CLOSE_DELAY_S;
        portEXIT_CRITICAL(&s_lock);
    }
}

// ------------------------------------------------------------------ 主任务

static void net_task(void *arg)
{
    (void)arg;
    net_msg_t msg;
    TickType_t last_retry = 0;
    while (!s_quit) {
        TickType_t wait = pdMS_TO_TICKS(1000);
        if (xQueueReceive(s_queue, &msg, wait) == pdTRUE) {
            switch (msg.id) {
            case NET_CMD_SCAN:
                do_scan();
                break;
            case NET_CMD_CONNECT_SAVED:
                if (!try_saved_round()) {
                    set_state(APP_NET_OFFLINE_RETRY);
                    set_cur_ssid("");
                    set_ip("");
                    last_retry = xTaskGetTickCount();
                    ESP_LOGW(TAG, "整轮列表连接失败,%.1fs 后重试", RETRY_GAP_MS / 1000.0);
                }
                break;
            case NET_CMD_CONNECT_SSID:
                connect_from_portal(msg.ssid);
                break;
            case NET_CMD_PORTAL_ON:
                portal_ap_start();
                break;
            case NET_CMD_PORTAL_OFF:
                portal_ap_stop();
                break;
            case NET_CMD_STA_DROPPED:
                // 意外掉线:清陈旧 IP(界面不再显示假的已连接),立即重连。
                set_ip("");
                set_state(APP_NET_OFFLINE_RETRY);
                if (!try_saved_round()) {
                    set_state(APP_NET_OFFLINE_RETRY);
                    last_retry = xTaskGetTickCount();
                }
                continue; // 掉线重连优先于下一轮等待
            case NET_CMD_RELOAD_CONFIG:
                // 从 NVS 重载(门户刚写入),同步 has_config 供 UI/页面判断阶段。
                if (app_storage_load_netlist(&s_list)) {
                    portENTER_CRITICAL(&s_lock);
                    s_status.has_config = s_list.count > 0;
                    portEXIT_CRITICAL(&s_lock);
                    ESP_LOGI(TAG, "已重载热点列表:%u 个", (unsigned)s_list.count);
                }
                break;
            default:
                break;
            }
            continue;
        }

        // ---- 1 秒周期维护 ----
        portENTER_CRITICAL(&s_lock);
        bool portal = s_status.portal_active;
        int close_s = s_status.portal_close_s;
        bool online = (s_status.state == APP_NET_ONLINE);
        portEXIT_CRITICAL(&s_lock);

        // 在线探活:状态声称在线,但驱动报告未关联 → 事件可能丢帧(如休眠
        // 期间),按掉线处理。esp_wifi_sta_get_ap_info 未关联时返回非 OK。
        if (online) {
            wifi_ap_record_t rec;
            if (esp_wifi_sta_get_ap_info(&rec) != ESP_OK) {
                s_expected_up = false;
                set_ip("");
                set_state(APP_NET_OFFLINE_RETRY);
                ESP_LOGW(TAG, "探活失败:STA 已断开,开始自动重连");
                if (!try_saved_round()) {
                    set_state(APP_NET_OFFLINE_RETRY);
                    last_retry = xTaskGetTickCount();
                }
                online = (s_status.state == APP_NET_ONLINE);
            }
        }

        // AP 自动关闭倒计时:任何路径连上网络(点选/自动回退)都应启动,
        // 否则配网横幅和热点会一直挂着(实测踩坑:自动连接路径漏了倒计时)。
        if (portal && online && close_s < 0) {
            portENTER_CRITICAL(&s_lock);
            s_status.portal_close_s = PORTAL_CLOSE_DELAY_S;
            portEXIT_CRITICAL(&s_lock);
        }
        // AP 自动关闭倒计时(点选连接成功后的过渡窗口)。
        if (portal && close_s > 0) {
            portENTER_CRITICAL(&s_lock);
            s_status.portal_close_s--;
            int now = s_status.portal_close_s;
            portEXIT_CRITICAL(&s_lock);
            if (now == 0) portal_ap_stop();
        }
        // 掉线重试:整轮失败后每 RETRY_GAP_MS 再试一轮。
        if (!s_expected_up && !online && s_list.count > 0 && !portal &&
            (xTaskGetTickCount() - last_retry) >= pdMS_TO_TICKS(RETRY_GAP_MS)) {
            last_retry = xTaskGetTickCount();
            ESP_LOGI(TAG, "重试已保存热点…");
            if (try_saved_round()) continue;
            set_state(APP_NET_OFFLINE_RETRY);
        }
        // 在线时刷新 RSSI 供 UI 展示(探活成功后顺带读)。
        if (online) {
            wifi_ap_record_t rec;
            if (esp_wifi_sta_get_ap_info(&rec) == ESP_OK) {
                portENTER_CRITICAL(&s_lock);
                s_status.rssi = rec.rssi;
                portEXIT_CRITICAL(&s_lock);
            }
        }
    }
    vTaskDelete(NULL);
}

// ------------------------------------------------------------------ 公开 API

void app_net_get_status(app_net_status_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_lock);
    *out = s_status; // 结构 ~1.6KB,临界区拷贝仍在微秒级;调用频率低(LVGL 250ms/HTTP 按需)
    portEXIT_CRITICAL(&s_lock);
}

static bool post_cmd(net_msg_t *msg)
{
    return s_queue && xQueueSend(s_queue, msg, 0) == pdTRUE;
}

void app_net_start_portal(void)
{
    net_msg_t m = { .id = NET_CMD_PORTAL_ON };
    post_cmd(&m);
}

void app_net_stop_portal(void)
{
    net_msg_t m = { .id = NET_CMD_PORTAL_OFF };
    post_cmd(&m);
}

void app_net_scan(void)
{
    net_msg_t m = { .id = NET_CMD_SCAN };
    post_cmd(&m);
}

void app_net_connect_saved(void)
{
    net_msg_t m = { .id = NET_CMD_CONNECT_SAVED };
    post_cmd(&m);
}

void app_net_reload_config(void)
{
    net_msg_t m = { .id = NET_CMD_RELOAD_CONFIG };
    post_cmd(&m);
}

void app_net_connect_ssid(const char *ssid)
{
    net_msg_t m = { .id = NET_CMD_CONNECT_SSID };
    snprintf(m.ssid, sizeof(m.ssid), "%s", ssid ? ssid : "");
    post_cmd(&m);
}

int app_net_init(const app_netlist_t *list, bool open_portal_on_no_config)
{
    if (s_wifi_init) return ESP_ERR_INVALID_STATE;

    s_list = *list; // 拷贝一份任务私有列表:连接回退在任务内读写,读侧只看快照
    memset(&s_status, 0, sizeof(s_status));
    s_status.portal_close_s = -1;

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    // 用带检查的创建步骤:便捷 API 失败时会 abort,而这里是应用启动路径,要能回滚。
    esp_netif_config_t sta_cfg = ESP_NETIF_DEFAULT_WIFI_STA();
    s_sta_netif = esp_netif_new(&sta_cfg);
    if (!s_sta_netif) return ESP_ERR_NO_MEM;
    err = esp_netif_attach_wifi_station(s_sta_netif);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_default_wifi_sta_handlers();
    if (err != ESP_OK) return err;

    esp_netif_config_t ap_cfg = ESP_NETIF_DEFAULT_WIFI_AP();
    s_ap_netif = esp_netif_new(&ap_cfg);
    if (!s_ap_netif) return ESP_ERR_NO_MEM;
    err = esp_netif_attach_wifi_ap(s_ap_netif);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_default_wifi_ap_handlers();
    if (err != ESP_OK) return err;

    wifi_init_config_t wi = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wi);
    if (err != ESP_OK) return err;
    s_wifi_init = true;

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              on_wifi_event, NULL, &s_evt_any);
    if (err != ESP_OK) return err;
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              on_ip_event, NULL, &s_evt_any);
    if (err != ESP_OK) return err;

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM); // 凭据由我们自己的 NVS 管,避免双份
    if (err != ESP_OK) return err;
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) return err;

    // 配网 AP 名:前缀 + MAC 尾两字节,保证多设备不重名。
    uint8_t mac[6] = { 0 };
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char ap_ssid[APP_NET_SSID_LEN];
    snprintf(ap_ssid, sizeof(ap_ssid), "%s%02X%02X", APP_NET_AP_PREFIX, mac[4], mac[5]);
    portENTER_CRITICAL(&s_lock);
    strlcpy(s_status.ap_ssid, ap_ssid, sizeof(s_status.ap_ssid));
    s_status.has_config = s_list.count > 0;
    s_status.portal_close_s = -1;
    portEXIT_CRITICAL(&s_lock);

    s_events = xEventGroupCreate();
    s_queue = xQueueCreate(8, sizeof(net_msg_t));
    if (!s_events || !s_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(net_task, "app_net", 5120, NULL, 5, &s_task) != pdPASS)
        return ESP_ERR_NO_MEM;

    err = esp_wifi_start();
    if (err != ESP_OK) return err;

    bool open_portal = (s_list.count == 0) && open_portal_on_no_config;
    if (open_portal) {
        portal_ap_start();
    } else if (s_list.count > 0) {
        app_net_connect_saved();
    }
    return ESP_OK;
}
