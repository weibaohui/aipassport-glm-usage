// main/main.c —— GLM 用量宝 应用入口(基于 FoloToy AI Passport BSP 的二次开发)。
//
// 应用需求:设备启动 → 无已保存配置时自动进入配网模式(自建热点,电脑连接后
// 在 192.168.4.1 填 API Key、扫描并保存多个热点、点选连接);联网成功后每 60 秒
// 查询 GLM Coding Plan 用量并显示在屏幕上。
//
// 按键交互(应用自定义,与基线 demo 菜单无关):
//   上/下 单击 = 用量页 / 网络页切换;OK 单击 = 立即刷新;OK 长按 = 进入配网。
//
// 启动顺序(强约束):
//   NVS → 显示/LVGL → 无线栈 → 查询任务 → 按键 → 首页。
//   显示失败则无法继续(本应用的一切输出都在屏幕上);其余子系统失败不阻塞,
//   界面会把降级状态画出来。
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "app_glm_client.h"
#include "app_net.h"
#include "app_portal.h"
#include "app_storage.h"
#include "app_ui.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "main";

#define INPUT_QUEUE_DEPTH 8

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

static QueueHandle_t s_input_queue;
static TaskHandle_t s_input_task;
static volatile bool s_input_ready;

// 按键回调运行在 button 组件的共享 esp_timer 任务上:只入队,立即返回。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (!s_input_ready || !s_input_queue) return;
    const input_event_t input = { .btn = btn, .event = ev };
    (void)xQueueSend(s_input_queue, &input, 0);
}

// 按键事件消费任务:串行化处理,UI 内部再按需转发到网络/查询任务。
//
// 事件规整(button 组件一次物理动作会发多个事件):
//   PRESS   丢弃 —— 应用只响应 CLICK/DOUBLE/LONG,PRESS 是游戏类即时响应用的,
//           之前把它也映射成"单击"导致 OK 熄屏后被抬起事件立刻唤醒(实测踩坑);
//   CLICK   → 0;DOUBLE → 2;LONG → 3。
//   长按后紧跟的 CLICK 抑制 —— LONG 触发动作后,抬起时的 CLICK 不是新按键意图
//   (比如长按 OK 进配网后不该再把屏幕熄掉),同键 500ms 内的 CLICK 直接吞掉。
static void input_task(void *arg)
{
    (void)arg;
    input_event_t input;
    int suppress_btn = -1;          // 待抑制 CLICK 的键;-1=无
    TickType_t suppress_until = 0;  // 抑制窗截止时刻
    for (;;) {
        if (xQueueReceive(s_input_queue, &input, portMAX_DELAY) != pdTRUE) continue;
        if (input.event == BSP_BTN_PRESS) continue; // 见上:PRESS 全部丢弃

        // 长按后的 CLICK 抑制窗检查。
        if (input.event == BSP_BTN_CLICK && input.btn == suppress_btn &&
            xTaskGetTickCount() < suppress_until) {
            suppress_btn = -1;
            continue;
        }

        int ev;
        switch (input.event) {
        case BSP_BTN_LONG:  ev = 3; break;
        case BSP_BTN_DOUBLE: ev = 2; break;
        default:            ev = 0; break; // CLICK
        }
        if (input.event == BSP_BTN_LONG) {
            // 记录抑制窗:同键的 CLICK 在长按后一个抖动周期内视为抬手残波。
            suppress_btn = (int)input.btn;
            suppress_until = xTaskGetTickCount() + pdMS_TO_TICKS(500);
        }
        app_ui_on_key((int)input.btn, ev);
    }
}

static esp_err_t input_dispatch_init(void)
{
    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (!s_input_queue) return ESP_ERR_NO_MEM;
    // 栈 6KB:按键路径会做页面重建(LVGL 建控件)+ 读 NVS(热点列表 blob),
    // 3KB 时进 WiFi 管理页会栈溢出重启(实测踩坑)。
    if (xTaskCreate(input_task, "app_input", 6144, NULL, 5, &s_input_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

// 门户心跳:根据设备 AP 状态启停 HTTP/DNS。esp_timer 回调上下文,app_ui_portal_tick
// 内部只做状态检查与 socket/httpd 启停,无 LVGL 调用,安全。
static void portal_tick_cb(void *arg)
{
    (void)arg;
    app_ui_portal_tick();
}

void app_main(void)
{
    ESP_LOGI(TAG, "GLM 用量宝 启动");

    // 1) I2C(电量计用;音频不用,不初始化 codec)。失败不阻塞,电量显示降级。
    bsp_i2c_init();
    if (bsp_battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "电量计未就绪,右上角电量将显示 --");
    }

    // 2) 显示 + LVGL。失败则应用没有可用输出,打日志后退出(用户可看串口)。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,应用无法继续");
        return;
    }
    bsp_display_backlight(100);

    // 3) NVS(配置承载)。失败则视为全新设备:仍能开配网,但保存会失败,
    //    门户的保存接口会把 ok:false 返回给网页,用户可见。
    int st_err = app_storage_init();
    if (st_err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败:%s", esp_err_to_name(st_err));
    }

    // 4) 无线栈:有已保存热点就自动回退连接;配网统一从设置菜单进
    //    (菜单 → 配网 → 开启),不再开机自动开门户。
    app_netlist_t list;
    if (!app_storage_load_netlist(&list)) {
        app_netlist_reset(&list);
    }
    int net_err = app_net_init(&list, false);
    if (net_err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi 初始化失败:%s — 无法联网,界面将显示等待状态",
                 esp_err_to_name(net_err));
    }

    // 5) 用量查询任务(内部自等联网/对时)。
    int glm_err = app_glm_client_start();
    if (glm_err != ESP_OK) {
        ESP_LOGE(TAG, "用量任务启动失败:%s", esp_err_to_name(glm_err));
    }

    // 6) 按键(失败不阻塞:只是失去手动刷新/切页/配网入口)。
    esp_err_t input_err = input_dispatch_init();
    esp_err_t btn_err = input_err == ESP_OK ? bsp_button_init(on_key, NULL)
                                            : ESP_ERR_INVALID_STATE;
    if (btn_err != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败:%s", esp_err_to_name(btn_err));
        if (s_input_queue) { vQueueDelete(s_input_queue); s_input_queue = NULL; }
        s_input_task = NULL;
    }

    // 7) 管理门户(常驻):配网期经 192.168.4.1,联网后经局域网 IP 访问。
    if (!app_portal_start()) {
        ESP_LOGE(TAG, "管理门户启动失败");
    }

    // 8) 门户心跳定时器(1s):兜底拉活 + AP 关闭后撤 DNS。
    const esp_timer_create_args_t tick_args = {
        .callback = portal_tick_cb,
        .name = "portal_tick",
    };
    esp_timer_handle_t tick_timer;
    if (esp_timer_create(&tick_args, &tick_timer) == ESP_OK) {
        esp_timer_start_periodic(tick_timer, 1000000); // 1s
    }

    // 9) 首页。UI 构建必须持 LVGL 锁;此后轮询定时器都在 LVGL 任务内自转。
    if (bsp_lvgl_lock(1000)) {
        app_ui_init();
        bsp_lvgl_unlock();
        s_input_ready = true;
    } else {
        ESP_LOGE(TAG, "LVGL 锁获取失败,界面未创建");
    }

    ESP_LOGI(TAG, "启动完成:storage=%s net=%s glm=%s btn=%s",
             st_err == ESP_OK ? "ok" : esp_err_to_name(st_err),
             net_err == ESP_OK ? "ok" : esp_err_to_name(net_err),
             glm_err == ESP_OK ? "ok" : esp_err_to_name(glm_err),
             btn_err == ESP_OK ? "ok" : esp_err_to_name(btn_err));
}
