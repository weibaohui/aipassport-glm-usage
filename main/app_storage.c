// main/app_storage.c —— NVS 持久化实现,键与语义见 app_storage.h。
#include "app_storage.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "app_storage";
static const char *NS = "glm";   // 命名空间:本应用所有键集中于此,清除时整体擦除

int app_storage_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // 分区格式变化(例如从旧固件/合并镜像整体刷入后):标准流程是擦除重试。
        // 仅这两种错误允许擦除;其他错误原样返回,不做破坏性操作。
        ESP_LOGW(TAG, "NVS 分区需重建(%s),擦除后重试", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err != ESP_OK) return err;
        err = nvs_flash_init();
    }
    return err;
}

static bool nvs_get_str_all(const char *key, char *buf, size_t buf_len)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t needed = buf_len;
    esp_err_t err = nvs_get_str(h, key, buf, &needed);
    nvs_close(h);
    if (err != ESP_OK) {
        buf[0] = '\0';
        return false;
    }
    return true;
}

static bool nvs_set_str_all(const char *key, const char *value)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_str(h, key, value ? value : "");
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

bool app_storage_load_api_key(char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0) return false;
    buf[0] = '\0';
    return nvs_get_str_all("api_key", buf, buf_len) && buf[0] != '\0';
}

bool app_storage_save_api_key(const char *api_key)
{
    return nvs_set_str_all("api_key", api_key);
}

bool app_storage_load_netlist(app_netlist_t *list)
{
    if (!list) return false;
    app_netlist_reset(list);
    // 1.1KB 栈缓冲:本函数会被 LVGL 轮询与门户 HTTP 两个任务并发调用,
    // 不能用 static(非线程安全);两个调用方栈均在 4KB 以上,可承受。
    char blob[APP_NETLIST_BLOB_MAX];
    if (!nvs_get_str_all("nets", blob, sizeof(blob))) return false;
    if (!app_netlist_deserialize(blob, list)) {
        ESP_LOGW(TAG, "已存热点列表损坏,已丢弃");
        app_netlist_reset(list);
        return false;
    }
    // 恢复持久化的点选状态。
    char sel[APP_NETLIST_SSID_MAX] = { 0 };
    if (nvs_get_str_all("sel_ssid", sel, sizeof(sel)) && sel[0] != '\0') {
        (void)app_netlist_select(list, sel);
    }
    return true;
}

bool app_storage_save_netlist(const app_netlist_t *list)
{
    if (!list) return false;
    char blob[APP_NETLIST_BLOB_MAX];
    if (!app_netlist_serialize(list, blob, sizeof(blob))) {
        ESP_LOGE(TAG, "热点列表序列化失败(不应发生)");
        return false;
    }
    if (!nvs_set_str_all("nets", blob)) return false;
    const char *sel = (list->selected >= 0 && list->selected < (int)list->count)
                          ? list->items[list->selected].ssid : "";
    return nvs_set_str_all("sel_ssid", sel);
}

bool app_storage_load_org(char *org_buf, size_t org_len)
{
    if (!org_buf || org_len == 0) return false;
    org_buf[0] = '\0';
    return nvs_get_str_all("org_id", org_buf, org_len) && org_buf[0] != '\0';
}

bool app_storage_load_project(char *proj_buf, size_t proj_len)
{
    if (!proj_buf || proj_len == 0) return false;
    proj_buf[0] = '\0';
    return nvs_get_str_all("proj_id", proj_buf, proj_len) && proj_buf[0] != '\0';
}

bool app_storage_save_org_project(const char *org_id, const char *project_id)
{
    // 两者必须成对配置(接口要求同时带组织与项目头);任一为空即整体清除。
    if (!org_id || !project_id || org_id[0] == '\0' || project_id[0] == '\0') {
        return nvs_set_str_all("org_id", "") && nvs_set_str_all("proj_id", "");
    }
    return nvs_set_str_all("org_id", org_id) && nvs_set_str_all("proj_id", project_id);
}

bool app_storage_load_period(uint16_t *period_s)
{
    if (!period_s) return false;
    *period_s = 60;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    uint16_t v = 0;
    esp_err_t err = nvs_get_u16(h, "period_s", &v);
    nvs_close(h);
    if (err != ESP_OK || v == 0) return false;
    *period_s = v;
    return true;
}

bool app_storage_save_period(uint16_t period_s)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_u16(h, "period_s", period_s);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

bool app_storage_clear_all(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_erase_all(h);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}
