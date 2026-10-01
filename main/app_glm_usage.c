// main/app_glm_usage.c —— 见 app_glm_usage.h 顶部说明。
//
// 实现说明:JSON 解析用 cJSON(IDF 侧链接 json 组件;主机测试直接编译
// tests/thirdparty/cJSON/cJSON.c,两端同一份应用源码,保证测试对象与固件一致)。
#include "app_glm_usage.h"

#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool glm_usage_parse(const char *body, size_t body_len, glm_usage_t *out)
{
    if (!body || !out || body_len == 0) return false;
    memset(out, 0, sizeof(*out));
    out->http_code = -1;
    out->tokens_5h_used_pct = -1;
    out->tokens_week_used_pct = -1;
    out->tokens_5h_reset_ms = -1;
    out->tokens_week_reset_ms = -1;
    out->mcp_used = -1;
    out->mcp_total = -1;
    out->mcp_remaining = -1;
    out->tokens_5h_remaining = -1;
    out->tokens_week_remaining = -1;
    out->five_hour_resets_left = -1;
    out->week_resets_left = -1;

    // 响应体理论上以 NUL 结尾更省事,但 HTTP 分块时不能保证,这里复制一份补 NUL。
    // 典型响应 <1KB,堆开销可接受;C3 堆紧张时此处是单次、短生命周期的分配。
    char *text = malloc(body_len + 1);
    if (!text) return false;
    memcpy(text, body, body_len);
    text[body_len] = '\0';

    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) return false;

    bool parsed = false;
    // 业务消息(错误原因)原样带出给界面显示;超长截断。内容来自服务端,
    // 属于动态中文文本,超出字体子集的字符由 fallback 链显示占位符。
    cJSON *msg = cJSON_GetObjectItemCaseSensitive(root, "msg");
    if (cJSON_IsString(msg) && msg->valuestring) {
        strncpy(out->msg, msg->valuestring, sizeof(out->msg) - 1);
    }
    // 业务码可能缺;拿不到就保持 -1,由上层按"无效响应"处理。
    cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (cJSON_IsNumber(code)) {
        out->http_code = code->valueint;
        parsed = true;
    }

    // level 与 limits 都挂在 data 下。
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (cJSON_IsObject(data)) {
        cJSON *lv = cJSON_GetObjectItemCaseSensitive(data, "level");
        if (cJSON_IsString(lv) && lv->valuestring) {
            strncpy(out->level, lv->valuestring, sizeof(out->level) - 1);
            parsed = true;
        }
        cJSON *limits = cJSON_GetObjectItemCaseSensitive(data, "limits");
        if (cJSON_IsArray(limits)) {
            // 收集窗口额度项(TOKENS_LIMIT=个人套餐 token,或 CREDIT_LIMIT=团队
            // 套餐点数)与 TIME_LIMIT(MCP 每月次数)各一。约定:重置时间早的是
            // 5 小时窗口;若 nextResetTime 缺失(-1),保持数组先后顺序。
            // n_tok 计数驱动槽位:第 1 项进 5h 槽;第 2 项进周槽(但若它重置更早
            // 则与 5h 槽互换);更多项时维持"5h=最早,周=次早"不变量。
            typedef struct {
                int64_t reset;  // 重置时间(Unix 毫秒,-1 缺失)
                int pct;        // 已用百分比(-1 缺失)
                int remaining;  // 剩余点数/次数(-1 缺失;团队套餐 CREDIT_LIMIT 才有)
            } win_t;
            win_t w5 = { -1, -1, -1 }, ww = { -1, -1, -1 };
            int n_tok = 0;
            cJSON *item;
            cJSON_ArrayForEach(item, limits) {
                cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
                if (!cJSON_IsString(type) || !type->valuestring) continue;
                if (strcmp(type->valuestring, "TIME_LIMIT") == 0) {
                    cJSON *v;
                    if ((v = cJSON_GetObjectItemCaseSensitive(item, "currentValue")) && cJSON_IsNumber(v))
                        out->mcp_used = v->valueint;
                    if ((v = cJSON_GetObjectItemCaseSensitive(item, "usage")) && cJSON_IsNumber(v))
                        out->mcp_total = v->valueint;
                    if ((v = cJSON_GetObjectItemCaseSensitive(item, "remaining")) && cJSON_IsNumber(v))
                        out->mcp_remaining = v->valueint;
                    parsed = true;
                    continue;
                }
                bool is_window =
                    strcmp(type->valuestring, "TOKENS_LIMIT") == 0 ||
                    strcmp(type->valuestring, "CREDIT_LIMIT") == 0;
                if (!is_window) continue;
                cJSON *p = cJSON_GetObjectItemCaseSensitive(item, "percentage");
                cJSON *r = cJSON_GetObjectItemCaseSensitive(item, "nextResetTime");
                cJSON *rem = cJSON_GetObjectItemCaseSensitive(item, "remaining");
                win_t w = {
                    .reset = cJSON_IsNumber(r) ? (int64_t)r->valuedouble : -1,
                    .pct = cJSON_IsNumber(p) ? p->valueint : -1,
                    .remaining = cJSON_IsNumber(rem) ? rem->valueint : -1,
                };
                if (n_tok == 0) {
                    w5 = w;
                } else if (n_tok == 1) {
                    if (w.reset >= 0 && w5.reset >= 0 && w.reset < w5.reset) {
                        ww = w5; w5 = w;
                    } else {
                        ww = w;
                    }
                } else if (w.reset >= 0) {
                    if (w5.reset >= 0 && w.reset < w5.reset) {
                        ww = w5; w5 = w;
                    } else if (ww.reset < 0 || w.reset < ww.reset) {
                        ww = w;
                    }
                }
                n_tok++;
            }
            if (n_tok > 0) {
                out->tokens_5h_reset_ms = w5.reset;
                out->tokens_5h_used_pct = w5.pct;
                out->tokens_5h_remaining = w5.remaining;
                out->tokens_week_reset_ms = ww.reset;
                out->tokens_week_used_pct = ww.pct;
                out->tokens_week_remaining = ww.remaining;
                parsed = true;
            }
        }
    }
    cJSON_Delete(root);
    if (out->http_code >= 200 && out->http_code < 300) out->ok = true;
    return parsed;
}

bool glm_resets_parse(const char *body, size_t body_len,
                      int *out_5h, int *out_week)
{
    if (out_5h) *out_5h = -1;
    if (out_week) *out_week = -1;
    if (!body || body_len == 0) return false;
    char *text = malloc(body_len + 1);
    if (!text) return false;
    memcpy(text, body, body_len);
    text[body_len] = '\0';
    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) return false;

    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    bool ok = false;
    if (cJSON_IsObject(data)) {
        ok = true; // data 对象存在即视为解析成功;缺数组以 -1 表示"未提供"
        // available 可能缺省(视为 true,与控制台口径一致);仅显式 false 才排除。
        static const char *pairs[][2] = {
            { "fiveHourResets", "5h" },
            { "weekResets", "week" },
        };
        for (size_t i = 0; i < 2; i++) {
            cJSON *arr = cJSON_GetObjectItemCaseSensitive(data, pairs[i][0]);
            int count = -1;
            if (cJSON_IsArray(arr)) {
                count = 0;
                cJSON *it;
                cJSON_ArrayForEach(it, arr) {
                    cJSON *av = cJSON_GetObjectItemCaseSensitive(it, "available");
                    if (!cJSON_IsBool(av) || cJSON_IsTrue(av)) count++;
                }
                ok = true;
            }
            if (i == 0 && out_5h) *out_5h = count;
            if (i == 1 && out_week) *out_week = count;
        }
    }
    cJSON_Delete(root);
    return ok;
}

bool glm_customer_parse_projects(const char *body, size_t body_len,
                                 char *out, size_t out_len)
{
    if (!body || !out || out_len == 0) return false;
    out[0] = '\0';
    char *text = malloc(body_len + 1);
    if (!text) return false;
    memcpy(text, body, body_len);
    text[body_len] = '\0';

    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) return false;

    // 结构:data.organizations[] → {organizationName, organizationId,
    // projects:[{projectName, projectId}]}。逐个拷贝到紧凑输出。
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    cJSON *orgs = cJSON_IsObject(data)
                      ? cJSON_GetObjectItemCaseSensitive(data, "organizations")
                      : NULL;
    cJSON *out_arr = cJSON_CreateArray();
    bool ok = false;
    if (cJSON_IsArray(orgs) && out_arr) {
        cJSON *o;
        cJSON_ArrayForEach(o, orgs) {
            cJSON *oid = cJSON_GetObjectItemCaseSensitive(o, "organizationId");
            cJSON *oname = cJSON_GetObjectItemCaseSensitive(o, "organizationName");
            if (!cJSON_IsString(oid) || !oid->valuestring) continue;
            cJSON *entry = cJSON_CreateObject();
            cJSON_AddStringToObject(entry, "orgId", oid->valuestring);
            cJSON_AddStringToObject(entry, "orgName",
                                    cJSON_IsString(oname) && oname->valuestring ? oname->valuestring : "");
            cJSON *plist = cJSON_GetObjectItemCaseSensitive(o, "projects");
            cJSON *out_projects = cJSON_AddArrayToObject(entry, "projects");
            if (cJSON_IsArray(plist) && out_projects) {
                cJSON *p;
                cJSON_ArrayForEach(p, plist) {
                    cJSON *pid = cJSON_GetObjectItemCaseSensitive(p, "projectId");
                    if (!cJSON_IsString(pid) || !pid->valuestring) continue;
                    cJSON *pname = cJSON_GetObjectItemCaseSensitive(p, "projectName");
                    cJSON *pe = cJSON_CreateObject();
                    cJSON_AddStringToObject(pe, "id", pid->valuestring);
                    cJSON_AddStringToObject(pe, "name",
                                            cJSON_IsString(pname) && pname->valuestring ? pname->valuestring : "");
                    cJSON_AddItemToArray(out_projects, pe);
                }
            }
            cJSON_AddItemToArray(out_arr, entry);
            ok = true;
        }
    }
    char *printed = out_arr ? cJSON_PrintUnformatted(out_arr) : NULL;
    if (printed) {
        ok = strlen(printed) < out_len;
        if (ok) snprintf(out, out_len, "%s", printed);
        cJSON_free(printed);
    }
    cJSON_Delete(out_arr);
    cJSON_Delete(root);
    return ok;
}

bool glm_usage_is_auth_error(const glm_usage_t *u)
{
    if (!u) return false;
    return u->http_code == 401 || u->http_code == 403 || u->http_code == 1001;
}

char *glm_usage_format_reset_ms(int64_t reset_ms, char *buf, size_t buf_len)
{
    if (!buf || buf_len < 12) return buf;
    if (reset_ms <= 0) {
        snprintf(buf, buf_len, "--");
        return buf;
    }
    // 东八区 = UTC + 8h;毫秒转秒后加偏移,再拆月日时分(年通过天数反推后丢弃)。
    int64_t secs = reset_ms / 1000 + 8 * 3600;
    int64_t days = secs / 86400;
    int64_t rem = secs % 86400;
    if (days < 0) { snprintf(buf, buf_len, "--"); return buf; }

    // 从 1970-01-01 反推年月日(民用历法算法,Public Domain,Howard Hinnant)。
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = mp < 10 ? mp + 3 : mp - 9;

    snprintf(buf, buf_len, "%02lld-%02lld %02lld:%02lld",
             (long long)m, (long long)d, (long long)(rem / 3600), (long long)((rem % 3600) / 60));
    return buf;
}
