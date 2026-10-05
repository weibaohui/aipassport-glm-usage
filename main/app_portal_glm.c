// main/app_portal_glm.c —— GLM 专属门户内容,见 app_portal_glm.h。
//
// 片段里只放框架模板没有的东西(Key/团队两张卡);周期/熄屏下拉、WiFi 管理、
// 导出导入清除都由框架模板自带,不要在这里重复。片段经 <!--APP_CONFIG_HTML-->
// 注入点进入阶段二容器,$/esc/jget/jpost 由框架公共脚本先行定义。
#include "app_portal_glm.h"

#include <stdlib.h>
#include <string.h>

#include "app_storage.h"
#include "appfw_net.h"
#include "app_glm_client.h"
#include "cJSON.h"
#include "esp_http_server.h"

// 门户片段:两张 GLM 卡片 + 脚本(徽标/回显/保存/清除/团队发现)。
// 周期与熄屏的回显由框架状态接口自带(period_s/screen_off_s),下拉也由
// 框架模板自带,这里不再重复。
static const char GLM_FRAGMENT[] =
    "<div class=\"card\"><h2>1 · API Key <span id=\"kbadge\" class=\"badge\">…</span></h2>\n"
    "<input type=\"text\" id=\"key\" placeholder=\"粘贴智谱 API Key(bigmodel.cn)\">\n"
    "<button onclick=\"saveKey()\">保存</button>\n"
    "<button class=\"danger\" onclick=\"glmClear()\">清除 Key 与团队</button>\n"
    "<small>保存后设备数秒内开始查询并显示在屏幕上;个人套餐填到这里即可</small></div>\n"
    "\n"
    "<details class=\"card\"><summary style=\"font-size:15px;color:#7fd4a0\">2 · 团队上下文(选填) <span id=\"tbadge\" class=\"badge\">…</span></summary>\n"
    "<input type=\"text\" id=\"org\" placeholder=\"组织 ID org-…\">\n"
    "<input type=\"text\" id=\"project\" placeholder=\"项目 ID proj-…\">\n"
    "<button onclick=\"saveKey()\">保存</button>\n"
    "<small>个人套餐跳过;两个 ID 可手动填写,也可粘贴登录 Token 自动填入</small>\n"
    "<details><summary>粘贴登录 Token 自动填入(团队套餐推荐)</summary>\n"
    "<input type=\"password\" id=\"jwt\" placeholder=\"粘贴 bigmodel.cn 网页登录 Token(F12 → authorization 头)\">\n"
    "<button class=\"ghost\" onclick=\"discover()\">获取组织 / 项目</button>\n"
    "<div id=\"orgsel\"></div><div id=\"projsel\"></div>\n"
    "<small>Token 不存储,仅用于获取团队 ID、项目 ID</small></details></details>\n"
    "\n"
    "<script>\n"
    "let orgList=[];\n"
    "function glmBadge(){\n"
    "  jget('/api/status').then(s=>{\n"
    "    badge('kbadge',!!s.key);\n"
    "    badge('tbadge',!!(s.org&&s.project));\n"
    "    if(!$('key').value&&s.key)$('key').value=s.key;\n"
    "    if(!$('org').value&&s.org)$('org').value=s.org;\n"
    "    if(!$('project').value&&s.project)$('project').value=s.project;\n"
    "  }).catch(()=>{});\n"
    "}\n"
    "async function saveKey(){\n"
    "  const r=await jpost('/api/glm',{key:$('key').value.trim(),org:$('org').value.trim(),project:$('project').value.trim()});\n"
    "  if(r.ok){alert('已保存。设备数秒内开始查询用量,请看屏幕');glmBadge();}\n"
    "  else alert('保存失败(长度超限?)');\n"
    "}\n"
    "async function glmClear(){\n"
    "  if(!confirm('确定清除 API Key 与团队上下文?'))return;\n"
    "  const r=await jpost('/api/glm',{clear:true});\n"
    "  if(r.ok){$('key').value='';$('org').value='';$('project').value='';badge('kbadge',false);badge('tbadge',false);alert('已清除');}\n"
    "  else alert('清除失败');\n"
    "}\n"
    "async function discover(){\n"
    "  const j=$('jwt').value.trim();\n"
    "  if(!j){alert('请先粘贴登录 Token');return}\n"
    "  $('orgsel').innerHTML='<small>获取中…</small>';$('projsel').innerHTML='';\n"
    "  const r=await jpost('/api/discover',{jwt:j});\n"
    "  if(r.error){$('orgsel').innerHTML='<span class=err>'+esc(r.error)+'</span>';return}\n"
    "  orgList=r||[];\n"
    "  if(!orgList.length){$('orgsel').innerHTML='<span class=err>未发现组织</span>';return}\n"
    "  let oh='<select id=\"orgpick\" onchange=\"orgPicked()\">';\n"
    "  orgList.forEach((o,i)=>{oh+='<option value='+i+'>'+esc(o.orgName||o.orgId)+'</option>'});\n"
    "  oh+='</select>';\n"
    "  $('orgsel').innerHTML=oh;orgPicked();\n"
    "}\n"
    "function orgPicked(){\n"
    "  const o=orgList[$('orgpick').value]||{projects:[]};\n"
    "  let ph='<select id=\"projpick\" onchange=\"projPicked()\">';\n"
    "  (o.projects||[]).forEach((pj,i)=>{ph+='<option value='+i+'>'+esc(pj.name||pj.id)+'</option>'});\n"
    "  ph+='</select>';\n"
    "  $('projsel').innerHTML=ph;projPicked();\n"
    "}\n"
    "function projPicked(){\n"
    "  const o=orgList[$('orgpick').value];const pj=(o.projects||[])[$('projpick').value];\n"
    "  if(!o||!pj)return;\n"
    "  $('org').value=o.orgId;$('project').value=pj.id;\n"
    "  $('status').innerHTML='<span class=ok>已选择 '+esc(o.orgName)+' / '+esc(pj.name)+',填好 API Key 后点保存</span>';\n"
    "}\n"
    "glmBadge();\n"
    "</script>\n";

static const char *glm_config_html(void)
{
    return GLM_FRAGMENT;
}

// POST /api/glm:保存 Key(空串=不改动)与团队上下文,或 {clear:true} 清除。
static esp_err_t handler_glm(httpd_req_t *req)
{
    cJSON *root = appfw_prov_read_json(req);
    if (!root) return ESP_FAIL;
    cJSON *clr = cJSON_GetObjectItemCaseSensitive(root, "clear");
    if (cJSON_IsTrue(clr)) {
        cJSON_Delete(root);
        appfw_prov_send_ok(req, app_storage_clear_glm());
        return ESP_OK;
    }
    cJSON *key = cJSON_GetObjectItemCaseSensitive(root, "key");
    if (!cJSON_IsString(key) || !key->valuestring ||
        strlen(key->valuestring) >= APP_STORAGE_API_KEY_MAX) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "key invalid");
        return ESP_FAIL;
    }
    cJSON *org = cJSON_GetObjectItemCaseSensitive(root, "org");
    cJSON *proj = cJSON_GetObjectItemCaseSensitive(root, "project");
    const char *org_s = (cJSON_IsString(org) && org->valuestring) ? org->valuestring : "";
    const char *proj_s = (cJSON_IsString(proj) && proj->valuestring) ? proj->valuestring : "";
    if (strlen(org_s) >= APP_ORG_ID_MAX || strlen(proj_s) >= APP_PROJ_ID_MAX) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "org/project too long");
        return ESP_FAIL;
    }
    // Key 为空串 = 不改动已存的 Key(用于只改团队上下文);非空才覆盖。
    bool ok = true;
    if (key->valuestring[0] != '\0') ok = app_storage_save_api_key(key->valuestring);
    ok = ok && app_storage_save_org_project(org_s, proj_s);
    if (ok) {
        // 保存即生效:唤醒查询任务立刻用新 Key/组织重试。
        app_glm_client_refresh_now();
    }
    cJSON_Delete(root);
    appfw_prov_send_ok(req, ok);
    return ESP_OK;
}

#define APP_JWT_MAX 1024        // 网页 JWT ~400 字符,留裕量
#define APP_DISCOVER_OUT_MAX 2048

// POST /api/discover:网页登录 Token 换组织/项目清单(Token 不存储)。
static esp_err_t handler_discover(httpd_req_t *req)
{
    // 发现需要设备自身有外网(拿网页 Token 调控制台接口)。配网热点模式下
    // 设备尚未联网,提前给出明确指引而不是让用户等超时。
    appfw_net_status_t st;
    appfw_net_get_status(&st);
    if (st.state != APPFW_NET_ONLINE) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req,
            "{\"error\":\"设备还未联网:请先完成热点配网并连接,之后用屏幕上显示的局域网 IP 重新打开本页再试\"}",
            HTTPD_RESP_USE_STRLEN);
    }
    cJSON *root = appfw_prov_read_json(req);
    if (!root) return ESP_FAIL;
    cJSON *jwt = cJSON_GetObjectItemCaseSensitive(root, "jwt");
    if (!cJSON_IsString(jwt) || !jwt->valuestring || strlen(jwt->valuestring) >= APP_JWT_MAX) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "jwt invalid");
        return ESP_FAIL;
    }
    char *out = malloc(APP_DISCOVER_OUT_MAX);
    if (!out) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    int rc = app_glm_client_discover_projects(jwt->valuestring, out, APP_DISCOVER_OUT_MAX);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret;
    if (rc == ESP_OK) {
        ret = httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    } else if (rc == 1) {
        ret = httpd_resp_send(req, "{\"error\":\"登录 Token 已过期,请在浏览器重新复制\"}",
                              HTTPD_RESP_USE_STRLEN);
    } else {
        ret = httpd_resp_send(req, "{\"error\":\"获取失败,请检查网络后重试\"}",
                              HTTPD_RESP_USE_STRLEN);
    }
    free(out);
    return ret;
}

bool glm_portal_ready(void *httpd)
{
    static const httpd_uri_t routes[] = {
        { .uri = "/api/glm",     .method = HTTP_POST, .handler = handler_glm },
        { .uri = "/api/discover", .method = HTTP_POST, .handler = handler_discover },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        if (httpd_register_uri_handler(httpd, &routes[i]) != ESP_OK) return false;
    }
    return true;
}

// 状态回显与导出:GLM 字段进 /api/status 与 /api/config/export(Key 回显是
// 用户明确要求的老行为:页面填好 Key,用户核对而不是盲打)。
static void glm_config_fill(void *obj)
{
    cJSON *root = (cJSON *)obj;
    char key[APP_STORAGE_API_KEY_MAX];
    if (app_storage_load_api_key(key, sizeof(key))) {
        cJSON_AddStringToObject(root, "key", key);
    }
    char org[64] = { 0 }, proj[64] = { 0 };
    app_storage_load_org(org, sizeof(org));
    app_storage_load_project(proj, sizeof(proj));
    cJSON_AddStringToObject(root, "org", org);
    cJSON_AddStringToObject(root, "project", proj);
}

// 配置导入:应用自有字段(api_key/org/project;缺省不改)。
static bool glm_config_apply(void *root_obj)
{
    cJSON *root = (cJSON *)root_obj;
    if (!cJSON_IsObject(root)) return true;
    cJSON *key = cJSON_GetObjectItemCaseSensitive(root, "api_key");
    cJSON *org = cJSON_GetObjectItemCaseSensitive(root, "org");
    cJSON *proj = cJSON_GetObjectItemCaseSensitive(root, "project");
    bool ok = true;
    if (cJSON_IsString(key) && key->valuestring[0]) {
        ok = app_storage_save_api_key(key->valuestring) && ok;
    }
    if (cJSON_IsString(org) || cJSON_IsString(proj)) {
        ok = app_storage_save_org_project(
                 cJSON_IsString(org) && org->valuestring ? org->valuestring : "",
                 cJSON_IsString(proj) && proj->valuestring ? proj->valuestring : "") && ok;
    }
    if (ok) app_glm_client_refresh_now();
    return ok;
}

void app_portal_glm_configure(appfw_prov_cfg_t *cfg)
{
    cfg->app_config_html = glm_config_html;
    cfg->app_config_fill = glm_config_fill;
    cfg->app_config_apply = glm_config_apply;
    cfg->on_httpd_ready = glm_portal_ready;
}
