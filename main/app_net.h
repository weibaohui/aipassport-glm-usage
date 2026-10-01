// main/app_net.h —— WiFi 引擎:STA 联网 + 配网 SoftAP(APSTA)+ 扫描,单一所有者任务。
//
// 设计要点:
// - 全部 WiFi 阻塞操作(扫描/连接/模式切换)都收敛在 app_net 内部任务里串行执行,
//   按键回调、HTTP 处理器、LVGL 任务只通过 app_net_* 请求函数投递命令,绝不直接
//   调 esp_wifi_* —— 避免多任务竞争和回调阻塞。
// - 状态用自旋锁保护的快照发布(app_net_get_status / app_net_get_scan),供 UI
//   定时器与门户查询;快照都是短小拷贝,临界区在微秒级。
// - 连接回退顺序由 app_netlist_next_target 定义:点选优先,其后按保存顺序轮转。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_netlist.h"

#define APP_NET_SCAN_MAX 20          // 门户展示的扫描结果上限(按 RSSI 排序取前 N)
#define APP_NET_SSID_LEN 33          // 与 802.11 SSID 上限一致(32+NUL)
#define APP_NET_IP_LEN 16            // "255.255.255.255" + NUL
#define APP_NET_AP_PREFIX "GLM-Meter-" // 配网 AP 名前缀,后接 MAC 尾 4 个 hex

typedef enum {
    APP_NET_IDLE = 0,      // WiFi 已启动,未在连接
    APP_NET_SCANNING,      // 正在扫描
    APP_NET_CONNECTING,    // 正在连接某个热点(autofail 或点选)
    APP_NET_ONLINE,        // 已拿到 IP,可联网
    APP_NET_OFFLINE_RETRY, // 掉线/连败,正在按列表重试
} app_net_state_t;

typedef struct {
    char ssid[APP_NET_SSID_LEN];     // AP 名称(NUL 结尾,可能含中文/空格)
    int8_t rssi;                     // 信号强度 dBm(负值,越大越好)
    bool auth;                       // 是否加密(门户据此提示)
} app_net_scan_item_t;

typedef struct {
    app_net_state_t state;           // 当前状态机状态
    bool portal_active;              // 配网 AP 是否已开启
    bool has_config;                 // 是否已有已保存热点(决定启动走向)
    char cur_ssid[APP_NET_SSID_LEN]; // 正在连/已连的热点;IDLE 时为空
    char ip[APP_NET_IP_LEN];         // STA IP;未联网为空串
    int8_t rssi;                     // 已连 AP 的信号;未知为 0
    char ap_ssid[APP_NET_SSID_LEN];  // 配网 AP 名称(始终可显示)
    int portal_close_s;              // 联网成功后 AP 自动关闭倒计时秒;不倒计时为 -1
    uint32_t scan_seq;               // 扫描结果代数:每次新扫描 +1,门户据此刷新
    uint8_t scan_count;              // 有效扫描条数 0..APP_NET_SCAN_MAX
    app_net_scan_item_t scan[APP_NET_SCAN_MAX];
} app_net_status_t;

// 初始化 NVS 之外的无线栈(netif/事件循环/WiFi 驱动/内部任务),并启动 STA。
// list:已保存热点(可为空表);open_portal_on_no_config:空表时立即自动开配网。
// 成功后状态为 IDLE(空表+开配网时为 PORTAL)。返回 0 成功,否则 ESP_ERR 码。
int app_net_init(const app_netlist_t *list, bool open_portal_on_no_config);

// 请求开启配网 AP + DNS + HTTP 门户(命令投递,立即返回)。
void app_net_start_portal(void);

// 请求关闭配网 AP(回到纯 STA)。
void app_net_stop_portal(void);

// 门户刚保存过配置:让 net 任务重新加载 NVS 里的热点列表(任务内的副本
// 平时不动,避免门户/NVS 与连接状态竞争)。保存/删除热点后必须调用。
void app_net_reload_config(void);

// 请求扫描周边热点(结果写进状态快照,scan_seq 递增)。
void app_net_scan(void);

// 按已保存列表自动连接(先点选项,再按保存顺序轮转;全部失败进入重试态)。
void app_net_connect_saved(void);

// 点选连接已保存列表中的指定热点(门户"连接"按钮)。列表中没有该 SSID 则忽略。
void app_net_connect_ssid(const char *ssid);

// 读取状态快照(内部自旋锁,输出参数整体拷贝)。任一指针为 NULL 跳过对应拷贝。
void app_net_get_status(app_net_status_t *out);
