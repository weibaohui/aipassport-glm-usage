# GLM 用量宝 应用组件(挂在 aipassport-fw 框架之上)。
#
# 通用能力(WiFi/门户/存储/UI 骨架/熄屏/按键/BSP/MCP)全部来自框架 appfw;
# 本组件只保留业务:GLM 套餐用量查询与展示、Key/团队上下文存储与门户注入。
# 旧的自研 app_net/app_netlist/app_portal/app_ui 已由框架取代(见
# docs/migration-to-framework.zh_CN.md),demo_*.c/ui_pixel*.c 仅为基线参考,
# 不编入固件。
idf_component_register(
    SRCS "main.c"
         "app_glm_usage.c"
         "app_glm_client.c"
         "app_storage.c"
         "app_home.c"
         "app_portal_glm.c"
    INCLUDE_DIRS "."
    REQUIRES appfw bsp esp_event esp_hw_support esp_netif esp_timer esp_wifi nvs_flash
             json esp_http_client esp-tls esp_http_server lwip app_update
)

# 应用中文字体子集(生成物,来源/命令/字符清单见 assets/fonts/README.zh_CN.md)。
# 字体位图常驻 Flash,不占内部 RAM 堆;切勿在多个组件重复编译同一字体。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/app_font_16.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/app_font_24.c"
)
