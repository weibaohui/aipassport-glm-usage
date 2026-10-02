// main/app_ui.c —— 界面实现,布局与交互契约见 app_ui.h。
//
// 视觉:深色底(#0E1116)+ 绿色强调(#35C26B),与基线 demo 的像素纸风完全不同;
// 仅用 LVGL 基础控件(label/bar),不使用图片素材,便于小内存设备复用。
//
// 状态机:
//   UI_MAIN  主页面(用量页/网络页,UP/DOWN 切换)
//   UI_MENU  设置菜单(下键单击进入;UP/DOWN 选择,OK 进子页)
//   UI_SUB_* 设置子页(刷新周期/熄屏时间/WiFi 管理/设备信息)
//
// 线程模型(关键):
//   - 按键处理在 input 任务上下文:所有 UI 修改都在 bsp_lvgl_lock() 保护下进行
//     (BSP 明确允许非 LVGL 任务持锁操作 lv_*);副作用(熄屏/刷新/连接请求)在
//     解锁后执行,避免持锁做慢操作。
//   - LVGL 轮询定时器运行于 LVGL 任务上下文,负责动态数据(用量/电量/告警)。
//   - 页内容在进入一个状态时整体重建;重建只发生在持锁路径。
#include "app_ui.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_glm_client.h"
#include "app_net.h"
#include "app_portal.h"
#include "app_storage.h"
#include "bsp_battery.h"
#include "bsp_button.h" // 复用其枚举值做键语义(见 app_ui_on_key 入参约定)
#include "bsp_display.h"
#include "esp_app_desc.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

// ---- 应用字体(生成于 assets/fonts/,见该目录 README;中文+ASCII 子集) ----
LV_FONT_DECLARE(app_font_16);
LV_FONT_DECLARE(app_font_24);

// 描述符浅拷贝 + fallback:子集缺的字符(生僻 SSID)退到 Montserrat,
// 仍缺则由 LVGL 占位符(□)显式提示 —— 不做静默丢弃。
static lv_font_t s_font16;
static lv_font_t s_font24;

#define COL_BG 0x0E1116
#define COL_TEXT 0xE6E6E6
#define COL_DIM 0x8B98A5
#define COL_OK 0x35C26B
#define COL_WARN 0xE5A13D
#define COL_BAD 0xE5484D
#define COL_BAR 0x24303C
#define COL_CARD 0x171C24
#define COL_SEL_BG 0x1D4030   // 选中行底色(绿色暗调)
#define COL_TITLE 0xF2F5F7

typedef enum {
    UI_MAIN = 0,   // 主页面(用量/网络)
    UI_MENU,       // 设置菜单
    UI_SUB_REFRESH,// 刷新周期
    UI_SUB_SOFF,   // 熄屏时间
    UI_SUB_WIFI,   // WiFi 管理
    UI_SUB_PROV,   // 配网(热点开关与状态)
    UI_SUB_INFO,   // 设备信息
} ui_state_t;

typedef struct {
    lv_obj_t *page;         // 当前页容器(状态切换时整体重建)
    lv_obj_t *battery;      // 右上角电量
    lv_obj_t *warn;         // 右上角网络告警 ⚠
    // 用量页控件(仅 UI_MAIN+用量页有效)
    lv_obj_t *week_bar, *week_pct, *week_reset;
    lv_obj_t *h5_bar, *h5_pct, *h5_reset;
    lv_obj_t *mcp_val, *mcp_label, *foot;
    // 配网横幅(仅主页面构建)
    lv_obj_t *portal;
    // 菜单/子页行控件(高亮刷新用)
    lv_obj_t *rows[8];
    int row_count;
} ui_t;

static ui_t s_ui;
static ui_state_t s_state = UI_MAIN; // UI 状态(input 持锁写,轮询读;enum 对齐访问)
static int s_menu_sel;               // 菜单光标
static int s_opt_sel;                // 子页选项光标
static int s_wifi_sel;               // WiFi 页光标
static int s_prov_sel;               // 配网页光标(0=开关,1=返回)
static lv_obj_t *s_scr;
static uint32_t s_last_input_ms;     // 最近按键时刻(保留:显示层用)
static int64_t s_last_input_us;      // 最近交互时刻(esp_timer 微秒;熄屏判定用)
static atomic_bool s_screen_off;     // 面板睡眠中(portal_tick 熄屏 / input_task 唤醒)
static int s_prefs_age;              // portal_tick 循环计数:周期性重读用户偏好
static char s_key_mask[24];          // "abcd…wxyz" 掩码(网络页/信息页重建时刷新)
static char s_saved_list_text[112];  // 网络页配置摘要(组织/项目/刷新/熄屏,构建时生成)
static char s_wifi_cache[APP_NETLIST_MAX][APP_NETLIST_SSID_MAX]; // WiFi 页条目快照
static int s_wifi_cache_n;           // 快照条数

static lv_obj_t *s_toast;         // 底部吐司(挂在 screen 上,跨页面)
static lv_timer_t *s_toast_timer; // 吐司自动隐藏定时器

// ---- 选项表(子页) ----
static const uint16_t REFRESH_OPTS[] = { 60, 300, 600, 900, 1800, 3600 };
static const char *REFRESH_LBL[] = { "1 分钟", "5 分钟", "10 分钟", "15 分钟", "30 分钟", "1 小时" };
#define REFRESH_N 6
static const uint16_t SOFF_OPTS[] = { 60, 300, 600, 900, 1800, 0 };
static const char *SOFF_LBL[] = { "1 分钟", "5 分钟", "10 分钟", "15 分钟", "30 分钟", "永不" };
#define SOFF_N 6

static const char *MENU_LBL[] = {
    LV_SYMBOL_REFRESH "  刷新周期",
    LV_SYMBOL_BELL "  熄屏时间",
    LV_SYMBOL_WIFI "  WiFi 管理",
    LV_SYMBOL_LIST "  设备信息",
    LV_SYMBOL_HOME "  配网",
    LV_SYMBOL_LEFT "  返回",
};
#define MENU_N 6

// ---------------------------------------------------------------- 工具

static void style_label(lv_obj_t *l, const lv_font_t *f, uint32_t color)
{
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
}

// 用量条:轨道深灰、指示条按用量变色(>90% 红、>70% 橙,其余绿)。
static lv_obj_t *make_bar(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_bar_set_range(bar, 0, 100);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_BAR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, h / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_OK), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, h / 2, LV_PART_INDICATOR);
    return bar;
}

static void set_bar_pct(lv_obj_t *bar, int pct)
{
    // pct<0 表示未知:条置 0,由旁边文本显示 "--"。
    lv_bar_set_value(bar, pct > 0 ? pct : 0, LV_ANIM_OFF);
    uint32_t col = COL_OK;
    if (pct >= 90) col = COL_BAD;
    else if (pct >= 70) col = COL_WARN;
    lv_obj_set_style_bg_color(bar, lv_color_hex(col), LV_PART_INDICATOR);
}

static void set_pct_text(lv_obj_t *l, int pct)
{
    if (pct < 0) lv_label_set_text(l, "--");
    else lv_label_set_text_fmt(l, "%d%%", pct);
}

// API Key 掩码(abcd…wxyz;不足 8 位只显示前 2 位;无 Key="未设置")。
// 不能只在开机时生成一次:门户保存 Key 后屏幕不会重启,页面每次重建时调用。
static void refresh_key_mask(void)
{
    char key[APP_STORAGE_API_KEY_MAX];
    if (app_storage_load_api_key(key, sizeof(key))) {
        size_t n = strlen(key);
        if (n >= 8) snprintf(s_key_mask, sizeof(s_key_mask), "%.4s…%.4s", key, key + n - 4);
        else snprintf(s_key_mask, sizeof(s_key_mask), "%.2s…", key);
    } else {
        snprintf(s_key_mask, sizeof(s_key_mask), "未设置");
    }
}

// ---------------------------------------------------------------- 吐司

// 底部吐司:圆角药丸,1.2s 自动消失。toast_timer_cb 由 LVGL 任务触发。
static void toast_timer_cb(lv_timer_t *t)
{
    if (s_toast) lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_timer_del(t);
    s_toast_timer = NULL;
}

// 必须持 bsp_lvgl_lock() 调用。
static void show_toast(const char *text)
{
    if (!s_toast) {
        s_toast = lv_label_create(s_scr);
        lv_obj_set_style_bg_color(s_toast, lv_color_hex(COL_CARD), 0);
        lv_obj_set_style_bg_opa(s_toast, LV_OPA_80, 0);
        lv_obj_set_style_radius(s_toast, 12, 0);
        lv_obj_set_style_pad_hor(s_toast, 12, 0);
        lv_obj_set_style_pad_ver(s_toast, 5, 0);
        style_label(s_toast, &s_font16, 0xFFFFFF);
        lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -34);
    }
    lv_label_set_text(s_toast, text);
    lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_toast);
    if (s_toast_timer) lv_timer_del(s_toast_timer);
    s_toast_timer = lv_timer_create(toast_timer_cb, 1200, NULL);
}

// ---------------------------------------------------------------- 页面构建

// 顶部栏:标题 + 强调下划线 + 右上角电量/告警(每个状态页都有,风格统一)。
static void build_top_bar(lv_obj_t *page, const char *title)
{
    lv_obj_t *t = lv_label_create(page);
    style_label(t, &s_font24, COL_TITLE);
    lv_label_set_text(t, title);
    lv_obj_set_pos(t, 12, 8);

    // 强调色下划线:标题下方 40×3 圆角短条,统一视觉锚点。
    lv_obj_t *ul = lv_obj_create(page);
    lv_obj_remove_style_all(ul);
    lv_obj_set_size(ul, 40, 3);
    lv_obj_set_pos(ul, 14, 40);
    lv_obj_set_style_bg_color(ul, lv_color_hex(COL_OK), 0);
    lv_obj_set_style_radius(ul, 2, 0);

    s_ui.battery = lv_label_create(page);
    style_label(s_ui.battery, &s_font16, COL_DIM);
    lv_obj_set_pos(s_ui.battery, 178, 14);
    lv_label_set_text(s_ui.battery, "--");

    // 网络告警图标:LVGL 内置符号(Montserrat 自带字形,不依赖中文字库)。
    // 断网/重连中显示红色 ⚠,恢复在线自动隐藏 —— 见 poll_timer_cb。
    s_ui.warn = lv_label_create(page);
    lv_obj_set_style_text_font(s_ui.warn, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_ui.warn, lv_color_hex(COL_BAD), 0);
    lv_obj_set_pos(s_ui.warn, 158, 15);
    lv_label_set_text(s_ui.warn, LV_SYMBOL_WARNING);
    lv_obj_add_flag(s_ui.warn, LV_OBJ_FLAG_HIDDEN);
}

// 菜单/子页通用行:圆角卡片;cursor=光标高亮(绿底描边)。symbol/文本由调用方定。
static lv_obj_t *make_row_h(lv_obj_t *page, int y, int h, bool cursor,
                            const char *symbol, const char *text)
{
    lv_obj_t *row = lv_obj_create(page);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 216, h);
    lv_obj_set_pos(row, 12, y);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(cursor ? COL_SEL_BG : COL_CARD), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    if (cursor) {
        lv_obj_set_style_border_color(row, lv_color_hex(COL_OK), 0);
        lv_obj_set_style_border_width(row, 1, 0);
    } else {
        lv_obj_set_style_border_width(row, 0, 0);
    }

    lv_obj_t *sym = lv_label_create(row);
    lv_obj_set_style_text_font(sym, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(sym, lv_color_hex(cursor ? COL_OK : COL_DIM), 0);
    lv_label_set_text(sym, symbol);
    lv_obj_align(sym, LV_ALIGN_LEFT_MID, 10, 0);

    lv_obj_t *lbl = lv_label_create(row);
    style_label(lbl, &s_font16, COL_TEXT);
    lv_label_set_text(lbl, text);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 32, 0);
    return row;
}

// 默认 40 高的便捷包装(菜单/返回行用)。
static lv_obj_t *make_row(lv_obj_t *page, int y, bool cursor,
                          const char *symbol, const char *text)
{
    return make_row_h(page, y, 40, cursor, symbol, text);
}

static void build_usage_page(lv_obj_t *page)
{
    // 行 1:本周 token 额度
    lv_obj_t *l = lv_label_create(page);
    style_label(l, &s_font16, COL_DIM);
    lv_label_set_text(l, "本周额度");
    lv_obj_set_pos(l, 14, 50);
    s_ui.week_pct = lv_label_create(page);
    style_label(s_ui.week_pct, &s_font24, COL_TEXT);
    lv_obj_set_pos(s_ui.week_pct, 172, 46);
    lv_label_set_text(s_ui.week_pct, "--");
    s_ui.week_bar = make_bar(page, 14, 82, 212, 12);
    s_ui.week_reset = lv_label_create(page);
    style_label(s_ui.week_reset, &s_font16, COL_DIM);
    lv_obj_set_pos(s_ui.week_reset, 14, 100);
    lv_label_set_text(s_ui.week_reset, "重置 --");

    // 行 2:5 小时窗口
    l = lv_label_create(page);
    style_label(l, &s_font16, COL_DIM);
    lv_label_set_text(l, "5小时窗口");
    lv_obj_set_pos(l, 14, 126);
    s_ui.h5_pct = lv_label_create(page);
    style_label(s_ui.h5_pct, &s_font24, COL_TEXT);
    lv_obj_set_pos(s_ui.h5_pct, 172, 122);
    lv_label_set_text(s_ui.h5_pct, "--");
    s_ui.h5_bar = make_bar(page, 14, 158, 212, 12);
    s_ui.h5_reset = lv_label_create(page);
    style_label(s_ui.h5_reset, &s_font16, COL_DIM);
    lv_obj_set_pos(s_ui.h5_reset, 14, 176);
    lv_label_set_text(s_ui.h5_reset, "重置 --");

    // 行 3:MCP 月度调用(个人)/剩余重置次数(团队),无数值语义时不画条。
    l = lv_label_create(page);
    style_label(l, &s_font16, COL_DIM);
    lv_label_set_text(l, "MCP 调用(每月)");
    lv_obj_set_pos(l, 14, 202);
    s_ui.mcp_label = l;
    s_ui.mcp_val = lv_label_create(page);
    style_label(s_ui.mcp_val, &s_font24, COL_TEXT);
    lv_obj_set_pos(s_ui.mcp_val, 140, 198);
    lv_label_set_text(s_ui.mcp_val, "--");

    // 状态行:套餐 + 刷新倒计时/错误(超长截尾)。
    s_ui.foot = lv_label_create(page);
    style_label(s_ui.foot, &s_font16, COL_DIM);
    lv_obj_set_width(s_ui.foot, 216);
    lv_label_set_long_mode(s_ui.foot, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_ui.foot, 14, 240);
    lv_label_set_text(s_ui.foot, "等待数据…");
}

static void build_menu(lv_obj_t *page)
{
    for (int i = 0; i < MENU_N; i++) {
        bool cursor = (i == s_menu_sel);
        lv_obj_t *row = make_row(page, 48 + i * 40, cursor, " ", MENU_LBL[i]);
        // 右侧箭头:提示"OK 进入"。
        lv_obj_t *arrow = lv_label_create(row);
        lv_obj_set_style_text_font(arrow, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(arrow, lv_color_hex(COL_DIM), 0);
        lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
        lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -10, 0);
        s_ui.rows[i] = row;
    }
    s_ui.row_count = MENU_N;

}

// 子页选项列表:cursor 高亮光标;当前已存值行前带绿色 ✓。
static void build_option_page(lv_obj_t *page, const uint16_t *opts,
                              const char **lbls, int n, uint16_t current)
{
    for (int i = 0; i < n; i++) {
        bool cursor = (i == s_opt_sel);
        bool is_current = (opts[i] == current);
        char text[40];
        snprintf(text, sizeof(text), "%s %s", is_current ? LV_SYMBOL_OK : " ", lbls[i]);
        s_ui.rows[i] = make_row_h(page, 46 + i * 36, 30, cursor,
                                  cursor ? LV_SYMBOL_RIGHT : " ", text);
    }
    // 返回行:最后一项,OK 即回菜单。
    s_ui.rows[n] = make_row(page, 46 + n * 36, s_opt_sel == n,
                            LV_SYMBOL_LEFT, "返回");
    s_ui.row_count = n + 1;
}

static void build_wifi_page(lv_obj_t *page)
{
    app_netlist_t list;
    bool have = app_storage_load_netlist(&list);
    s_wifi_cache_n = 0;
    if (have) {
        for (uint8_t i = 0; i < list.count; i++) {
            strncpy(s_wifi_cache[i], list.items[i].ssid, APP_NETLIST_SSID_MAX - 1);
            s_wifi_cache[i][APP_NETLIST_SSID_MAX - 1] = '\0';
            s_wifi_cache_n++;
        }
    }
    int total = s_wifi_cache_n + 1; // +1 = 返回行(索引 count)
    if (s_wifi_sel >= total) s_wifi_sel = total - 1;
    if (s_wifi_sel < 0) s_wifi_sel = 0;

    if (s_wifi_cache_n == 0) {
        lv_obj_t *empty = lv_label_create(page);
        style_label(empty, &s_font16, COL_DIM);
        lv_label_set_text(empty, "暂无已存热点\n可在网页配网时添加");
        lv_obj_set_pos(empty, 14, 70);
        s_ui.row_count = 0;
        return;
    }

    app_net_status_t st;
    app_net_get_status(&st);
    // 精确几何:行容器高 28(单行文本 22px),间距 30 → 相邻行 2px 间隙,不重叠。
    // 总高:46 + 8×30 + 30(返回) = 316 ≤ 320,一屏放下(已存热点上限 8)。
    for (int i = 0; i < s_wifi_cache_n; i++) {
        bool cursor = (i == s_wifi_sel);
        bool current = (strcmp(st.cur_ssid, s_wifi_cache[i]) == 0);
        char text[APP_NETLIST_SSID_MAX + 8];
        snprintf(text, sizeof(text), "%s %s",
                 current ? LV_SYMBOL_OK : " ", s_wifi_cache[i]);
        make_row_h(page, 46 + i * 30, 28, cursor,
                   cursor ? LV_SYMBOL_RIGHT : " ", text);
    }
    // 返回行:索引 = count,OK 即回菜单。
    make_row(page, 46 + s_wifi_cache_n * 30, s_wifi_sel == s_wifi_cache_n,
             LV_SYMBOL_LEFT, "返回");
}

// 配网子页:开关 + 热点名/管理地址/连接数。
static void build_prov_page(lv_obj_t *page)
{
    app_net_status_t st;
    app_net_get_status(&st);
    int y = 52;

    const struct { const char *k; char v[72]; } rows[] = {
        { "状态", { 0 } },
        { "热点", { 0 } },
        { "管理页", { 0 } },
        { "已连设备", { 0 } },
        { "本机 IP", { 0 } },
    };
    snprintf((char *)rows[0].v, sizeof(rows[0].v), "%s",
             st.portal_active ? "已开启" : "未开启");
    snprintf((char *)rows[1].v, sizeof(rows[1].v), "%s", st.ap_ssid);
    snprintf((char *)rows[2].v, sizeof(rows[2].v), "http://192.168.4.1");
    // 已连设备数:STA 列表接口在所有配置下都有原型;查询失败显示 --。
    wifi_sta_list_t sta_list;
    int sta_n = -1;
    if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) sta_n = (int)sta_list.num;
    snprintf((char *)rows[3].v, sizeof(rows[3].v),
             sta_n >= 0 ? "%d" : "--", sta_n);
    snprintf((char *)rows[4].v, sizeof(rows[4].v), "%s",
             st.ip[0] ? st.ip : "未连接");

    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        lv_obj_t *k = lv_label_create(page);
        style_label(k, &s_font16, COL_DIM);
        lv_label_set_text(k, rows[i].k);
        lv_obj_set_pos(k, 14, y);
        lv_obj_t *v = lv_label_create(page);
        style_label(v, &s_font16, COL_TEXT);
        lv_obj_set_width(v, 140);
        lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
        lv_label_set_text(v, rows[i].v);
        lv_obj_set_pos(v, 96, y);
        y += 30;
    }

    // 两行光标式(与其他子页一致):0=开关,1=返回;OK 执行光标所在行。
    s_ui.rows[0] = make_row(page, y + 8, s_prov_sel == 0, LV_SYMBOL_RIGHT,
                            st.portal_active ? "关闭配网" : "开启配网");
    s_ui.rows[1] = make_row(page, y + 58, s_prov_sel == 1, LV_SYMBOL_LEFT, "返回");
    s_ui.row_count = 2;

}


static void build_info_page(lv_obj_t *page)
{
    const esp_app_desc_t *app = esp_app_get_description();
    app_net_status_t st;
    app_net_get_status(&st);
    refresh_key_mask();
    char org[64] = { 0 }, proj[64] = { 0 };
    bool has_org = app_storage_load_org(org, sizeof(org));
    bool has_proj = app_storage_load_project(proj, sizeof(proj));
    uint16_t period_s = GLM_API_PERIOD_S, soff = 300;
    app_storage_load_period(&period_s);
    app_storage_load_screen_off(&soff);

    char soff_str[16];
    snprintf(soff_str, sizeof(soff_str), soff == 0 ? "永不" : "%u 分钟",
             (unsigned)(soff / 60));

    char url[48];
    snprintf(url, sizeof(url), "%s", st.ip[0] ? st.ip : "联网后可用");

    const struct { const char *k; char v[72]; } rows[] = {
        { "固件", { 0 } },
        { "管理地址", { 0 } },
        { "API Key", { 0 } },
        { "组织", { 0 } },
        { "项目", { 0 } },
        { "刷新 / 熄屏", { 0 } },
    };
    snprintf((char *)rows[0].v, sizeof(rows[0].v), "%s", app->version);
    snprintf((char *)rows[1].v, sizeof(rows[1].v), "http://%s", url);
    snprintf((char *)rows[2].v, sizeof(rows[2].v), "%s", s_key_mask);
    snprintf((char *)rows[3].v, sizeof(rows[3].v), "%s", has_org ? "已配置" : "未配置");
    snprintf((char *)rows[4].v, sizeof(rows[4].v), "%s", has_proj ? "已配置" : "未配置");
    snprintf((char *)rows[5].v, sizeof(rows[5].v), "%u 分钟 / %s",
             (unsigned)(period_s / 60), soff_str);

    int y = 54;
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        lv_obj_t *k = lv_label_create(page);
        style_label(k, &s_font16, COL_DIM);
        lv_label_set_text(k, rows[i].k);
        lv_obj_set_pos(k, 14, y);

        bool is_url = (i == 1); // 管理地址行:纯 ASCII,用小一号 Montserrat 防换行
        lv_obj_t *v = lv_label_create(page);
        style_label(v, is_url ? &lv_font_montserrat_14 : &s_font16, COL_TEXT);
        lv_obj_set_width(v, is_url ? 150 : 140);
        lv_obj_set_height(v, 20); // 固定行高:LONG_DOT 截尾,绝不挤到下一行
        lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
        lv_label_set_text(v, rows[i].v);
        lv_obj_set_pos(v, is_url ? 86 : 92, y);
        y += 27;
    }

    // 返回行:OK 即回菜单。
    make_row(page, y + 4, true, LV_SYMBOL_LEFT, "返回");
}

// 重建当前状态页:删除旧容器后按 s_state 重新构建。持锁调用。
static void rebuild_page(void)
{
    if (s_ui.page) {
        lv_obj_delete(s_ui.page);
        memset(&s_ui, 0, sizeof(s_ui));
    }
    s_ui.page = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_ui.page);
    lv_obj_set_size(s_ui.page, 240, 320);

    switch (s_state) {
    case UI_MAIN:
        build_top_bar(s_ui.page, "GLM 用量");
        build_usage_page(s_ui.page);
        // 配网横幅:仅主页面有。
        s_ui.portal = lv_label_create(s_ui.page);
        style_label(s_ui.portal, &s_font16, COL_WARN);
        lv_obj_set_width(s_ui.portal, 216);
        lv_label_set_long_mode(s_ui.portal, LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(s_ui.portal, 12, 292);
        lv_label_set_text(s_ui.portal, "");
        break;
    case UI_MENU:
        build_top_bar(s_ui.page, "设置");
        build_menu(s_ui.page);
        break;
    case UI_SUB_REFRESH: {
        build_top_bar(s_ui.page, "刷新周期");
        uint16_t cur = GLM_API_PERIOD_S;
        app_storage_load_period(&cur);
        build_option_page(s_ui.page, REFRESH_OPTS, REFRESH_LBL, REFRESH_N, cur);
        break;
    }
    case UI_SUB_SOFF: {
        build_top_bar(s_ui.page, "熄屏时间");
        uint16_t cur = 300;
        app_storage_load_screen_off(&cur);
        build_option_page(s_ui.page, SOFF_OPTS, SOFF_LBL, SOFF_N, cur);
        break;
    }
    case UI_SUB_WIFI:
        build_top_bar(s_ui.page, "WiFi 管理");
        build_wifi_page(s_ui.page);
        break;
    case UI_SUB_PROV:
        build_top_bar(s_ui.page, "配网");
        build_prov_page(s_ui.page);
        break;
    case UI_SUB_INFO:
        build_top_bar(s_ui.page, "设备信息");
        build_info_page(s_ui.page);
        break;
    }
}

// 光标移动后仅刷新行样式(不重建页面),输入路径下反馈即时。持锁调用。
static void refresh_rows_cursor(int sel)
{
    for (int i = 0; i < s_ui.row_count; i++) {
        lv_obj_t *row = s_ui.rows[i];
        if (!row) continue;
        bool cursor = (i == sel);
        lv_obj_set_style_bg_color(row, lv_color_hex(cursor ? COL_SEL_BG : COL_CARD), 0);
        if (cursor) lv_obj_set_style_border_width(row, 1, 0);
        else lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_t *sym = lv_obj_get_child(row, 0);
        if (sym) lv_obj_set_style_text_color(sym, lv_color_hex(cursor ? COL_OK : COL_DIM), 0);
    }
}

// ---------------------------------------------------------------- 数据刷新

static void update_usage_page(const glm_usage_t *u)
{
    set_bar_pct(s_ui.week_bar, u->tokens_week_used_pct);
    set_pct_text(s_ui.week_pct, u->tokens_week_used_pct);
    char buf[16];
    lv_label_set_text_fmt(s_ui.week_reset, "重置 %s",
                          glm_usage_format_reset_ms(u->tokens_week_reset_ms, buf, sizeof(buf)));

    set_bar_pct(s_ui.h5_bar, u->tokens_5h_used_pct);
    set_pct_text(s_ui.h5_pct, u->tokens_5h_used_pct);
    lv_label_set_text_fmt(s_ui.h5_reset, "重置 %s",
                          glm_usage_format_reset_ms(u->tokens_5h_reset_ms, buf, sizeof(buf)));

    if (u->mcp_total > 0) {
        lv_label_set_text(s_ui.mcp_label, "MCP 调用(每月)");
        lv_label_set_text_fmt(s_ui.mcp_val, "%d/%d", u->mcp_used, u->mcp_total);
    } else if (u->week_resets_left >= 0 && u->five_hour_resets_left >= 0) {
        lv_label_set_text(s_ui.mcp_label, "剩余重置");
        lv_label_set_text_fmt(s_ui.mcp_val, "周%d 5h%d",
                              u->week_resets_left, u->five_hour_resets_left);
    } else {
        lv_label_set_text(s_ui.mcp_label, "MCP 调用(每月)");
        lv_label_set_text(s_ui.mcp_val, "--");
    }
}

static void update_foot(const glm_usage_t *u, glm_err_t err, bool last_ok,
                        int64_t fetch_epoch_s)
{
    if (!last_ok && err != GLM_ERR_NONE) {
        const char *msg;
        // 服务端业务错误(u->msg 非空)优先原样展示:比笼统的"失败"更有用,
        // 例如"当前用户不存在coding plan"能直接指明账号状态。
        if (u->msg[0] != '\0' &&
            (err == GLM_ERR_AUTH || err == GLM_ERR_HTTP)) {
            char buf[80];
            snprintf(buf, sizeof(buf), "套餐接口:%s", u->msg);
            lv_label_set_text(s_ui.foot, buf);
            lv_obj_set_style_text_color(s_ui.foot, lv_color_hex(COL_BAD), 0);
            return;
        }
        switch (err) {
        case GLM_ERR_WAIT_NET: msg = "等待网络连接…"; break;
        case GLM_ERR_WAIT_TIME: msg = "正在对时…"; break;
        case GLM_ERR_AUTH: msg = "Key 无效,网页可改"; break;
        case GLM_ERR_PARSE: msg = "响应异常,稍后重试"; break;
        default: {
            char buf[48];
            int terr = app_glm_client_transport_err();
            if (terr != 0) {
                snprintf(buf, sizeof(buf), "查询失败(%x),稍后重试", terr);
                msg = buf;
            } else {
                msg = "查询失败,稍后重试";
            }
            break;
        }
        }
        lv_label_set_text(s_ui.foot, msg);
        lv_obj_set_style_text_color(s_ui.foot, lv_color_hex(COL_BAD), 0);
        return;
    }

    // 断网优先于一切:曾成功过也一样,网络没了就该说网络(而不是旧倒计时)。
    if (err == GLM_ERR_WAIT_NET) {
        lv_label_set_text(s_ui.foot, "网络已断开,自动重连中…");
        lv_obj_set_style_text_color(s_ui.foot, lv_color_hex(COL_BAD), 0);
        return;
    }

    // 正常:套餐 + 下轮倒计时 + 最近成功时间(未对时前显示等待文案)。
    // 周期是用户可配的(1~60 分钟),从 NVS 读,别写死。
    const char *plan = u->level[0] ? u->level : "--";
    if (fetch_epoch_s > 1000000000LL) { // 早于 2001 年的墙钟视为未对时
        uint16_t period_s = GLM_API_PERIOD_S;
        app_storage_load_period(&period_s);
        int64_t now = (int64_t)time(NULL);
        int left = (int)period_s - (int)(now - fetch_epoch_s);
        if (left < 0) left = 0;
        if (left > (int)period_s) left = period_s;
        char buf[16];
        glm_usage_format_reset_ms(fetch_epoch_s * 1000, buf, sizeof(buf));
        if (left >= 120) {
            lv_label_set_text_fmt(s_ui.foot, "套餐 %s · %d 分钟后刷新 · %s",
                                  plan, (left + 59) / 60, buf);
        } else {
            lv_label_set_text_fmt(s_ui.foot, "套餐 %s · %ds 后刷新 · %s", plan, left, buf);
        }
    } else {
        lv_label_set_text_fmt(s_ui.foot, "套餐 %s · 已连接,等待数据…", plan);
    }
    lv_obj_set_style_text_color(s_ui.foot, lv_color_hex(COL_DIM), 0);
}

// ---------------------------------------------------------------- 定时器

static void poll_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    // 1) 电量 + 网络告警:每轮取一次网络快照,多处共用。
    app_net_status_t net;
    app_net_get_status(&net);
    if (s_ui.battery) {
        int soc = bsp_battery_soc();
        if (soc >= 0) lv_label_set_text_fmt(s_ui.battery, "%d%%", soc);
        else lv_label_set_text(s_ui.battery, "--");
    }
    if (s_ui.warn) {
        // 非在线即告警(连接中/扫描/重试/空闲未连);在线隐藏。
        if (net.state != APP_NET_ONLINE) lv_obj_clear_flag(s_ui.warn, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_ui.warn, LV_OBJ_FLAG_HIDDEN);
    }

    // 2) 页面动态内容(仅主页面;菜单/子页为静态构建)。
    if (s_state == UI_MAIN && s_ui.week_bar) {
        glm_usage_t u;
        glm_err_t err;
        int64_t fetch_epoch_s;
        bool last_ok;
        app_glm_client_get_snapshot(&u, &err, &fetch_epoch_s, &last_ok);
        update_usage_page(&u);
        update_foot(&u, err, last_ok, fetch_epoch_s);
    }

    // 3) 配网横幅:仅主页面,AP 开着未联网时显示。
    if (s_state == UI_MAIN && s_ui.portal) {
        if (net.portal_active && net.state != APP_NET_ONLINE) {
            lv_label_set_text_fmt(s_ui.portal,
                                  "配网中:连接热点 %s,电脑打开 192.168.4.1",
                                  net.ap_ssid);
            lv_obj_clear_flag(s_ui.portal, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_ui.portal, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ---------------------------------------------------------------- 熄屏/唤醒

// 唤醒序列:面板 Sleep Out(0x11,IDF 驱动内置 100ms 时序等待,再补 30ms 凑满
// 规格要求的 120ms)→ 显示开(0x29)→ 恢复 LVGL 任务 → 背光 100。
// 运行于 input 任务上下文:此时 LVGL 已停,esp_lcd 命令无并发;调用安全。
static void screen_wake(void)
{
    esp_lcd_panel_handle_t panel = bsp_display_panel();
    if (panel) {
        (void)esp_lcd_panel_disp_sleep(panel, false);
        vTaskDelay(pdMS_TO_TICKS(30));
        (void)esp_lcd_panel_disp_on_off(panel, true);
    }
    lvgl_port_resume();
    bsp_display_backlight(100);
    atomic_store(&s_screen_off, false);
}

// 熄屏序列:背光 0(最大头)→ 停 LVGL 任务(刷屏 SPI 流量与渲染归零)→
// 面板显示关(0x28)+ Sleep In(0x10,µA 级)。两个调用方:
//   portal tick(esp_timer,自动熄屏)与 input 任务(OK 手动熄屏)。
// 都不是 LVGL 任务 —— lvgl_port_stop 不能由 LVGL 任务自己调用(死锁)。
static void screen_sleep(void)
{
    bsp_display_backlight(0);
    (void)lvgl_port_stop();
    esp_lcd_panel_handle_t panel = bsp_display_panel();
    if (panel) {
        (void)esp_lcd_panel_disp_on_off(panel, false);
        (void)esp_lcd_panel_disp_sleep(panel, true);
    }
    atomic_store(&s_screen_off, true);
}

// ---------------------------------------------------------------- 按键处理

// 键事件入口(input 任务):所有 UI 修改在 bsp_lvgl_lock 下;副作用在锁外。
void app_ui_on_key(int btn, int ev)
{
    s_last_input_ms = lv_tick_get();
    s_last_input_us = esp_timer_get_time();

    // 熄屏中:任意按键只唤醒,不执行该键的动作(防口袋/误触),事件丢弃。
    if (atomic_load(&s_screen_off)) {
        screen_wake();
        return;
    }

    // 副作用收集:持锁阶段只改 UI/状态,解锁后执行。
    bool do_sleep = false;
    bool do_refresh = false;

    if (!bsp_lvgl_lock(300)) {
        return; // 锁超时(极少见):丢弃本次按键,下一键恢复
    }

    switch (s_state) {
    case UI_MAIN:
        // 注意:菜单入口只有下键(单击)。OK 长按不做任何事 —— 避免与熄屏键
        // 语义混淆(实测用户预期:下键=设置)。
        if (btn == (int)BSP_BTN_OK && ev == 0) {
            do_sleep = true; // 单击 OK:手动熄屏;唤醒走最前面的分支
        } else if (ev == 0 && btn == (int)BSP_BTN_UP) {
            do_refresh = true; // 用量页=手动刷新
        } else if (ev == 0 && btn == (int)BSP_BTN_DOWN) {
            s_state = UI_MENU;
            rebuild_page();
        }
        break;

    case UI_MENU:
        if (ev == 3) {
            s_state = UI_MAIN;
            rebuild_page();
        } else if (ev == 0 && btn == (int)BSP_BTN_UP) {
            s_menu_sel = (s_menu_sel + MENU_N - 1) % MENU_N;
            refresh_rows_cursor(s_menu_sel);
        } else if (ev == 0 && btn == (int)BSP_BTN_DOWN) {
            s_menu_sel = (s_menu_sel + 1) % MENU_N;
            refresh_rows_cursor(s_menu_sel);
        } else if (ev == 0 && btn == (int)BSP_BTN_OK) {
            if (s_menu_sel == 5) {
                // 返回行
                s_state = UI_MAIN;
            } else {
                s_opt_sel = 0;
                s_wifi_sel = 0;
                s_state = (s_menu_sel == 0) ? UI_SUB_REFRESH
                        : (s_menu_sel == 1) ? UI_SUB_SOFF
                        : (s_menu_sel == 2) ? UI_SUB_WIFI
                        : (s_menu_sel == 3) ? UI_SUB_INFO : UI_SUB_PROV;
            }
            rebuild_page();
        }
        break;

    case UI_SUB_REFRESH:
    case UI_SUB_SOFF: {
        const uint16_t *opts = (s_state == UI_SUB_REFRESH) ? REFRESH_OPTS : SOFF_OPTS;
        int n = (s_state == UI_SUB_REFRESH) ? REFRESH_N : SOFF_N;
        if (ev == 3) {
            s_state = UI_MENU;
            rebuild_page();
        } else if (ev == 0 && btn == (int)BSP_BTN_UP) {
            s_opt_sel = (s_opt_sel + n) % (n + 1); // 最后一项是返回行
            refresh_rows_cursor(s_opt_sel);
        } else if (ev == 0 && btn == (int)BSP_BTN_DOWN) {
            s_opt_sel = (s_opt_sel + 1) % (n + 1);
            refresh_rows_cursor(s_opt_sel);
        } else if (ev == 0 && btn == (int)BSP_BTN_OK) {
            if (s_opt_sel == n) {
                // 返回行:回菜单。
                s_state = UI_MENU;
                rebuild_page();
                break;
            }
            uint16_t v = opts[s_opt_sel];
            bool ok = (s_state == UI_SUB_REFRESH) ? app_storage_save_period(v)
                                                  : app_storage_save_screen_off(v);
            app_glm_client_refresh_now(); // 刷新周期变化立即生效
            s_state = UI_MENU;
            rebuild_page();
            show_toast(ok ? "已保存并生效" : "保存失败");
        }
        break;
    }

    case UI_SUB_WIFI: {
        int total = s_wifi_cache_n + 1; // 含返回行
        if (ev == 3) {
            s_state = UI_MENU;
            rebuild_page();
        } else if (ev == 0 && btn == (int)BSP_BTN_UP && total > 1) {
            s_wifi_sel = (s_wifi_sel + total - 1) % total;
            rebuild_page(); // 行内容含连接标记与滚动窗口,重建最稳
        } else if (ev == 0 && btn == (int)BSP_BTN_DOWN && total > 1) {
            s_wifi_sel = (s_wifi_sel + 1) % total;
            rebuild_page();
        } else if (ev == 0 && btn == (int)BSP_BTN_OK) {
            if (s_wifi_sel == s_wifi_cache_n) {
                // 返回行
                s_state = UI_MENU;
                rebuild_page();
            } else if (s_wifi_cache_n > 0) {
                app_net_connect_ssid(s_wifi_cache[s_wifi_sel]);
                rebuild_page(); // 重建后 ✓ 标记移到新连接项
                show_toast("正在连接,请稍候…");
            }
        }
        break;
    }

    case UI_SUB_PROV:
        if (ev == 3) {
            s_state = UI_MENU;
            rebuild_page();
        } else if (ev == 0 && btn == (int)BSP_BTN_UP) {
            s_prov_sel = (s_prov_sel + 1) % 2; // 两行循环
            refresh_rows_cursor(s_prov_sel);
        } else if (ev == 0 && btn == (int)BSP_BTN_DOWN) {
            s_prov_sel = (s_prov_sel + 1) % 2;
            refresh_rows_cursor(s_prov_sel);
        } else if (ev == 0 && btn == (int)BSP_BTN_OK) {
            if (s_prov_sel == 1) {
                // 返回行
                s_state = UI_MENU;
                rebuild_page();
            } else {
                // 开关行:翻转配网门户。
                app_net_status_t st;
                app_net_get_status(&st);
                if (st.portal_active) app_net_stop_portal();
                else app_net_start_portal();
                rebuild_page(); // 开关行文案随状态刷新
                show_toast(st.portal_active ? "配网已关闭" : "配网已开启");
            }
        }
        break;

    case UI_SUB_INFO:
        // 信息页无动作项:任意键返回菜单。
        s_state = UI_MENU;
        rebuild_page();
        break;
    }

    bsp_lvgl_unlock();

    // ---- 副作用(锁外) ----
    if (do_sleep) screen_sleep();
    if (do_refresh) app_glm_client_refresh_now();
}

void app_ui_portal_tick(void)
{
    // HTTP 门户开机常驻(配网期经 192.168.4.1,联网后经局域网 IP,同一套页面);
    // 这里只做两件事:确保服务存活(异常退出则拉起)+ AP 关闭后撤掉 DNS 劫持。
    if (!app_portal_running()) {
        (void)app_portal_start();
    }
    app_net_status_t st;
    app_net_get_status(&st);
    if (!st.portal_active) {
        app_portal_stop_dns();
    }

    // 熄屏判定:每 30 秒重读一次用户偏好(门户可改),静息超时则熄屏。
    // 用 esp_timer 计时 —— lvgl_port_stop 之后 lv_tick 会停,不能用。
    if (++s_prefs_age >= 30) {
        s_prefs_age = 0;
        uint16_t off_s = 300;
        app_storage_load_screen_off(&off_s);
        if (off_s == 0) return; // 永不熄屏
        int64_t idle_us = esp_timer_get_time() - s_last_input_us;
        if (!atomic_load(&s_screen_off) && idle_us > (int64_t)off_s * 1000000LL) {
            screen_sleep();
        }
    }
}

void app_ui_init(void)
{
    // 字体描述符浅拷贝 + fallback 链(见 lvgl-chinese-fonts 文档方案 B)。
    s_font16 = app_font_16;
    s_font16.fallback = &lv_font_montserrat_14;
    s_font24 = app_font_24;
    s_font24.fallback = &lv_font_montserrat_20;

    refresh_key_mask();

    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(COL_BG), 0);
    lv_screen_load(s_scr);

    s_state = UI_MAIN;
    s_menu_sel = 0;
    rebuild_page();
    s_last_input_ms = lv_tick_get();
    s_last_input_us = esp_timer_get_time();
    atomic_init(&s_screen_off, false);
    lv_timer_create(poll_timer_cb, 500, NULL);
}
