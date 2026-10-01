// tests/test_glm_usage_parse.c —— app_glm_usage 纯逻辑的主机测试。
// 编译(见 tools/validate.sh):
//   cc -std=c11 -Wall -Wextra -Werror -Imain -Itests/thirdparty/cJSON \
//     tests/test_glm_usage_parse.c main/app_glm_usage.c tests/thirdparty/cJSON/cJSON.c
#include "app_glm_usage.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            failures++;                                                      \
        }                                                                    \
    } while (0)

// 官方响应形态(依据社区实测,cc-switch #1588)。
static const char *SAMPLE_OK =
    "{\"code\":200,\"msg\":\"操作成功\",\"success\":true,"
    "\"data\":{\"level\":\"pro\",\"limits\":["
    "{\"type\":\"TIME_LIMIT\",\"percentage\":7,\"usage\":1000,\"currentValue\":72,\"remaining\":928},"
    "{\"type\":\"TOKENS_LIMIT\",\"percentage\":44,\"nextResetTime\":1730000000000},"
    "{\"type\":\"TOKENS_LIMIT\",\"percentage\":53,\"nextResetTime\":1730600000000}"
    "]}}";

static void test_parse_ok(void)
{
    glm_usage_t u;
    CHECK(glm_usage_parse(SAMPLE_OK, strlen(SAMPLE_OK), &u));
    CHECK(u.ok);
    CHECK(u.http_code == 200);
    CHECK(strcmp(u.level, "pro") == 0);
    // 重置时间早的(1730000000000)是 5 小时窗口,晚的是本周。
    CHECK(u.tokens_5h_used_pct == 44);
    CHECK(u.tokens_5h_reset_ms == 1730000000000LL);
    CHECK(u.tokens_week_used_pct == 53);
    CHECK(u.tokens_week_reset_ms == 1730600000000LL);
    CHECK(u.mcp_used == 72);
    CHECK(u.mcp_total == 1000);
    CHECK(u.mcp_remaining == 928);
}

// TOKENS_LIMIT 顺序颠倒时,仍应按重置时间归位(5h = 较早)。
static void test_parse_order_swapped(void)
{
    const char *body =
        "{\"code\":200,\"data\":{\"level\":\"max\",\"limits\":["
        "{\"type\":\"TOKENS_LIMIT\",\"percentage\":10,\"nextResetTime\":9999999999999},"
        "{\"type\":\"TOKENS_LIMIT\",\"percentage\":90,\"nextResetTime\":1000000000000}"
        "]}}";
    glm_usage_t u;
    CHECK(glm_usage_parse(body, strlen(body), &u));
    CHECK(u.tokens_5h_used_pct == 90);
    CHECK(u.tokens_week_used_pct == 10);
}

// 缺 nextResetTime:按数组先后归位。
static void test_parse_missing_reset(void)
{
    const char *body =
        "{\"code\":200,\"data\":{\"level\":\"lite\",\"limits\":["
        "{\"type\":\"TOKENS_LIMIT\",\"percentage\":12},"
        "{\"type\":\"TOKENS_LIMIT\",\"percentage\":34}"
        "]}}";
    glm_usage_t u;
    CHECK(glm_usage_parse(body, strlen(body), &u));
    CHECK(u.tokens_5h_used_pct == 12);
    CHECK(u.tokens_week_used_pct == 34);
    CHECK(u.tokens_5h_reset_ms == -1);
}

// 业务码 401(密钥无效)也要能带出结构并标记 auth error。
static void test_parse_auth_error(void)
{
    const char *body = "{\"code\":401,\"msg\":\"无效的API Key\",\"success\":false}";
    glm_usage_t u;
    CHECK(glm_usage_parse(body, strlen(body), &u));
    CHECK(!u.ok);
    CHECK(glm_usage_is_auth_error(&u));
}

// 非法输入一律返回 false,且不越界。
static void test_parse_invalid(void)
{
    glm_usage_t u;
    CHECK(!glm_usage_parse(NULL, 0, &u));
    CHECK(!glm_usage_parse("not json", 8, &u));
    CHECK(!glm_usage_parse("{\"code\":200", 11, &u)); // 截断的 JSON
    CHECK(!glm_usage_parse("", 0, &u));
}

// 非法 HTTP 状态细分:非鉴权类失败不标记 auth error。
static void test_parse_http_error_code(void)
{
    const char *body = "{\"code\":500,\"msg\":\"internal\"}";
    glm_usage_t u;
    CHECK(glm_usage_parse(body, strlen(body), &u));
    CHECK(!u.ok);
    CHECK(!glm_usage_is_auth_error(&u));
}

// 重置时间格式化:东八区、已知时间点;非法值显示 "--"。
static void test_format_reset(void)
{
    char buf[16];
    // 2026-10-01 08:30 (UTC+8) = 2026-10-01 00:30 UTC = 1790814600 秒。
    glm_usage_format_reset_ms(1790814600000LL, buf, sizeof(buf));
    CHECK(strcmp(buf, "10-01 08:30") == 0);
    glm_usage_format_reset_ms(-1, buf, sizeof(buf));
    CHECK(strcmp(buf, "--") == 0);
}

// 团队套餐(type=2)响应:CREDIT_LIMIT 两项,remaining 字段带出。
static void test_parse_credit_limit(void)
{
    const char *body =
        "{\"code\":200,\"msg\":\"操作成功\",\"success\":true,"
        "\"data\":{\"level\":\"pro\",\"limits\":["
        "{\"type\":\"CREDIT_LIMIT\",\"unit\":3,\"number\":5,\"usage\":15000,"
        "\"currentValue\":360,\"remaining\":14639,\"percentage\":2,\"nextResetTime\":1790824884459},"
        "{\"type\":\"CREDIT_LIMIT\",\"unit\":6,\"number\":1,\"usage\":66000,"
        "\"currentValue\":51696,\"remaining\":14303,\"percentage\":78,\"nextResetTime\":1791126162984}"
        "]}}";
    glm_usage_t u;
    CHECK(glm_usage_parse(body, strlen(body), &u));
    CHECK(u.ok);
    CHECK(strcmp(u.level, "pro") == 0);
    // 重置早的(1790824884459)是 5 小时窗口;本周 78% / 剩余 14303。
    CHECK(u.tokens_5h_used_pct == 2);
    CHECK(u.tokens_5h_remaining == 14639);
    CHECK(u.tokens_week_used_pct == 78);
    CHECK(u.tokens_week_remaining == 14303);
    // 团队套餐无 MCP(TIME_LIMIT)限制。
    CHECK(u.mcp_total == -1);
}

// getCustomerInfo 真实响应形状 → 紧凑项目清单(含中文名、多组织)。
static void test_customer_projects(void)
{
    const char *body =
        "{\"code\":200,\"msg\":\"操作成功\",\"data\":{"
        "\"id\":9931991,\"customerName\":\"example_user\",\"phoneNumber\":\"130****0000\","
        "\"organizations\":["
        "{\"organizationName\":\"示例机构A\",\"organizationId\":\"org-EXAMPLE1111111111111111111111111111\",\"role\":\"owner\",\"isDefault\":true,"
        "\"projects\":[{\"projectName\":\"默认项目\",\"projectId\":\"proj_EXAMPLE2222222222222222222222222\"}]},"
        "{\"organizationName\":\"示例机构B\",\"organizationId\":\"org-EXAMPLE3333333333333333333333333\",\"role\":\"reader\",\"isDefault\":false,"
        "\"projects\":[{\"projectName\":\"示例团队项目\",\"projectId\":\"proj_EXAMPLE4444444444444444444444\"}]}"
        "]}}";
    char out[1024];
    CHECK(glm_customer_parse_projects(body, strlen(body), out, sizeof(out)));
    CHECK(strstr(out, "org-EXAMPLE1111111111111111111111111111") != NULL);
    CHECK(strstr(out, "示例机构A") != NULL);
    CHECK(strstr(out, "proj_EXAMPLE4444444444444444444444") != NULL);
    CHECK(strstr(out, "示例团队项目") != NULL);
    CHECK(strstr(out, "reader") == NULL); // 非必要字段已剥离

    // 缓冲不足 → false;非 JSON → false。
    char tiny[16];
    CHECK(!glm_customer_parse_projects(body, strlen(body), tiny, sizeof(tiny)));
    CHECK(!glm_customer_parse_projects("not json", 8, out, sizeof(out)));
}

// customer-package-reset/list 真实形状:available 计数(显式 false 排除,缺省算可用)。
static void test_resets_parse(void)
{
    const char *body =
        "{\"code\":200,\"msg\":\"操作成功\",\"data\":{"
        "\"customerId\":123,\"targetType\":\"TEAM\","
        "\"lastFiveHourResetTime\":null,\"lastWeekResetTime\":\"2026-09-20 23:02:43\","
        "\"fiveHourResets\":["
        "{\"recordId\":1,\"grantType\":\"DIRECT\",\"expireTime\":\"2026-10-18 20:25:45\",\"available\":true},"
        "{\"recordId\":2,\"grantType\":\"DIRECT\",\"expireTime\":\"2026-10-28 11:00:44\",\"available\":true},"
        "{\"recordId\":3,\"grantType\":\"DIRECT\",\"expireTime\":\"2026-10-28 11:00:44\",\"available\":false}"
        "],"
        "\"weekResets\":["
        "{\"recordId\":4,\"grantType\":\"DIRECT\",\"expireTime\":\"2026-10-20 23:02:43\",\"available\":true}"
        "]}}";
    int h5 = -1, wk = -1;
    CHECK(glm_resets_parse(body, strlen(body), &h5, &wk));
    CHECK(h5 == 2); // 3 条里 1 条 available:false
    CHECK(wk == 1);
    CHECK(glm_resets_parse(body, strlen(body), NULL, NULL)); // 允许只统计不取值

    // 缺数组 → -1;非 JSON → false。
    const char *empty = "{\"code\":200,\"data\":{\"targetType\":\"TEAM\"}}";
    CHECK(glm_resets_parse(empty, strlen(empty), &h5, &wk));
    CHECK(h5 == -1 && wk == -1);
    CHECK(!glm_resets_parse("nope", 4, &h5, &wk));
}

int main(void)
{
    test_parse_ok();
    test_parse_credit_limit();
    test_customer_projects();
    test_resets_parse();
    test_parse_order_swapped();
    test_parse_missing_reset();
    test_parse_auth_error();
    test_parse_invalid();
    test_parse_http_error_code();
    test_format_reset();
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("test_glm_usage_parse: all checks passed\n");
    return 0;
}
