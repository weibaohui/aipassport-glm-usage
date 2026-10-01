<p align="right">
  <strong>English</strong> · <a href="README.zh_CN.md">简体中文</a>
</p>

# GLM Usage Meter (AI Passport firmware)

A firmware for the [FoloToy AI Passport](https://github.com/FoloToy/ai-passport) handheld (ESP32-C3, 2.4" ST7789 240×320, 3 keys) that shows your **GLM Coding Plan quota** on the device screen and refreshes it automatically.

Built as a second-development application on the upstream `ai-passport` BSP. The upstream baseline is preserved in the git history and documented in [docs/README.zh_CN.md](docs/README.zh_CN.md).

## Features

- **Provisioning portal** — with no saved config the device opens a hotspot (`GLM-Meter-XXXX`, gateway `192.168.4.1`) with a captive portal. Phase 1 (offline): scan and save multiple Wi-Fi networks with per-network passwords; the device auto-connects and falls back through the list. Phase 2 (online, via the LAN address shown on screen): API key, team context, refresh period, screen-off timeout, network management.
- **GLM quota on screen** — usage page shows weekly quota, 5-hour window and MCP monthly calls (personal plans) or remaining credits (team plans), with progress bars, reset times and plan level. Refreshes every 1–60 minutes (configurable).
- **Team plan support** — team quotas use the `type=2` endpoint with organization/project headers. Paste a one-shot web login token in the portal to auto-discover your organizations/projects (the token is used once and never stored).
- **Auto screen-off** — configurable idle timeout (1–30 min or never): backlight off, LVGL stopped, ST7789 put to sleep (µA-level). Any key wakes the screen; usage polling keeps running in the background.
- **Config import/export** — download all settings (including Wi-Fi passwords) as a JSON file and restore them on any device.
- **Battery & power aware** — battery percentage on screen, screen-off is the biggest power lever; Wi-Fi/HTTP keep working with the screen off.

## Keys

| Key | Action (screen on) | Action (screen off) |
| --- | --- | --- |
| UP | refresh now (usage page) / back to usage page | wake |
| DOWN | open network page | wake |
| OK click | screen off | wake |
| OK long-press | open provisioning portal | wake |

The device is never unreachable: after the provisioning hotspot closes, the network page shows the current management URL (`http://<LAN-IP>`).

## BigModel endpoints used

| Plan | Request |
| --- | --- |
| Personal | `GET https://open.bigmodel.cn/api/monitor/usage/quota/limit` with `Authorization: <API key>` |
| Team | Same URL + `?type=2` and headers `bigmodel-organization: org-…`, `bigmodel-project: proj-…` (all three required) |
| Org/project discovery | `GET https://bigmodel.cn/api/biz/customer/getCustomerInfo` with a web login token (one-shot, not stored) |

> These monitor endpoints are not part of BigModel's public, documented API. They work today but may change without notice — if the response shape changes, the screen shows the server message.

## Flashing

Full image from `0x0` (resets settings):

```bash
esptool --chip esp32c3 write_flash 0x0 FoloToy-AI-Passport-full.bin
```

Config-preserving (bootloader + partition table + app only, NVS untouched):

```bash
esptool --chip esp32c3 write_flash \
  0x0 bootloader/bootloader.bin \
  0x8000 partition_table/partition-table.bin \
  0x10000 FoloToy-AI-Passport.bin
```

Verified firmware archives (with matching ELF/MAP) are kept in `build/firmware/<sha256>/` after every gate run.

## Build & test

Requires ESP-IDF 5.5.x. The unified gate compiles the firmware in a clean temp dir, merges the 0x0 image, verifies layout and archives debug artifacts; static checks include host tests (quota parsing for both plan shapes, network-list logic, CJK glyph coverage of every UI string):

```bash
./tools/validate.sh            # full gate
./tools/validate.sh --static   # repo checks + host tests
./tools/validate.sh --firmware # firmware build + merged image
```

Chinese text uses Noto Sans SC subsets (top-3500 frequency chars + punctuation) generated with `lv_font_conv`; `tests/test_ui_charset.py` fails the gate if any UI string leaves the charset. See [assets/README.zh_CN.md](assets/README.zh_CN.md).

## Simulator

[VOID001/FoloToy-Passport-Simulator](https://github.com/VOID001/FoloToy-Passport-Simulator) runs the same 0x0 image in a browser (WASM/QEMU), with a virtual Wi-Fi bridge for network testing.

## Security notes

Your API key and Wi-Fi passwords are stored only in the device's NVS partition and are never sent anywhere except BigModel. The provisioning portal is plain HTTP on the device's own network — flash/portal exposure is limited to your LAN. The config export file contains secrets; keep it safe.

## License

Application code: same license as the upstream [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport). Bundled [Noto Sans SC](assets/fonts/OFL.txt) is under SIL OFL 1.1; vendored [cJSON](tests/thirdparty/cJSON/README.md) is MIT.
