// main/app_portal_glm.h —— 门户的 GLM 专属部分:阶段二注入片段 + 私有端点。
//
// 框架门户(appfw_portal)提供通用阶段一(WiFi 配置)、扫描、保存、状态、
// 导出导入与清除;本模块通过注入点补上用量宝专属内容:
//   app_config_html → 阶段二「API Key」「团队上下文」两张卡片(含脚本)
//   on_httpd_ready  → 注册 POST /api/glm(保存 Key/团队/清除)与
//                     POST /api/discover(网页 Token 换组织/项目清单)
//   app_config_fill/apply → 状态回显与配置导入里的 GLM 字段
#pragma once

#include "appfw_portal.h"

// 装配注入点与私有端点(app_main 里在 appfw_prov_configure 之前调用)。
void app_portal_glm_configure(appfw_prov_cfg_t *cfg);
