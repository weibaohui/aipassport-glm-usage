<p align="right">
  <a href="README.md">English</a> · <strong>简体中文</strong>
</p>

# GLM 用量宝(AI Passport 固件)

面向 [FoloToy AI Passport](https://github.com/FoloToy/ai-passport) 掌机(ESP32-C3,2.4″ ST7789 240×320,三键)的固件:把你的 **GLM Coding Plan 套餐用量**显示在设备屏幕上,并自动定时刷新。

基于上游 `ai-passport` BSP 的二次开发应用。上游基线保留在 git 历史中,概览见 [docs/README.zh_CN.md](docs/README.zh_CN.md)。

## 功能

- **配网门户** — 无配置开机自动开热点(`GLM-Meter-XXXX`,网关 192.168.4.1)并带 captive portal。阶段一(未联网):扫描并保存多个热点(各自密码),设备自动连接、失败按列表回退;阶段二(联网后,经屏幕显示的局域网地址):API Key、团队上下文、刷新周期、熄屏档位、热点管理。
- **用量上屏** — 用量页显示本周额度、5 小时窗口、MCP 月度调用(个人套餐)或剩余点数(团队套餐),含进度条、重置时间与套餐等级。刷新周期 1~60 分钟可配。
- **团队套餐支持** — 团队额度走 `type=2` 接口 + 组织/项目头。门户粘贴一次性网页登录 Token 即可自动发现组织/项目(Token 用完即弃,不落盘)。
- **自动熄屏** — 静息超时可配(1~30 分钟或永不):关背光、停 LVGL、ST7789 进入睡眠(µA 级);任意按键点亮,查询照常在后台运行。
- **配置导入/导出** — 全部设置(含 WiFi 密码)导出为 JSON 文件,可在任意设备恢复。
- **电量显示** — 右上角常显;熄屏是最有效的省电手段,熄屏期间 WiFi/HTTP 照常工作。

## 按键

| 按键 | 亮屏状态 | 熄屏状态 |
| --- | --- | --- |
| 上键 | 用量页=立即刷新;网络页=切回用量页 | 点亮 |
| 下键 | 打开网络页 | 点亮 |
| OK 单击 | 熄屏 | 点亮 |
| OK 长按 | 进入配网门户 | 点亮 |

设备不会"失联":配网热点关闭后,网络页常显当前管理地址(`http://<局域网IP>`)。

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
