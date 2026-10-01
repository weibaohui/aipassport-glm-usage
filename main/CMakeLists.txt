# GLM 用量宝 应用组件。
#
# 基线 demo 源文件(demo_*.c / ui_pixel*.c)保留在仓库中作为参考实现,但不再
# 编入本应用固件:二次开发应用必须自带重新设计的 UI(app_ui.c),且本应用不用
# BLE —— 不编 demo_ble.c 才能从 REQUIRES 中去掉 bt,为 TLS+WiFi 省出内部 RAM。
# host 测试直接编译这些文件,不经过本 CMakeLists,不受影响。
idf_component_register(
    SRCS "main.c"
         "app_glm_usage.c"
         "app_netlist.c"
         "app_storage.c"
         "app_net.c"
         "app_portal.c"
         "app_glm_client.c"
         "app_ui.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_event esp_hw_support esp_netif esp_timer esp_wifi nvs_flash
             json esp_http_client esp-tls esp_http_server lwip app_update
)

# 应用中文字体子集(生成物,来源/命令/字符清单见 assets/fonts/README.zh_CN.md)。
# 字体位图常驻 Flash,不占内部 RAM 堆;切勿在多个组件重复编译同一字体。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/app_font_16.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/app_font_24.c"
)
