// main/app_netlist.c —— 已保存热点列表的纯逻辑实现,见 app_netlist.h。
#include "app_netlist.h"

#include <stdio.h>
#include <string.h>

void app_netlist_reset(app_netlist_t *list)
{
    if (!list) return;
    memset(list, 0, sizeof(*list));
    list->selected = -1;
}

bool app_netlist_add(app_netlist_t *list, const char *ssid, const char *pwd)
{
    if (!list || !ssid || ssid[0] == '\0') return false;
    if (strlen(ssid) >= APP_NETLIST_SSID_MAX) return false;
    if (pwd && strlen(pwd) >= APP_NETLIST_PWD_MAX) return false;

    // 同名热点视为修改密码,不新增条目(配网页重复勾选的语义)。
    for (uint8_t i = 0; i < list->count; i++) {
        if (strcmp(list->items[i].ssid, ssid) == 0) {
            snprintf(list->items[i].pwd, APP_NETLIST_PWD_MAX, "%s", pwd ? pwd : "");
            return true;
        }
    }
    if (list->count >= APP_NETLIST_MAX) return false;
    app_netlist_entry_t *e = &list->items[list->count++];
    snprintf(e->ssid, APP_NETLIST_SSID_MAX, "%s", ssid);
    snprintf(e->pwd, APP_NETLIST_PWD_MAX, "%s", pwd ? pwd : "");
    return true;
}

bool app_netlist_remove(app_netlist_t *list, uint8_t index)
{
    if (!list || index >= list->count) return false;

    // 点选态追随 SSID:先记住点选的 SSID,移位完成后在剩余条目里重新定位;
    // 若点选的正是被删项,自然找不到 → 复位 -1。(不能先按新 count 收缩:
    // 条目前移会让旧下标越界,把仍然有效的点选误清成 -1。)
    char sel_ssid[APP_NETLIST_SSID_MAX] = { 0 };
    bool had_sel = list->selected >= 0 && list->selected < (int8_t)list->count;
    if (had_sel) {
        snprintf(sel_ssid, sizeof(sel_ssid), "%s", list->items[list->selected].ssid);
    }

    // 后续条目前移覆盖被删项,保持保存顺序。
    for (uint8_t i = index; i + 1 < list->count; i++) {
        list->items[i] = list->items[i + 1];
    }
    list->count--;
    memset(&list->items[list->count], 0, sizeof(list->items[list->count]));

    list->selected = -1;
    if (had_sel) {
        for (uint8_t i = 0; i < list->count; i++) {
            if (strcmp(list->items[i].ssid, sel_ssid) == 0) {
                list->selected = (int8_t)i;
                break;
            }
        }
    }
    return true;
}

bool app_netlist_select(app_netlist_t *list, const char *ssid)
{
    if (!list || !ssid) return false;
    for (uint8_t i = 0; i < list->count; i++) {
        if (strcmp(list->items[i].ssid, ssid) == 0) {
            list->selected = (int8_t)i;
            return true;
        }
    }
    return false;
}

bool app_netlist_next_target(const app_netlist_t *list, uint8_t attempt,
                             app_netlist_entry_t *out)
{
    if (!list || !out || list->count == 0) return false;
    // 尝试序号 → 实际条目:第 0 次是 selected(若有效),其后按保存顺序轮转。
    // 这样"点选某热点"立即生效,又保证自动回退能遍历全部已存热点。
    uint8_t start = (list->selected >= 0 && list->selected < (int8_t)list->count)
                        ? (uint8_t)list->selected : 0;
    uint8_t idx = (uint8_t)((start + attempt) % list->count);
    *out = list->items[idx];
    return true;
}

bool app_netlist_serialize(const app_netlist_t *list, char *buf, size_t buf_len)
{
    if (!list || !buf || buf_len == 0) return false;
    size_t used = 0;
    buf[0] = '\0';
    for (uint8_t i = 0; i < list->count; i++) {
        const app_netlist_entry_t *e = &list->items[i];
        size_t ssid_len = strlen(e->ssid);
        size_t pwd_len = strlen(e->pwd);
        // 每条 "<len>:<ssid>:<len>:<pwd>" 加分隔符;写入前检查剩余空间,
        // 不足即失败 —— 调用方给定 APP_NETLIST_BLOB_MAX 时永远不会发生。
        int n = snprintf(buf + used, buf_len - used, "%u:%s:%u:%s",
                         (unsigned)ssid_len, e->ssid, (unsigned)pwd_len, e->pwd);
        if (n < 0 || (size_t)n >= buf_len - used) return false;
        used += (size_t)n;
        if (i + 1 < list->count) {
            if (used + 1 >= buf_len) return false;
            buf[used++] = '|';
            buf[used] = '\0';
        }
    }
    return true;
}

bool app_netlist_deserialize(const char *blob, app_netlist_t *list)
{
    if (!blob || !list) return false;
    app_netlist_reset(list);
    if (blob[0] == '\0') return true; // 空串 = 空列表,合法

    const char *p = blob;
    while (*p) {
        unsigned ssid_len = 0, pwd_len = 0;
        char ssid[APP_NETLIST_SSID_MAX];
        char pwd[APP_NETLIST_PWD_MAX];

        // 严格按 "<len>:<data>:<len>:<data>" 逐字段读取;任何一步畸形即整体失败,
        // 不做"尽力还原"—— 半损坏的列表比空列表更危险(会把坏密码当真)。
        int consumed = 0;
        if (sscanf(p, "%u%n", &ssid_len, &consumed) != 1) return false;
        p += consumed;
        if (*p != ':' || ssid_len >= APP_NETLIST_SSID_MAX) return false;
        p++;
        if (strlen(p) < ssid_len) return false;
        memcpy(ssid, p, ssid_len);
        ssid[ssid_len] = '\0';
        p += ssid_len;
        if (*p != ':') return false;
        p++;
        if (sscanf(p, "%u%n", &pwd_len, &consumed) != 1) return false;
        p += consumed;
        if (*p != ':' || pwd_len >= APP_NETLIST_PWD_MAX) return false;
        p++;
        if (strlen(p) < pwd_len) return false;
        memcpy(pwd, p, pwd_len);
        pwd[pwd_len] = '\0';
        p += pwd_len;

        if (!app_netlist_add(list, ssid, pwd)) return false;
        if (*p == '|') p++;
        else if (*p != '\0') return false;
    }
    return true;
}
