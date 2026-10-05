# 迁移到统一底层框架(aipassport-fw)方案

状态:**方案已定,未开始执行**(2026-10-05)。收音机(aipassport-radio)已跑通
"应用仓 + framework 子模块"模式,用量宝按同一模式迁移,消灭 main/ 里那批
框架前身模块的私有副本。

## 为什么迁

- main/ 里的 app_net / app_netlist / app_portal / app_storage / app_ui
  (合计 ~2800 行)是框架(appfw,~5500 行)的**祖先副本**,各自演化已经分叉:
  框架修的 bug(门户整页截断、扫描离信道黑洞、断线探活、按键规整)这里
  都不存在,框架的新能力(MCP 常驻服务、网络日志、亮度、菜单选项体系)
  这里也用不上。
- 迁移后用量宝获得:AI 管理(MCP 8080)、诊断日志、亮度/动效设置、
  与收音机同一套已验证的配网/重连/门户,且框架升级一次两个应用受益。

## 模块映射

| 用量宝(main/,components/) | 框架(components/framework) | 动作 |
| --- | --- | --- |
| components/bsp | framework/bsp | 删除本地,同一硬件同一 bsp_pins |
| app_storage.c/h | appfw_storage | 调用点改名(appfw_store_*) |
| app_netlist.c/h | appfw_netlist | 基本同构,改名 |
| app_net.c/h | appfw_net | 改名;重连/探活/扫描轮询由框架接管,删除本地的 |
| app_portal.c/h | appfw_portal | 阶段一(WiFi)直接用框架模板;阶段二(Key/团队/周期/熄屏)改写为 `app_config_html` 注入片段;免扫描加台用框架 `/api/networks/add` |
| app_ui.c/h | appfw_ui | 用量页改 `home_build` 钩子;设置/配网/熄屏菜单全删,用框架内置;信息行接 `info_rows`/`config_rows` 钩子 |
| app_glm_client / app_glm_usage / ui_pixel* | 保留(应用层) | 不动;用量页渲染改持 `bsp_lvgl_lock` 与框架字体 |
| main.c | — | 引导序列改 `appfw_*`:appfw_store → appfw_net → appfw_portal → appfw_ui_init(cfg);按键经 `appfw_ui_on_key` 规整 |

## 迁移带来的对齐项(顺带核对)

- 配网门户行为对齐收音机定稿:纯手动、专用热点模式、点连接即联网
- 熄屏改框架三层实现(背光→lvgl 停→面板睡眠),删除 app 低功耗副本
- 新增 sdkconfig:CONFIG_LV_USE_QRCODE=y 等(对齐收音机 sdkconfig.defaults)

## 步骤

1. `git submodule add git@github.com:weibaohui/aipassport-fw.git components/framework`
   (固定到收音机 dep.md 记录的已知正常提交,见 aipassport-radio/dep.md)
2. CMakeLists:main 的 SRCS 删去被替换模块;增挂 framework 组件
3. 按上表改名移植,先编过(`idf.py build`),再逐功能真机验证:
   配网两阶段 → Key/团队配置 → 用量查询 → 熄屏/唤醒 → 断线重连
4. 模拟器冒烟(注意:仿真器 NVS 写入会静默重启,配置类流程只上真机)
5. 应用仓建 dep.md(同收音机格式),记录首次配对的框架提交
6. 两仓提交推送;社区 859 的更新版本等下次授权码

## 风险

- app_ui 的用量页是自绘像素 UI,接框架 home_build 后布局要重排(工作量主要在这)
- 门户新模板的注入点契约(禁止出现应用词汇、标记恰一次性)有门禁测试,
  阶段二片段要按测试约束写
