<p align="right">
  <a href="README.md">English</a> · <strong>简体中文</strong>
</p>

# GLM 用量宝(AI Passport 固件)

面向 [FoloToy AI Passport](https://github.com/FoloToy/ai-passport) 掌机(ESP32-C3,2.4″ ST7789 240×320,三键)的固件:把你的 **GLM Coding Plan 套餐用量**显示在设备屏幕上,并自动定时刷新。

基于上游 `ai-passport` BSP 的二次开发应用。上游基线保留在 git 历史中,概览见 [docs/README.zh_CN.md](docs/README.zh_CN.md)。

## 功能

- **配网门户** — 纯手动:设置菜单 → 配网 → 开启热点。设备断开当前网络进入专用热点模式(`GLM-Meter-XXXX`,网关 192.168.4.1),手机连上后配网页自动弹出:扫描并保存多个热点(各自密码),点「保存并连接」后热点关闭、设备自动联网。联网后要再改配置,重新开启一次热点即可。
- **用量上屏** — 用量页显示本周额度、5 小时窗口、MCP 月度调用(个人套餐)或剩余点数(团队套餐),含进度条、重置时间与套餐等级。刷新周期 1~60 分钟可配。
- **团队套餐支持** — 团队额度走 `type=2` 接口 + 组织/项目头。门户粘贴一次性网页登录 Token 即可自动发现组织/项目(Token 用完即弃,不落盘)。
- **自动熄屏** — 静息超时可配(1~30 分钟或永不):关背光、停 LVGL、ST7789 进入睡眠(µA 级);任意按键点亮,查询照常在后台运行。
- **配置导入/导出** — 全部设置(含 WiFi 密码)导出为 JSON 文件,可在任意设备恢复。
- **AI 管理(MCP)** — 设备常驻 MCP 服务(`http://设备IP:8080/mcp`):配好 WiFi 后,API Key、团队套餐、用量查询、刷新周期、熄屏、亮度全部对 AI 说话即可完成,网页只是备用。
- **电量显示** — 右上角常显;熄屏是最有效的省电手段,熄屏期间 WiFi/HTTP 照常工作。

## 按键

| 按键 | 亮屏状态 | 熄屏状态 |
| --- | --- | --- |
| 上键 | 立即刷新用量 | 点亮 |
| 下键 | 打开设置菜单 | 点亮 |
| OK 单击 | 熄屏 | 点亮 |
| OK 长按 | 打开设置菜单 | 点亮 |

设置菜单含:刷新周期、熄屏时间、WiFi 管理、设备信息(含 API Key/套餐模式状态)、配网、AI 管理地址、亮度。

## 用 AI 配置(MCP)

在支持 MCP 的 AI 客户端(如 Claude)中添加 `http://设备IP:8080/mcp`(IP 见
屏幕 设置 → 设备信息),配好 WiFi 之后的一切都能对 AI 说话:

| 工具 | 作用 |
| --- | --- |
| `set_glm_key` | 保存智谱 API Key(保存即生效) |
| `glm_discover` | 用网页登录 Token 换取组织/项目清单(Token 不存储) |
| `set_glm_team` | 保存团队上下文(支持按 discover 清单编号选择) |
| `glm_usage` | 查询当前用量与 Key/套餐状态 |
| `clear_glm` | 清除 Key 与团队上下文 |
| `set_refresh_period` | 用量刷新周期(秒) |
| `set_screen_off` / `set_brightness` | 熄屏时间 / 屏幕亮度 |
| `wifi_status` / `wifi_connect_saved` | WiFi 状态与切换 |

典型流程:配网(上面的按键流程)→ 对 AI 说"配置 Key:xxx"→ AI 保存后设备
数秒内开始显示用量;团队套餐再把网页登录 Token 发给 AI,由它发现并保存
组织/项目。

## 使用的 BigModel 接口

| 套餐 | 请求 |
| --- | --- |
| 个人 | `GET https://open.bigmodel.cn/api/monitor/usage/quota/limit`,头 `Authorization: <APIKey>` |
| 团队 | 同 URL + `?type=2`,并带头 `bigmodel-organization: org-…`、`bigmodel-project: proj-…`(三者缺一不可) |
| 组织/项目发现 | `GET https://bigmodel.cn/api/biz/customer/getCustomerInfo`,鉴权用网页登录 Token(一次性,不存储) |

> 这些 monitor 接口不属于智谱公开文档化 API,当前可用但可能变动;响应形状变化时,屏幕会原样显示服务端消息。

## 烧录

整体镜像从 `0x0` 写入(会重置设置):

```bash
esptool --chip esp32c3 write_flash 0x0 FoloToy-AI-Passport-full.bin
```

保留配置(只刷 bootloader + 分区表 + 应用,NVS 原样):

```bash
esptool --chip esp32c3 write_flash \
  0x0 bootloader/bootloader.bin \
  0x8000 partition_table/partition-table.bin \
  0x10000 FoloToy-AI-Passport.bin
```

每次门禁通过后,`build/firmware/<sha256>/` 保留带校验的固件归档(含匹配 ELF/MAP)。

## 构建与测试

需要 ESP-IDF 5.5.x。统一门禁在干净的临时目录编译固件、合并 0x0 镜像、校验布局并归档调试产物;静态检查包含主机测试(两种套餐形状的用量解析、热点列表逻辑、全部 UI 文案的中文字形覆盖):

```bash
./tools/validate.sh            # 完整门禁
./tools/validate.sh --static   # 仓库检查 + 主机测试
./tools/validate.sh --firmware # 固件构建 + 合并镜像
```

中文使用 Noto Sans SC 子集(字频前 3500 字 + 标点,`lv_font_conv` 生成);任何 UI 文案超出字符集都会让 `tests/test_ui_charset.py` 门禁失败。见 [assets/README.zh_CN.md](assets/README.zh_CN.md)。

## 模拟器

[VOID001/FoloToy-Passport-Simulator](https://github.com/VOID001/FoloToy-Passport-Simulator) 可在浏览器(WASM/QEMU)运行同一 0x0 镜像,内置虚拟 WiFi 桥可测联网。

## 安全说明

API Key 与 WiFi 密码只存于设备本地 NVS 分区,除 BigModel 外不发送给任何第三方。配网门户是设备自身网络上的纯 HTTP 服务,暴露面限于局域网。配置导出文件包含敏感信息,请妥善保管。

## 许可

应用代码遵循上游 [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport) 的许可证;内置 [Noto Sans SC](assets/fonts/OFL.txt) 为 SIL OFL 1.1;vendored [cJSON](tests/thirdparty/cJSON/README.md) 为 MIT。
