// tests/test_app_netlist.c —— 已保存热点列表纯逻辑的主机测试。
// 编译(见 tools/validate.sh):
//   cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_app_netlist.c main/app_netlist.c
#include "app_netlist.h"

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

static void test_add_and_limit(void)
{
    app_netlist_t l;
    app_netlist_reset(&l);
    CHECK(l.count == 0 && l.selected == -1);

    CHECK(app_netlist_add(&l, "home", "pw1"));
    CHECK(app_netlist_add(&l, "office", "pw2"));
    CHECK(l.count == 2);

    // 同名 = 覆盖密码,不新增。
    CHECK(app_netlist_add(&l, "home", "pw1b"));
    CHECK(l.count == 2);
    CHECK(strcmp(l.items[0].pwd, "pw1b") == 0);

    // 塞满后再加应失败(循环用不同 SSID,同名会走覆盖路径)。
    char name[16];
    for (int i = 0; l.count < APP_NETLIST_MAX; i++) {
        snprintf(name, sizeof(name), "net-%d", i);
        CHECK(app_netlist_add(&l, name, ""));
    }
    CHECK(l.count == APP_NETLIST_MAX);
    CHECK(!app_netlist_add(&l, "overflow", ""));

    // 空SSID / 超长 SSID 拒绝。
    CHECK(!app_netlist_add(&l, "", ""));
    char long_ssid[64];
    memset(long_ssid, 'a', sizeof(long_ssid) - 1);
    long_ssid[sizeof(long_ssid) - 1] = '\0';
    CHECK(!app_netlist_add(&l, long_ssid, ""));
}

static void test_remove_and_select(void)
{
    app_netlist_t l;
    app_netlist_reset(&l);
    app_netlist_add(&l, "a", "1");
    app_netlist_add(&l, "b", "2");
    app_netlist_add(&l, "c", "3");
    CHECK(app_netlist_select(&l, "b"));
    CHECK(l.selected == 1);

    // 删除被点选项 → selected 复位 -1;顺序保持。
    CHECK(app_netlist_remove(&l, 1));
    CHECK(l.count == 2);
    CHECK(strcmp(l.items[0].ssid, "a") == 0);
    CHECK(strcmp(l.items[1].ssid, "c") == 0);
    CHECK(l.selected == -1);

    // 删除未点选项 → selected 追随 SSID 前移。
    CHECK(app_netlist_select(&l, "c"));
    CHECK(app_netlist_remove(&l, 0)); // 删 a
    CHECK(l.count == 1);
    CHECK(l.selected == 0);
    CHECK(!app_netlist_remove(&l, 5)); // 越界
}

static void test_next_target_order(void)
{
    app_netlist_t l;
    app_netlist_reset(&l);
    app_netlist_entry_t t;
    CHECK(!app_netlist_next_target(&l, 0, &t)); // 空表

    app_netlist_add(&l, "a", "1");
    app_netlist_add(&l, "b", "2");
    app_netlist_add(&l, "c", "3");

    // 未点选:从保存顺序开始。
    CHECK(app_netlist_next_target(&l, 0, &t) && strcmp(t.ssid, "a") == 0);
    CHECK(app_netlist_next_target(&l, 1, &t) && strcmp(t.ssid, "b") == 0);
    CHECK(app_netlist_next_target(&l, 2, &t) && strcmp(t.ssid, "c") == 0);
    CHECK(app_netlist_next_target(&l, 3, &t) && strcmp(t.ssid, "a") == 0); // 轮转

    // 点选 c:第 0 次是 c,其后按保存顺序 a、b。
    app_netlist_select(&l, "c");
    CHECK(app_netlist_next_target(&l, 0, &t) && strcmp(t.ssid, "c") == 0);
    CHECK(app_netlist_next_target(&l, 1, &t) && strcmp(t.ssid, "a") == 0);
    CHECK(app_netlist_next_target(&l, 2, &t) && strcmp(t.ssid, "b") == 0);
}

static void test_serialize_roundtrip(void)
{
    app_netlist_t l, back;
    app_netlist_reset(&l);
    app_netlist_add(&l, "home-WiFi", "p@ss:word|123"); // 密码含分隔符也要无损
    app_netlist_add(&l, "中文热点", "");
    app_netlist_add(&l, "office", "3");

    char blob[APP_NETLIST_BLOB_MAX];
    CHECK(app_netlist_serialize(&l, blob, sizeof(blob)));
    CHECK(app_netlist_deserialize(blob, &back));
    CHECK(back.count == 3);
    CHECK(strcmp(back.items[0].ssid, "home-WiFi") == 0);
    CHECK(strcmp(back.items[0].pwd, "p@ss:word|123") == 0);
    CHECK(strcmp(back.items[1].ssid, "中文热点") == 0);
    CHECK(back.items[1].pwd[0] == '\0');

    // 空表序列化为空串,反序列化回空表。
    app_netlist_reset(&l);
    CHECK(app_netlist_serialize(&l, blob, sizeof(blob)));
    CHECK(blob[0] == '\0');
    CHECK(app_netlist_deserialize(blob, &back));
    CHECK(back.count == 0);

    // 缓冲不足必须失败,不得越界。
    char tiny[8];
    app_netlist_add(&l, "this-is-a-long-ssid-name", "password");
    CHECK(!app_netlist_serialize(&l, tiny, sizeof(tiny)));

    // 畸形 blob 拒绝。
    CHECK(!app_netlist_deserialize("garbage", &back));
    CHECK(!app_netlist_deserialize("3:abc:1:", &back));  // 长度声明与实际不符
}

int main(void)
{
    test_add_and_limit();
    test_remove_and_select();
    test_next_target_order();
    test_serialize_roundtrip();
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("test_app_netlist: all checks passed\n");
    return 0;
}
