// main/app_storage.h —— GLM 专属配置的 NVS 持久化(API Key、团队上下文)。
//
// 迁移到统一框架后,通用配置已归框架 appfw_storage("appfw" 命名空间):
// 已保存热点(netlist)、刷新周期、熄屏时间、亮度。本模块只保留 "glm"
// 命名空间下的应用私有键 —— 保留独立命名空间是为了升级兼容:老设备 NVS
// 里已存的 Key/组织/项目 ID 在升级后原样可用。
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define APP_STORAGE_API_KEY_MAX 129   // bigmodel API Key 实际 ~40+ 字符,留足裕量
#define APP_ORG_ID_MAX 64
#define APP_PROJ_ID_MAX 64

// 读 API Key 到 buf(恒 NUL 结尾)。已保存返回 true;未保存/错误返回 false(buf 置空)。
bool app_storage_load_api_key(char *buf, size_t buf_len);
// 保存 API Key。空串等价于清除。成功返回 true。
bool app_storage_save_api_key(const char *api_key);

// 团队套餐组织/项目 ID(配网门户可选填写)。org_buf 恒 NUL 结尾;未保存返回
// false 且置空。proj 同理。两者需同时配置才启用团队套餐接口。
bool app_storage_load_org(char *org_buf, size_t org_len);
bool app_storage_load_project(char *proj_buf, size_t proj_len);
// 保存组织/项目 ID;两个都传空串即清除。返回 true 表示写入成功。
bool app_storage_save_org_project(const char *org_id, const char *project_id);

// 清空 GLM 私有键(Key/组织/项目;门户"清除"按钮用)。框架通用配置
// (热点/周期/熄屏)由框架自己的 /api/clear 负责,这里不碰。
bool app_storage_clear_glm(void);
