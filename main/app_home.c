// main/app_home.c —— 用量主页实现(布局与数据源沿用原 app_ui.c 用量页,
// 迁移到框架后页面骨架/菜单/熄屏由 appfw_ui 接管,见 docs/migration 文档)。
#include "app_home.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_storage.h"
#include "appfw_storage.h"
#include "app_glm_client.h"
#include "lvgl.h"

LV_FONT_DECLARE(app_font_16);
LV_FONT_DECLARE(app_font_24);

#define COL_TEXT 0xE6E6E6
#define COL_DIM 0x8B98A5
#define COL_OK 0x35C26B
#define COL_WARN 0xE5A13D
#define COL_BAD 0xE5484D
#define COL_BAR 0x24303C

static lv_font_t s_font16;
static lv_font_t s_font24;

typedef struct {
    lv_obj_t *week_bar, *week_pct, *week_reset;
    lv_obj_t *h5_bar, *h5_pct, *h5_reset;
    lv_obj_t *mcp_val, *mcp_label, *foot;
} home_ui_t;

static home_ui_t s_ui;

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

void app_home_build(lv_obj_t *page)
{
    // 字体描述符浅拷贝 + fallback 链(子集缺字退 Montserrat)。
    s_font16 = app_font_16;
    s_font16.fallback = &lv_font_montserrat_14;
    s_font24 = app_font_24;
    s_font24.fallback = &lv_font_montserrat_20;
    memset(&s_ui, 0, sizeof(s_ui));

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

void app_home_page_reset(void)
{
    memset(&s_ui, 0, sizeof(s_ui));
}

void app_home_refresh_now(void)
{
    app_glm_client_refresh_now();
}

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
        // 服务端业务错误(u->msg 非空)优先原样展示:比笼统的"失败"更有用。
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

    // 断网优先于一切:曾成功过也一样,网络没了就该说网络。
    if (err == GLM_ERR_WAIT_NET) {
        lv_label_set_text(s_ui.foot, "网络已断开,自动重连中…");
        lv_obj_set_style_text_color(s_ui.foot, lv_color_hex(COL_BAD), 0);
        return;
    }

    // 正常:套餐 + 下轮倒计时 + 最近成功时间。周期在框架设置菜单可配。
    const char *plan = u->level[0] ? u->level : "--";
    if (fetch_epoch_s > 1000000000LL) { // 早于 2001 年的墙钟视为未对时
        uint16_t period_s = 60;
        appfw_store_get_period(&period_s);
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

void app_home_poll(void)
{
    if (!s_ui.week_bar) return;
    glm_usage_t u;
    glm_err_t err;
    int64_t fetch_epoch_s;
    bool last_ok;
    app_glm_client_get_snapshot(&u, &err, &fetch_epoch_s, &last_ok);
    update_usage_page(&u);
    update_foot(&u, err, last_ok, fetch_epoch_s);
}

int app_home_config_rows(char (*keys)[24], char (*vals)[72], int max)
{
    int n = 0;
    char key[APP_STORAGE_API_KEY_MAX];
    if (n < max) {
        snprintf(keys[n], 24, "API Key");
        if (app_storage_load_api_key(key, sizeof(key))) {
            size_t len = strlen(key);
            snprintf(vals[n], 72, len >= 8 ? "%.4s…%.4s" : "%.2s…",
                     key, key + len - 4);
        } else {
            snprintf(vals[n], 72, "未设置");
        }
        n++;
    }
    if (n < max) {
        char org[64], proj[64];
        bool team = app_storage_load_org(org, sizeof(org)) &&
                    app_storage_load_project(proj, sizeof(proj));
        snprintf(keys[n], 24, "套餐模式");
        snprintf(vals[n], 72, team ? "团队(type=2)" : "个人");
        n++;
    }
    return n;
}
