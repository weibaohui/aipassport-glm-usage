// main/app_ui.h —— 应用界面:两页(用量 / 网络)+ 右上角电量 + 三键交互。
//
// 交互约定(重新设计,与基线 demo 菜单无关):
//   上   单击  用量页=手动立即刷新;网络页=切回用量页
//   下   单击  切到网络页
//   OK   单击  熄屏(亮屏态)/ 任意键唤醒(熄屏态,唤醒键不穿透)
//   OK   长按  进入配网模式(开启 SoftAP 门户)
//   无操作 5 分钟  背光调暗至 20%(任意按键恢复)
//
// 线程模型:所有 lv_* 调用都发生在 LVGL 任务上下文(esp_lvgl_port 的定时器回调
// 内),或持有 bsp_lvgl_lock() 的初始化路径;其他任务绝不直接碰 UI。
#pragma once

#include <stdbool.h>

// 创建页面并启动轮询定时器。必须满足:bsp_lvgl_init() 成功之后、在持锁环境
// 调用一次;之后无需再持锁(内部定时器运行于 LVGL 任务)。
void app_ui_init(void);

// 键事件入口(由按键分发任务调用;内部只改原子量/发请求,不做慢操作)。
void app_ui_on_key(int btn, int ev); // 用 int 避免 main 层依赖 BSP 头:0/1/2=上/下/OK,0=单击 3=长按

// 每秒调用一次(由 main 的维护定时器):根据网络状态启停配网门户的 HTTP/DNS,
// 与设备 AP 状态保持同步。放这里而不是 net 任务,是因为门户启停会做 socket/httpd
// 操作,由"UI 心跳"驱动比在 WiFi 任务里串行等待更稳。
void app_ui_portal_tick(void);
