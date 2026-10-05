// main/app_glm_mcp.c —— GLM 业务的 MCP 工具实现,见 app_glm_mcp.h。
#include "app_glm_mcp.h"

#include <stdio.h>
#include <string.h>

#include "app_storage.h"
#include "appfw_mcp.h"
#include "appfw_net.h"
#include "app_glm_client.h"
#include "esp_err.h"

// Key 掩码(abcd…wxyz;不足 8 位只显示前 2 位)。
static void key_mask(char *out, size_t cap)
{
    char key[APP_STORAGE_API_KEY_MAX];
    if (!app_storage_load_api_key(key, sizeof(key))) {
        snprintf(out, cap, "未设置");
        return;
    }
    size_t n = strlen(key);
    if (n >= 8) snprintf(out, cap, "%.4s…%.4s", key, key + n - 4);
    else snprintf(out, cap, "%.2s…", key);
}

static int tool_set_glm_key(cJSON *args, appfw_mcp_resp_t *resp)
{
    const cJSON *k = cJSON_GetObjectItemCaseSensitive(args, "key");
    if (!cJSON_IsString(k) || !k->valuestring[0] ||
        strlen(k->valuestring) >= APP_STORAGE_API_KEY_MAX) {
        appfw_mcp_resp_addf(resp, "参数 key(string,1-%d 字符)缺失或越界",
                            APP_STORAGE_API_KEY_MAX - 1);
        return 1;
    }
    if (!app_storage_save_api_key(k->valuestring)) {
        appfw_mcp_resp_addf(resp, "保存失败(NVS 写入未成功)");
        return 1;
    }
    app_glm_client_refresh_now(); // 保存即生效:查询任务立刻用新 Key 重试
    char mask[24];
    key_mask(mask, sizeof(mask));
    appfw_mcp_resp_addf(resp, "API Key 已保存(%s…),设备数秒内开始查询,可用 glm_usage 查看结果",
                        mask);
    return 0;
}

static int tool_set_glm_team(cJSON *args, appfw_mcp_resp_t *resp)
{
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(args, "org");
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(args, "project");
    const char *org = (cJSON_IsString(o) && o->valuestring) ? o->valuestring : "";
    const char *proj = (cJSON_IsString(p) && p->valuestring) ? p->valuestring : "";
    if (strlen(org) >= 64 || strlen(proj) >= 64) {
        appfw_mcp_resp_addf(resp, "org/project 过长(≤63 字符)");
        return 1;
    }
    if (org[0] == '\0' && proj[0] == '\0') {
        appfw_mcp_resp_addf(resp,
                            "两个 ID 都为空=清除团队上下文(回到个人套餐);要设置请两个都传");
        return 1;
    }
    if (org[0] == '\0' || proj[0] == '\0') {
        appfw_mcp_resp_addf(resp, "org 与 project 必须成对配置(或都传空串清除)");
        return 1;
    }
    if (!app_storage_save_org_project(org, proj)) {
        appfw_mcp_resp_addf(resp, "保存失败(NVS 写入未成功)");
        return 1;
    }
    app_glm_client_refresh_now();
    appfw_mcp_resp_addf(resp, "团队上下文已保存(%s / %s),设备改走团队套餐接口,数秒后生效",
                        org, proj);
    return 0;
}

static int tool_glm_discover(cJSON *args, appfw_mcp_resp_t *resp)
{
    // 发现要拿网页 Token 调控制台接口,设备必须已联网。
    appfw_net_status_t st;
    appfw_net_get_status(&st);
    if (st.state != APPFW_NET_ONLINE) {
        appfw_mcp_resp_addf(resp, "设备未联网,无法发现(先完成 WiFi 配置)");
        return 1;
    }
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(args, "jwt");
    if (!cJSON_IsString(j) || !j->valuestring[0] || strlen(j->valuestring) >= 1024) {
        appfw_mcp_resp_addf(resp, "参数 jwt(string,bigmodel.cn 网页登录 Token)缺失");
        return 1;
    }
    char *out = malloc(2048);
    if (!out) {
        appfw_mcp_resp_addf(resp, "内存不足");
        return 1;
    }
    const int rc = app_glm_client_discover_projects(j->valuestring, out, 2048);
    if (rc == 1) {
        free(out);
        appfw_mcp_resp_addf(resp, "登录 Token 已过期,请在浏览器重新复制");
        return 1;
    }
    if (rc != ESP_OK) {
        free(out);
        appfw_mcp_resp_addf(resp, "获取失败,请检查网络后重试");
        return 1;
    }
    // 精简成 AI 易读的清单;超出 1024 字节的结果缓冲会被截断,所以限量列出。
    appfw_mcp_resp_addf(resp, "发现组织/项目(每组织最多列 4 个项目),用 set_glm_team 保存:\n");
    cJSON *list = cJSON_Parse(out);
    free(out);
    if (!list) {
        appfw_mcp_resp_addf(resp, "响应解析失败,请重试");
        return 1;
    }
    int shown = 0;
    const cJSON *o;
    cJSON_ArrayForEach(o, list) {
        if (shown >= 5) break;
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(o, "orgName");
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(o, "orgId");
        appfw_mcp_resp_addf(resp, "%d. %s(orgId=%s)\n", shown + 1,
                            cJSON_IsString(name) ? name->valuestring : "?",
                            cJSON_IsString(id) ? id->valuestring : "?");
        int pj = 0;
        const cJSON *it;
        const cJSON *projects = cJSON_GetObjectItemCaseSensitive(o, "projects");
        cJSON_ArrayForEach(it, projects) {
            if (pj >= 4) {
                appfw_mcp_resp_addf(resp, "    …\n");
                break;
            }
            const cJSON *pn = cJSON_GetObjectItemCaseSensitive(it, "name");
            const cJSON *pid = cJSON_GetObjectItemCaseSensitive(it, "id");
            appfw_mcp_resp_addf(resp, "    - %s(project=%s)\n",
                                cJSON_IsString(pn) ? pn->valuestring : "?",
                                cJSON_IsString(pid) ? pid->valuestring : "?");
            pj++;
        }
        shown++;
    }
    cJSON_Delete(list);
    return 0;
}

static int tool_glm_usage(cJSON *args, appfw_mcp_resp_t *resp)
{
    (void)args;
    glm_usage_t u;
    glm_err_t err;
    int64_t fetch_epoch_s;
    bool last_ok;
    app_glm_client_get_snapshot(&u, &err, &fetch_epoch_s, &last_ok);

    char mask[24];
    key_mask(mask, sizeof(mask));
    char org[64] = { 0 }, proj[64] = { 0 };
    bool team = app_storage_load_org(org, sizeof(org)) &&
                app_storage_load_project(proj, sizeof(proj));
    appfw_mcp_resp_addf(resp, "Key %s | 套餐模式 %s\n", mask,
                        team ? "团队(type=2)" : "个人");

    if (!last_ok && err != GLM_ERR_NONE) {
        appfw_mcp_resp_addf(resp, "暂无数据(%s)",
                            err == GLM_ERR_WAIT_NET ? "等待网络"
                            : err == GLM_ERR_AUTH   ? "Key 无效或未设置"
                                                    : "查询未成功,稍后再试");
        return 0;
    }
    if (u.tokens_week_used_pct >= 0) {
        appfw_mcp_resp_addf(resp, "本周额度 %d%%", u.tokens_week_used_pct);
    }
    if (u.tokens_5h_used_pct >= 0) {
        appfw_mcp_resp_addf(resp, " | 5小时窗口 %d%%", u.tokens_5h_used_pct);
    }
    if (u.mcp_total > 0) {
        appfw_mcp_resp_addf(resp, " | MCP %d/%d", u.mcp_used, u.mcp_total);
    }
    char buf[16];
    if (u.tokens_week_reset_ms > 0) {
        appfw_mcp_resp_addf(resp, "\n周额度重置 %s",
                            glm_usage_format_reset_ms(u.tokens_week_reset_ms, buf, sizeof(buf)));
    }
    if (u.tokens_5h_reset_ms > 0) {
        appfw_mcp_resp_addf(resp, " | 5h 窗口重置 %s",
                            glm_usage_format_reset_ms(u.tokens_5h_reset_ms, buf, sizeof(buf)));
    }
    if (u.level[0]) appfw_mcp_resp_addf(resp, "\n套餐:%s", u.level);
    return 0;
}

static int tool_clear_glm(cJSON *args, appfw_mcp_resp_t *resp)
{
    (void)args;
    if (!app_storage_clear_glm()) {
        appfw_mcp_resp_addf(resp, "清除失败(NVS 写入未成功)");
        return 1;
    }
    app_glm_client_refresh_now();
    appfw_mcp_resp_addf(resp, "已清除 API Key 与团队上下文(回到个人套餐/未配置态)");
    return 0;
}

static const appfw_mcp_tool_t GLM_TOOLS[] = {
    { "set_glm_key", "保存智谱 API Key(保存即生效,设备数秒内开始查询用量)",
      "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\"}},\"required\":[\"key\"]}",
      tool_set_glm_key },
    { "set_glm_team", "设置团队套餐上下文(组织/项目 ID,成对配置;先 glm_discover 获取)",
      "{\"type\":\"object\",\"properties\":{\"org\":{\"type\":\"string\"},\"project\":{\"type\":\"string\"}},\"required\":[\"org\",\"project\"]}",
      tool_set_glm_team },
    { "glm_discover", "用 bigmodel.cn 网页登录 Token 换取组织/项目清单(Token 不存储;设备需已联网)",
      "{\"type\":\"object\",\"properties\":{\"jwt\":{\"type\":\"string\"}},\"required\":[\"jwt\"]}",
      tool_glm_discover },
    { "glm_usage", "查询当前用量(本周额度/5小时窗口/MCP 调用/套餐与 Key 状态)",
      "{}", tool_glm_usage },
    { "clear_glm", "清除 API Key 与团队上下文(回到未配置态)",
      "{}", tool_clear_glm },
};

void glm_mcp_init(void)
{
    appfw_mcp_set_tools(GLM_TOOLS, (int)(sizeof(GLM_TOOLS) / sizeof(GLM_TOOLS[0])));
}
