// main/app_portal.h —— 配网门户:SoftAP 生效期间提供 DNS 劫持 + 中文配置网页。
//
// 电脑/手机连上设备 AP 后:
//   - 任意域名解析都被指向 192.168.4.1(captive portal 探测页会自动弹出);
//   - 浏览器打开 http://192.168.4.1 进入配置页:填 API Key、扫描并勾选多个
//     热点分别输密码、保存、点选连接、删除、清除配置。
// REST 约定(全部 JSON,除 / 返回 HTML):
//   GET  /              配置主页
//   GET  /api/status    连接状态(轮询)
//   GET  /api/scan      最近一次扫描结果(扫描由 POST 触发后异步进行)
//   POST /api/scan      触发一次扫描
//   GET  /api/saved     已保存热点(只回 SSID 与选中态,不回传密码)
//   POST /api/key       {key} 保存 API Key(空串=清除)
//   POST /api/networks  {networks:[{ssid,pwd}...]} 整表替换已保存热点
//   POST /api/connect   {ssid} 点选连接已保存热点
//   POST /api/clear     清除全部配置(API Key + 热点)
#pragma once

#include <stdbool.h>

// 启动 HTTP 服务(TCP 80,常驻)与 DNS 劫持(UDP 53,配网期把 captive 探测
// 引到设备)。HTTP 在配网(AP 模式,经 192.168.4.1)和联网(STA 模式,经局域网
// IP)两种阶段都应可访问,因此开机即启动且不再停止。幂等;启动失败返回 false。
bool app_portal_start(void);

// 停止 DNS 劫持(联网后不再需要);HTTP 服务保持。幂等。
void app_portal_stop_dns(void);

// 门户 HTTP 当前是否在服务(用于自检与异常重启)。
bool app_portal_running(void);
