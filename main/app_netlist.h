// main/app_netlist.h —— 已保存热点列表的纯逻辑层。
//
// 需求:配网时可勾选多个热点并分别输入密码保存;之后固件可"点选"其中某个
// 热点进行连接;设备启动/断线时按保存顺序逐个尝试(自动回退)。
//
// 列表持久化到 NVS:为规避 NVS key 数量限制与逐条读写的事务复杂度,把整张
// 列表序列化为一条字符串记录,格式:
//   entry := <ssid-len> ':' <ssid> ':' <pwd-len> ':' <pwd>
//   blob  := entry [ '|' entry ]*
// 长度前缀保证 SSID/密码本身含 ':' 或 '|' 时也能无损还原(纯逻辑可测)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 条数上限:配网页一次保存的候选有限;条数越多,断线回退遍历越慢。
#define APP_NETLIST_MAX 8
// SSID 最长 32 字节(802.11 规范);密码:WPA2 最长 64 字节。
#define APP_NETLIST_SSID_MAX 33
#define APP_NETLIST_PWD_MAX 65
// 序列化缓冲:8 × (2+1+32+1+2+1+64) + 7 个分隔符 ≈ 841,取整留裕量。
#define APP_NETLIST_BLOB_MAX 1100

typedef struct {
    char ssid[APP_NETLIST_SSID_MAX]; // NUL 结尾;空串视为无效条目
    char pwd[APP_NETLIST_PWD_MAX];   // NUL 结尾;开放网络允许空串
} app_netlist_entry_t;

typedef struct {
    app_netlist_entry_t items[APP_NETLIST_MAX];
    uint8_t count;                   // 有效条数 0..APP_NETLIST_MAX
    int8_t selected;                 // 用户点选的热点下标;-1 = 未指定(从 0 依次试)
} app_netlist_t;

// 清空列表(count=0,selected=-1)。
void app_netlist_reset(app_netlist_t *list);

// 追加一条(SSID 空串或超长时拒绝)。SSID 已存在时改为覆盖其密码(配网页
// 的"再次勾选同名热点=改密码"语义)。成功返回 true,列表满/参数非法返回 false。
bool app_netlist_add(app_netlist_t *list, const char *ssid, const char *pwd);

// 删除指定下标条目;selected 同步收缩并保持指向同一 SSID(找不到则 -1)。
// 下标越界返回 false。
bool app_netlist_remove(app_netlist_t *list, uint8_t index);

// 设置"点选"的热点(按 SSID 找);找不到返回 false。
bool app_netlist_select(app_netlist_t *list, const char *ssid);

// 取第 i 个尝试目标:优先 selected,其后按保存顺序跳过已选项,填入 out。
// 返回 false 表示列表为空。连接回退顺序的单一事实来源。
bool app_netlist_next_target(const app_netlist_t *list, uint8_t attempt,
                             app_netlist_entry_t *out);

// 序列化/反序列化(见文件头格式说明)。
// 序列化输出以 NUL 结尾;buf_len 不足返回 false(内容可能已部分写入,勿用)。
bool app_netlist_serialize(const app_netlist_t *list, char *buf, size_t buf_len);
bool app_netlist_deserialize(const char *blob, app_netlist_t *list);
