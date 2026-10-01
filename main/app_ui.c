// main/app_ui.c —— 界面实现,布局与交互契约见 app_ui.h。
//
// 视觉:深色底(#0E1116)+ 绿色强调(#35C26B),与基线 demo 的像素纸风完全不同;
// 仅用 LVGL 基础控件(label/bar),不使用图片素材,便于小内存设备复用。
//
// 线程模型(关键):
//   - 按键处理在 input 任务上下文,绝不能直接碰 lv_*;这里只置"待切页"标志。
//   - 一切页面创建/销毁/改文本都发生在 LVGL 定时器回调(LVGL 任务上下文)。
//   - 页内容在切页时整体重建:所有 UI 更新都来自定时器轮询共享快照,没有外部
//     任务持有控件指针,重建不产生悬空引用。
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
#include "esp_lcd_panel_ops.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
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

typedef struct {
    lv_obj_t *page;         // 当前页容器
    lv_obj_t *battery;      // 右上角电量
    lv_obj_t *portal;       // 配网横幅(每页都有)
    // 用量页控件
    lv_obj_t *week_bar, *week_pct, *week_reset;
    lv_obj_t *h5_bar, *h5_pct, *h5_reset;
    lv_obj_t *mcp_bar, *mcp_val, *mcp_label, *foot;
    // 网络页控件
    lv_obj_t *net_lines;
} ui_t;

static ui_t s_ui;
static int s_page;                 // 0=用量 1=网络(LVGL 上下文读写)
static volatile int s_pending_page = -1; // 按键任务→LVGL 的切页请求;-1=无
static lv_obj_t *s_scr;
static uint32_t s_last_input_ms;   // 最近按键时刻(保留:亮屏时显示层用)
static atomic_bool s_screen_off;   // 面板睡眠中(portal_tick 熄屏 / input_task 唤醒,双任务访问)
static int64_t s_last_input_us;    // 最近交互时刻(esp_timer 微秒;LVGL 停止后仍可靠)
static int s_prefs_age;            // portal_tick 循环计数:周期性重读用户偏好
static char s_saved_list_text[288]; // 已存热点段文本(网络页构建时读一次 NVS 生成,轮询复用;
                                    // 不能"只拼一次"——下一轮 set_text 会用不含它的文本覆盖)
static char s_team_line[112];        // "团队:…"+"刷新:… 分钟" 两行状态(同上缓存策略)
static char s_key_mask[24];        // "abcd…wxyz" 掩码,init 时生成一次

// ---------------------------------------------------------------- 工具

// 从 NVS 重读 API Key 生成掩码(abcd…wxyz;不足 8 位只显示前 2 位;无 Key="未设置")。
// 不能只在开机时生成一次:门户保存 Key 后屏幕不会重启,网络页每次重建时调用本函数,
// 显示才能与实际配置保持一致(团队/周期状态同理)。
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

// ---------------------------------------------------------------- 页面构建

// 顶部栏:标题 + 右上角电量(规范默认位;读 bsp_battery_soc,失败优雅降级)。
static void build_top_bar(lv_obj_t *page, const char *title)
{
    lv_obj_t *t = lv_label_create(page);
    style_label(t, &s_font24, COL_TEXT);
    lv_label_set_text(t, title);
    lv_obj_set_pos(t, 12, 10);

    s_ui.battery = lv_label_create(page);
    style_label(s_ui.battery, &s_font16, COL_DIM);
    lv_obj_set_pos(s_ui.battery, 178, 16);
    lv_label_set_text(s_ui.battery, "--");
}

static void build_portal_banner(lv_obj_t *page)
{
    s_ui.portal = lv_label_create(page);
    style_label(s_ui.portal, &s_font16, COL_WARN);
    lv_obj_set_width(s_ui.portal, 216);
    lv_label_set_long_mode(s_ui.portal, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_ui.portal, 12, 292);
    lv_label_set_text(s_ui.portal, "");
}

// 用量页:三组"标签+百分比+进度条",底部一行状态(套餐/倒计时/错误)。
// 纵向节奏:行高 76px,三行 + 状态行 + 横幅,总计 320px 内。
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

    // 行 3:MCP 月度调用(个人套餐);团队套餐时由 update 改写为"剩余点数"。
    s_ui.mcp_label = lv_label_create(page);
    style_label(s_ui.mcp_label, &s_font16, COL_DIM);
    lv_label_set_text(s_ui.mcp_label, "MCP 调用(每月)");
    lv_obj_set_pos(s_ui.mcp_label, 14, 202);
    s_ui.mcp_val = lv_label_create(page);
    style_label(s_ui.mcp_val, &s_font24, COL_TEXT);
    lv_obj_set_pos(s_ui.mcp_val, 140, 198);
    lv_label_set_text(s_ui.mcp_val, "--");
    s_ui.mcp_bar = make_bar(page, 14, 234, 212, 12);

    // 状态行:套餐 + 刷新倒计时/错误(超长截尾)。
    s_ui.foot = lv_label_create(page);
    style_label(s_ui.foot, &s_font16, COL_DIM);
    lv_obj_set_width(s_ui.foot, 216);
    lv_label_set_long_mode(s_ui.foot, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_ui.foot, 14, 258);
    lv_label_set_text(s_ui.foot, "等待数据…");
}

static void build_net_page(lv_obj_t *page)
{
    s_ui.net_lines = lv_label_create(page);
    style_label(s_ui.net_lines, &s_font16, COL_TEXT);
    lv_obj_set_pos(s_ui.net_lines, 14, 50);
    lv_obj_set_width(s_ui.net_lines, 212);
    lv_label_set_long_mode(s_ui.net_lines, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_ui.net_lines, "加载中…");
}

// 重建当前页:删除旧容器(连带全部子对象)后按 s_page 重新构建。s_ui 内所有
// 控件指针随本次构建重新赋值,不残留悬空指针。
static void rebuild_page(void)
{
    if (s_ui.page) {
        lv_obj_delete(s_ui.page);
        memset(&s_ui, 0, sizeof(s_ui));
    }
    s_saved_list_text[0] = '\0';
    s_team_line[0] = '\0';
    if (s_page == 1) {
        refresh_key_mask(); // Key 可能刚在门户被保存/清除,重建时重读
        app_netlist_t list;
        if (app_storage_load_netlist(&list) && list.count > 0) {
            size_t used = (size_t)snprintf(s_saved_list_text, sizeof(s_saved_list_text), "已存热点:\n");
            for (uint8_t i = 0; i < list.count && used < sizeof(s_saved_list_text) - 48; i++) {
                used += (size_t)snprintf(s_saved_list_text + used,
                                         sizeof(s_saved_list_text) - used, "%s %s\n",
                                         list.selected == (int8_t)i ? ">" : "·",
                                         list.items[i].ssid);
            }
        } else {
            snprintf(s_saved_list_text, sizeof(s_saved_list_text), "已存热点:无\n");
        }
        // 团队上下文与刷新周期状态(真机可看的配置摘要,与门户一致)。
        // 参数一行一个(组织、项目、刷新各自独立一行;周期都是 60 的倍数按分钟显示)。
        // 组织与项目目前成对保存,状态一致,但分行显示便于核对每一项。
        char org_chk[64] = { 0 }, proj_chk[64] = { 0 };
        bool has_org = app_storage_load_org(org_chk, sizeof(org_chk));
        bool has_proj = app_storage_load_project(proj_chk, sizeof(proj_chk));
        uint16_t period_s = GLM_API_PERIOD_S;
        app_storage_load_period(&period_s);
        uint16_t soff = 300;
        app_storage_load_screen_off(&soff);
        snprintf(s_team_line, sizeof(s_team_line),
                 "组织:%s\n项目:%s\n刷新:%u 分钟\n熄屏:%s\n",
                 has_org ? "已配置" : "未配置",
                 has_proj ? "已配置" : "未配置", (unsigned)(period_s / 60),
                 soff == 0 ? "从不" : "");
        if (soff != 0) {
            char tail[24];
            snprintf(tail, sizeof(tail), "%u 分钟\n", (unsigned)(soff / 60));
            strncat(s_team_line, tail, sizeof(s_team_line) - strlen(s_team_line) - 1);
        }
    }
    s_ui.page = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_ui.page);
    lv_obj_set_size(s_ui.page, 240, 320);

    if (s_page == 0) {
        build_top_bar(s_ui.page, "GLM 用量");
        build_usage_page(s_ui.page);
    } else {
        build_top_bar(s_ui.page, "网络");
        build_net_page(s_ui.page);
    }
    build_portal_banner(s_ui.page);
}

// ---------------------------------------------------------------- 数据刷新

// 重置行:固定"重置 时间";团队套餐的点数窗口额外显示剩余(响应里有才显示)。
static void set_reset_line(lv_obj_t *label, int64_t reset_ms, int remaining)
{
    char buf[16];
    if (remaining >= 0) {
        lv_label_set_text_fmt(label, "重置 %s · 余 %d",
                              glm_usage_format_reset_ms(reset_ms, buf, sizeof(buf)), remaining);
    } else {
        lv_label_set_text_fmt(label, "重置 %s",
                              glm_usage_format_reset_ms(reset_ms, buf, sizeof(buf)));
    }
}

static void update_usage_page(const glm_usage_t *u)
{
    set_bar_pct(s_ui.week_bar, u->tokens_week_used_pct);
    set_pct_text(s_ui.week_pct, u->tokens_week_used_pct);
    set_reset_line(s_ui.week_reset, u->tokens_week_reset_ms, u->tokens_week_remaining);

    set_bar_pct(s_ui.h5_bar, u->tokens_5h_used_pct);
    set_pct_text(s_ui.h5_pct, u->tokens_5h_used_pct);
    set_reset_line(s_ui.h5_reset, u->tokens_5h_reset_ms, u->tokens_5h_remaining);

    // 行 3:个人套餐显示 MCP 每月调用(已用/总量);团队套餐无 MCP 限制,
    // 改显示本周剩余点数,进度条沿用本周已用百分比。
    if (u->mcp_total > 0) {
        lv_label_set_text(s_ui.mcp_label, "MCP 调用(每月)");
        lv_label_set_text_fmt(s_ui.mcp_val, "%d/%d", u->mcp_used, u->mcp_total);
        set_bar_pct(s_ui.mcp_bar, u->mcp_used * 100 / u->mcp_total);
    } else if (u->tokens_week_remaining >= 0) {
        lv_label_set_text(s_ui.mcp_label, "剩余点数");
        lv_label_set_text_fmt(s_ui.mcp_val, "%d", u->tokens_week_remaining);
        set_bar_pct(s_ui.mcp_bar, u->tokens_week_used_pct);
    } else {
        lv_label_set_text(s_ui.mcp_label, "MCP 调用(每月)");
        lv_label_set_text(s_ui.mcp_val, "--");
        set_bar_pct(s_ui.mcp_bar, -1);
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
        case GLM_ERR_AUTH: msg = "Key 无效,长按OK配网"; break;
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

static void update_net_page(void)
{
    app_net_status_t st;
    app_net_get_status(&st);
    char text[512];
    size_t used = 0;
    used += (size_t)snprintf(text + used, sizeof(text) - used, "WiFi ");
    switch (st.state) {
    case APP_NET_ONLINE:
        used += (size_t)snprintf(text + used, sizeof(text) - used,
                                 "已连接 %s(%d dBm)\nIP %s\n\n",
                                 st.cur_ssid, st.rssi, st.ip);
        break;
    case APP_NET_CONNECTING:
        used += (size_t)snprintf(text + used, sizeof(text) - used,
                                 "正在连接 %s…\n\n", st.cur_ssid);
        break;
    case APP_NET_OFFLINE_RETRY:
        used += (size_t)snprintf(text + used, sizeof(text) - used, "连接失败,自动重试中…\n\n");
        break;
    case APP_NET_SCANNING:
        used += (size_t)snprintf(text + used, sizeof(text) - used, "扫描中…\n\n");
        break;
    default:
        used += (size_t)snprintf(text + used, sizeof(text) - used, "未连接\n\n");
        break;
    }
    used += (size_t)snprintf(text + used, sizeof(text) - used, "API Key:%s\n", s_key_mask);
    used += (size_t)snprintf(text + used, sizeof(text) - used, "%s", s_team_line);

    used += (size_t)snprintf(text + used, sizeof(text) - used, "%s", s_saved_list_text);
    // 联网后管理页走局域网 IP(同一套配网页:改 Key/选项目/改热点都用它)。
    if (st.state == APP_NET_ONLINE && st.ip[0]) {
        used += (size_t)snprintf(text + used, sizeof(text) - used,
                                 "管理页 http://%s\n", st.ip);
    }
    used += (size_t)snprintf(text + used, sizeof(text) - used, "长按OK:配网");
    lv_label_set_text(s_ui.net_lines, text);
}

// ---------------------------------------------------------------- 定时器与入口

static void poll_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    // 1) 应用待切页请求(按键任务只置标志,重建在这里 = LVGL 上下文,安全)。
    if (s_pending_page >= 0 && s_pending_page != s_page) {
        s_page = s_pending_page;
        rebuild_page();
    }
    s_pending_page = -1;

    // 2) 电量:CW2017 寄存器读,代价低,每轮刷新。
    if (s_ui.battery) {
        int soc = bsp_battery_soc();
        if (soc >= 0) lv_label_set_text_fmt(s_ui.battery, "%d%%", soc);
        else lv_label_set_text(s_ui.battery, "--");
    }

    // 3) 页面内容。
    if (s_page == 0 && s_ui.week_bar) {
        glm_usage_t u;
        glm_err_t err;
        int64_t fetch_epoch_s;
        bool last_ok;
        app_glm_client_get_snapshot(&u, &err, &fetch_epoch_s, &last_ok);
        update_usage_page(&u);
        update_foot(&u, err, last_ok, fetch_epoch_s);
    } else if (s_page == 1 && s_ui.net_lines) {
        update_net_page();
    }

    // 4) 配网横幅:仅在"AP 开着且未联网"时显示(联网后用户已不需要引导)。
    if (s_ui.portal) {
        app_net_status_t st;
        app_net_get_status(&st);
        if (st.portal_active && st.state != APP_NET_ONLINE) {
            lv_label_set_text_fmt(s_ui.portal,
                                  "配网中:连接热点 %s,电脑打开 192.168.4.1",
                                  st.ap_ssid);
            lv_obj_clear_flag(s_ui.portal, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_ui.portal, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

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
// 面板显示关(0x28)+ Sleep In(0x10,µA 级)。运行于 portal tick(esp_timer)上下文,
// LVGL 任务自身不调用 lvgl_port_stop(等自己退出会死锁),此处从其他任务停它是安全的。
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

void app_ui_on_key(int btn, int ev)
{
    s_last_input_ms = lv_tick_get();
    s_last_input_us = esp_timer_get_time();
    // 熄屏中:任意按键只唤醒,不执行该键的动作(防口袋/误触),事件丢弃。
    if (atomic_load(&s_screen_off)) {
        screen_wake();
        return;
    }
    // ev:0=单击 3=长按;btn:0=上 1=下 2=OK(见 app_ui.h 注释)。
    // 本函数运行在 input 任务上下文:只允许发请求/置标志,不做任何 lv_* 调用。
    if (btn == (int)BSP_BTN_OK && ev == 3) {
        app_net_start_portal(); // 长按 OK:进入配网(portal tick 随后拉起 HTTP/DNS)
        return;
    }
    if (btn == (int)BSP_BTN_OK && ev == 0) {
        app_glm_client_refresh_now(); // 单击 OK:手动立即刷新
        return;
    }
    if (ev == 0 && (btn == (int)BSP_BTN_UP || btn == (int)BSP_BTN_DOWN)) {
        s_pending_page = (btn == (int)BSP_BTN_UP) ? 0 : 1; // 上=用量页,下=网络页
    }
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

    s_page = 0;
    rebuild_page();
    s_last_input_ms = lv_tick_get();
    s_last_input_us = esp_timer_get_time();
    atomic_init(&s_screen_off, false);
    lv_timer_create(poll_timer_cb, 500, NULL);
}
