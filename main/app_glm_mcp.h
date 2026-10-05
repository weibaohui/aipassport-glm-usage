// main/app_glm_mcp.h —— GLM 业务的 MCP 工具表。
//
// 把原网页门户里"GLM 配置"那套功能开放给 AI:配好 WiFi 后,Key、团队上下文、
// 用量查询全部经 MCP 完成(AI 是遥控器,与收音机同一理念)。
#pragma once

// 注册应用工具(set_glm_key / set_glm_team / glm_discover / glm_usage /
// clear_glm)。内部转调 appfw_mcp_set_tools;须在 appfw_mcp_server_start 前调。
void glm_mcp_init(void);
