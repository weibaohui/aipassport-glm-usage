// main/app_home.h —— 用量主页(挂在框架 appfw_ui 的 home 钩子上)。
//
// 框架拥有:状态栏(标题/电量/⚠)、设置菜单、配网、熄屏、按键规整;
// 本模块只负责主页面内容:本周额度 / 5 小时窗口 / MCP 调用 三块用量卡
// 与状态行。上键(框架 home_up)触发一次立即刷新。
#pragma once

#include "lvgl.h"

// 持 bsp_lvgl_lock 调用一次(框架 home_build)。
void app_home_build(lv_obj_t *page);
// 框架主页轮询(LVGL 任务,500ms):用量快照 → 控件。
void app_home_poll(void);
// 主页上键(锁外):唤醒查询任务立即刷新。
void app_home_refresh_now(void);
// 页面重建回调:清空控件把手(框架删页后防悬空)。
void app_home_page_reset(void);
// 设备信息页「配置」区:Key 与团队上下文的回显行。
int app_home_config_rows(char (*keys)[24], char (*vals)[72], int max);
