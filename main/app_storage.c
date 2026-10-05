// main/app_storage.c —— GLM 私有键("glm" 命名空间)的 NVS 读写实现。
// 通用配置(热点/周期/熄屏)已归框架 appfw_storage,见 app_storage.h 说明。
#include "app_storage.h"

#include <string.h>

#include "nvs.h"

static const char *NS = "glm";   // 命名空间:升级自旧版固件时这些键原样继承

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
    return nvs_set_str_all("org_id", org_id) && nvs_set_str_all("proj_id", project_id);
}

bool app_storage_clear_glm(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    // 逐键擦除而不是 erase_all:命名空间里将来可能还有别的键。
    esp_err_t err = nvs_erase_key(h, "api_key");
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    esp_err_t e2 = nvs_erase_key(h, "org_id");
    if (e2 == ESP_ERR_NVS_NOT_FOUND) e2 = ESP_OK;
    esp_err_t e3 = nvs_erase_key(h, "proj_id");
    if (e3 == ESP_ERR_NVS_NOT_FOUND) e3 = ESP_OK;
    if (err == ESP_OK) err = e2;
    if (err == ESP_OK) err = e3;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}
