// main/main.c —— GLM 用量宝(appfw 框架版)启动装配。
//
// 应用只负责:1) 框架初始化顺序;2) 注入业务(用量主页 / 门户 GLM 卡片与端点)。
// 全部通用能力(WiFi 引擎 / 配网门户 / 存储 / UI 骨架 / 熄屏 / 按键 / MCP / BSP)
// 来自 components/framework(框架 submodule),见 docs/migration-to-framework.zh_CN.md。
//
// 按键约定(框架默认):主页 下=设置菜单,上=立即刷新用量,OK 单击=熄屏;
// 菜单/子页=光标行(上下移,OK 执行)。任意键唤醒熄屏。
#include <stdbool.h>

#include "app_glm_client.h"
#include "appfw_mcp.h"
#include "appfw_net.h"
#include "appfw_netlist.h"
#include "appfw_portal.h"
#include "appfw_storage.h"
#include "appfw_ui.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "app_glm_mcp.h"
#include "app_home.h"
#include "app_portal_glm.h"

static const char *TAG = "main";

// ---- 输入任务(框架提供事件规整与栈预算,应用只给回调) ----
static QueueHandle_t s_key_queue;
static volatile bool s_keys_ready;

static void on_key_from_bsp(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (!s_keys_ready || !s_key_queue) return;
    const int msg = (int)btn | ((int)ev << 4);
    (void)xQueueSend(s_key_queue, &msg, 0);
}

static void key_task(void *arg)
{
    (void)arg;
    int msg;
    for (;;) {
        if (xQueueReceive(s_key_queue, &msg, portMAX_DELAY) == pdTRUE) {
            appfw_ui_on_key(msg & 0xF, (msg >> 4) & 0xF);
        }
    }
}

static void second_tick_cb(void *arg)
{
    (void)arg;
    appfw_ui_second_tick();
}

// 设置菜单显示哪些框架自带项(与 MCP 挂载的基础工具同一份配置)。
// 用量刷新周期有意义,保留;日志页暂不开(netlog 未初始化)。
#define APP_MENU_SHOW_MASK (APPFW_MENU_ITEM_REFRESH_PERIOD | \
                            APPFW_MENU_ITEM_SCREEN_OFF | \
                            APPFW_MENU_ITEM_WIFI_MANAGER | \
                            APPFW_MENU_ITEM_DEVICE_INFO | \
                            APPFW_MENU_ITEM_PROVISIONING | \
                            APPFW_MENU_ITEM_AI_ADMIN | \
                            APPFW_MENU_ITEM_BRIGHTNESS)

void app_main(void)
{
    ESP_LOGI(TAG, "GLM 用量宝(appfw)启动");

    // 1) I2C(电量计用;本应用不用音频)。失败不阻塞,电量显示降级。
    bsp_i2c_init();
    if (bsp_battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "电量计未就绪,右上角电量将显示 --");
    }

    // 2) 显示 + LVGL。失败则应用没有可用输出,打日志后退出。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,应用无法继续");
        return;
    }
    bsp_display_backlight(100);

    // 3) 配置存储:框架("appfw" 命名空间:热点/周期/熄屏/亮度)先行。
    if (appfw_store_init() != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败(配置将无法保存)");
    }

    // 4) 无线栈:有已保存热点就自动回退连接;配网从设置菜单手动进入
    //    (菜单 → 配网 → 开启热点),设备不自动开门户。
    appfw_netlist_t list;
    if (!appfw_store_netlist_load(&list)) {
        appfw_netlist_reset(&list);
    }
    const int net_err = appfw_net_init(&list, false);
    if (net_err != 0) {
        ESP_LOGW(TAG, "WiFi 初始化返回 %d", net_err);
    }

    // 5) 用量查询任务(内部自等联网/对时)。
    if (app_glm_client_start() != ESP_OK) {
        ESP_LOGE(TAG, "用量任务启动失败");
    }

    // 6) 按键(失败不阻塞:只是失去手动刷新/菜单入口)。
    s_key_queue = xQueueCreate(8, sizeof(int));
    if (s_key_queue &&
        xTaskCreate(key_task, "app_input", 6144, NULL, 5, NULL) == pdPASS &&
        bsp_button_init(on_key_from_bsp, NULL) == ESP_OK) {
        s_keys_ready = true;
    } else {
        ESP_LOGE(TAG, "按键初始化失败");
    }

    // 7) AI 管理(MCP 常驻服务):应用工具(GLM 配置/用量)+ 框架基础工具
    //    (跟随菜单使能位)。配好 WiFi 后,Key/团队/用量全交给 AI。
    glm_mcp_init();
    appfw_mcp_set_builtin_tools(APP_MENU_SHOW_MASK);
    appfw_mcp_server_start();

    // 8) 主页(UI 构建必须持 LVGL 锁;轮询定时器由框架在内部建)。
    const appfw_ui_cfg_t ucfg = {
        .home_title = "GLM 用量宝",
        .home_build = app_home_build,
        .home_poll  = app_home_poll,
        .home_up    = app_home_refresh_now,   // 上键:立即刷新
        .config_rows = app_home_config_rows,
        .menu_show_mask = APP_MENU_SHOW_MASK,
        // 内置菜单默认入口:下键进设置菜单(与旧版交互一致)。
        .menu_open_btn = 0,
        .page_reset = app_home_page_reset,
    };
    if (bsp_lvgl_lock(1000)) {
        appfw_ui_init(&ucfg);
        bsp_lvgl_unlock();
        s_keys_ready = true;
    } else {
        ESP_LOGE(TAG, "LVGL 锁获取失败,界面未创建");
    }

    // 9) 配网门户注入(GLM 卡片 + 私有端点)。门户按需启停:配网页「开启热点」
    //    时由框架拉起;联网后经局域网 IP 访问(设置菜单「AI 管理」页看地址)。
    appfw_prov_cfg_t pcfg = { 0 };
    app_portal_glm_configure(&pcfg);
    appfw_prov_configure(&pcfg);

    // 10) 秒级维护(门户拉活/DNS 收撤 + 熄屏判定)。
    esp_timer_handle_t tick;
    const esp_timer_create_args_t ta = { .callback = second_tick_cb, .name = "tick" };
    if (esp_timer_create(&ta, &tick) == ESP_OK) {
        esp_timer_start_periodic(tick, 1000000);
    }

    ESP_LOGI(TAG, "启动完成:net=%d", net_err);
}
