// main/app_storage.h —— 应用配置的 NVS 持久化(API Key、已保存热点、点选状态)。
//
// NVS 键(命名空间 "glm",键名 ≤15 字符):
//   api_key  —— GLM API Key(明文存于设备本地 NVS;配网门户提交后写入)
//   nets     —— 已保存热点列表(app_netlist_serialize 格式)
//   sel_ssid —— 用户点选的热点 SSID;为空表示未点选
//   org_id   —— 团队套餐组织 ID(bigmodel-organization 头);为空=个人套餐模式
//   proj_id  —— 团队套餐项目 ID(bigmodel-project 头)
//   period_s —— 用量刷新周期(秒);未保存=默认 60
// 本模块是薄封装:纯逻辑都在 app_netlist / app_glm_usage,可测逻辑不放这里。
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "app_netlist.h"

#define APP_STORAGE_API_KEY_MAX 129   // bigmodel API Key 实际 ~40+ 字符,留足裕量

// 初始化 NVS。分区新建/版本变化(ESP_ERR_NVS_NO_FREE_PAGES /
// ESP_ERR_NVS_NEW_VERSION_FOUND)时擦除重试 —— 本固件是该设备 NVS 的唯一所有者,
// 这两个错误只意味着分区表格式变化,标准做法即擦除。其他错误不擦除,向上返回。
// 返回 ESP_OK 或底层错误码。
int app_storage_init(void);

// 读 API Key 到 buf(恒 NUL 结尾)。已保存返回 true;未保存/错误返回 false(buf 置空)。
bool app_storage_load_api_key(char *buf, size_t buf_len);
// 保存 API Key。空串等价于清除。ESP_OK 时返回 true。
bool app_storage_save_api_key(const char *api_key);

// 读热点列表。无记录返回 false(list 复位为空);blob 损坏同样返回 false 且复位,
// 坏列表直接丢弃 —— 宁可让用户重新配网,也不拿半损坏的密码反复尝试。
bool app_storage_load_netlist(app_netlist_t *list);
// 保存热点列表与点选状态(list->selected >= 0 时一并写入 sel_ssid)。
bool app_storage_save_netlist(const app_netlist_t *list);

// 团队套餐组织/项目 ID(配网门户可选填写)。org_buf 恒 NUL 结尾;未保存返回
// false 且置空。proj 同理。两者需同时配置才启用团队套餐接口。
bool app_storage_load_org(char *org_buf, size_t org_len);
bool app_storage_load_project(char *proj_buf, size_t proj_len);
// 保存组织/项目 ID;两个都传空串即清除。返回 true 表示写入成功。
bool app_storage_save_org_project(const char *org_id, const char *project_id);

// 熄屏超时(秒):静息超过该时长自动关背光并让面板睡眠。合法值:
// 60/300/600/1800/0(0=永不熄屏);未保存返回 false,调用方用默认 300。
bool app_storage_load_screen_off(uint16_t *screen_off_s);
bool app_storage_save_screen_off(uint16_t screen_off_s);

// 刷新周期(秒)。合法值:60/300/600/900/1800/3600(门户下拉的六个选项);
// 未保存返回 false,调用方用默认 60。保存前不校验,调用方负责只传合法值。
bool app_storage_load_period(uint16_t *period_s);
bool app_storage_save_period(uint16_t period_s);

// 清空本应用全部配置(门户"清除配置"按钮用),返回 true 表示命名空间已擦净。
bool app_storage_clear_all(void);
