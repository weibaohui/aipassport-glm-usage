// main/app_glm_usage.h —— 智谱 GLM Coding Plan 用量数据的纯逻辑层。
//
// 对应接口(实测存在,官方文档未完全公开,字段以实际返回为准):
//   GET https://open.bigmodel.cn/api/monitor/usage/quota/limit
//   Authorization: <API_KEY>            (部分实现也接受 Bearer 前缀)
// 返回示例(取自社区实测 cc-switch #1588):
//   {"code":200,"msg":"操作成功","success":true,
//    "data":{"level":"pro","limits":[
//      {"type":"TIME_LIMIT","percentage":7,"usage":1000,"currentValue":72,"remaining":928},
//      {"type":"TOKENS_LIMIT","percentage":44,"nextResetTime":1730000000000},
//      {"type":"TOKENS_LIMIT","percentage":53,"nextResetTime":1730600000000}]}}
//
// 两个 TOKENS_LIMIT(个人套餐)或 CREDIT_LIMIT(团队套餐,type=2 接口)分别
// 对应【5 小时窗口】和【本周】额度:重置时间更早(nextResetTime 更小)的是
// 5 小时窗口。本模块只做解析与派生计算,不碰 ESP-IDF/LVGL,便于主机测试覆盖。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 解析成功后能给出的全部信息。所有整数字段用 -1 表示"响应中不存在/不可用",
// 字符串字段用空串表示缺失,调用方据此降级显示,避免出现野值。
typedef struct {
    bool ok;                        // 整体解析是否成功(HTTP 200 且结构完整)
    int http_code;                  // 响应里的业务码 code(HTTP 状态码由调用方另行持有)
    char msg[64];                   // 响应里的 msg(业务错误原因,如"当前用户不存在coding plan");缺失为空串
    char level[16];                 // 套餐等级,如 "pro"/"max";缺失为空串
    // 5 小时窗口 token 额度(重置时间较早的那个 TOKENS_LIMIT)
    int tokens_5h_used_pct;         // 已用百分比 0..100
    int64_t tokens_5h_reset_ms;     // 重置时间(Unix 毫秒);缺失为 -1
    // 本周 token 额度(重置时间较晚的那个 TOKENS_LIMIT)
    int tokens_week_used_pct;       // 已用百分比 0..100
    int64_t tokens_week_reset_ms;   // 重置时间(Unix 毫秒);缺失为 -1
    // MCP 工具每月调用次数(TIME_LIMIT;个人套餐有,团队套餐无)
    int mcp_used;                   // 已用次数
    int mcp_total;                  // 总次数
    int mcp_remaining;              // 剩余次数
    // 团队套餐(type=2 接口)的 CREDIT_LIMIT 额度是"点数"而非 token,
    // 剩余点数单独带出供界面展示;-1 表示响应中没有。
    int tokens_5h_remaining;        // 5 小时窗口剩余点数
    int tokens_week_remaining;      // 本周剩余点数
    // 重置次数(customer-package-reset/list,仅团队套餐):available 的重置
    // 记录条数 = 额度还能重置的次数;-1 表示未获取。
    int five_hour_resets_left;      // 5 小时窗口剩余重置次数
    int week_resets_left;           // 本周剩余重置次数
} glm_usage_t;

// 把一段 HTTP 响应体解析为 glm_usage_t。
// body/body_len:响应体字节(无需 NUL 结尾);out:输出结构,函数内先整体清零。
// 返回 true 表示拿到了业务结构(即使业务码非 200,也会带出 http_code 供上层
// 判定"密钥无效/额度接口变更"等);返回 false 表示响应体根本不是预期的 JSON。
// 纯函数,无副作用,线程安全;可在任意任务/主机环境调用。
bool glm_usage_parse(const char *body, size_t body_len, glm_usage_t *out);

// 粗判响应是否"密钥被拒绝":业务码 401/1001 或 HTTP 401(上层把 HTTP 状态码
// 塞进 http_code 时也覆盖)。用于屏幕上把"查询失败"细化为"密钥无效"。
bool glm_usage_is_auth_error(const glm_usage_t *u);

// 解析 customer-package-reset/list 响应,统计可用重置次数:
// fiveHourResets[]/weekResets[] 中 available==true 的条数。解析成功返回 true
// 并写 out_5h/out_week(响应缺数组时写 -1);非 JSON 返回 false。纯函数。
bool glm_resets_parse(const char *body, size_t body_len,
                      int *out_5h, int *out_week);

// 把 getCustomerInfo 响应(data 部分为 JSON)压缩成门户可用的项目清单:
//   [{"orgName":"…","orgId":"org-…","projects":[{"name":"…","id":"proj_…"}]},…]
// 只保留名称/ID 字段,便于配网页直接渲染下拉框;org/projects 为空时输出 "[]"。
// out_len 不足返回 false(输出勿用);body 非 JSON 返回 false。
bool glm_customer_parse_projects(const char *body, size_t body_len,
                                 char *out, size_t out_len);

// 把 Unix 毫秒时间戳格式化为 "MM-DD HH:MM"(按东八区,无跨时区需求)。
// buf 至少 12 字节;时间戳 <0 或溢出时写入 "--"。返回 buf。
// 纯函数:不做系统时间调用,时区换算固定 +8h(设备无 RTC,本应用固定国内使用)。
char *glm_usage_format_reset_ms(int64_t reset_ms, char *buf, size_t buf_len);
