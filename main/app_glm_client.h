// main/app_glm_client.h —— GLM 套餐用量后台客户端:每 60 秒查询一次并发布快照。
//
// 流程:等联网 → 等 SNTP 对时(TLS 证书校验需要正确时间)→ HTTPS 请求
// open.bigmodel.cn/api/monitor/usage/quota/limit → app_glm_usage_parse →
// 发布共享快照(自旋锁保护)→ 睡 60 秒,循环。
// 认证头:先按社区实测格式 "Authorization: <key>";若 401 再用 "Bearer <key>"
// 兼容一次 —— 两种口径都能工作,设备侧无需改固件。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_glm_usage.h"

// 个人套餐:BASE 不带参数直接查询。团队套餐:追加 "?type=2" 并带
// bigmodel-organization / bigmodel-project 两个请求头(2026-10-01 实测三者
// 缺一不可;鉴权仍用普通 API Key)。组织/项目 ID 在配网门户填写。
#define GLM_API_BASE "https://open.bigmodel.cn/api/monitor/usage/quota/limit"
#define GLM_API_URL GLM_API_BASE
#define GLM_API_PERIOD_S 60          // 用户需求:每 1 分钟一次

// 查询失败原因(屏幕据其显示不同文案)。
typedef enum {
    GLM_ERR_NONE = 0,
    GLM_ERR_WAIT_NET,     // 尚未联网
    GLM_ERR_WAIT_TIME,    // 时间未同步(SNTP 未成功)
    GLM_ERR_AUTH,         // 密钥无效/被拒
    GLM_ERR_HTTP,         // HTTP/网络错误
    GLM_ERR_PARSE,        // 响应不是预期的 JSON(接口变更?)
} glm_err_t;

// 启动后台查询任务。key 最大长度限制沿用 app_storage;任务常驻。
// 返回 0 成功;任务/队列创建失败返回 ESP_ERR 码。
int app_glm_client_start(void);

// 最近一次传输层/HTTP 错误的 esp_err_t(0=无);用于界面显示诊断码。
int app_glm_client_transport_err(void);

// 用一次性网页 JWT 调 getCustomerInfo,把组织/项目清单压缩成 JSON 写入 out
// (格式见 glm_customer_parse_projects)。仅供配网门户"发现项目"用:JWT 不落盘。
// 阻塞数秒(HTTPS),在门户 HTTP 任务的请求上下文调用。返回 0 成功;HTTP 401/403
// 返回 1(Token 失效);其他传输错误返回 ESP_ERR 码;解析失败返回 ESP_ERR_INVALID_RESPONSE。
int app_glm_client_discover_projects(const char *jwt, char *out, size_t out_len);

// 请求立即刷新一次(按键触发);下次循环提前执行,不阻塞调用者。
void app_glm_client_refresh_now(void);

// 读取最近一次快照(拷贝出自旋锁区)。fetch_epoch_s:最近一次拿到 HTTP 响应的
// Unix 秒(可能尚未对时,仅用于展示相对时间);err:最近一次错误;last_ok:是否
// 至少成功过一次(决定 UI 显示"数据更新于"还是错误文案)。
void app_glm_client_get_snapshot(glm_usage_t *usage, glm_err_t *err,
                                 int64_t *fetch_epoch_s, bool *last_ok);
