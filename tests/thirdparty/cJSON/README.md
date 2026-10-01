# tests/thirdparty/cJSON

<p align="right"><strong>English</strong> · <a href="README.zh_CN.md">简体中文</a></p>

Host-test-only vendored copy of [cJSON](https://github.com/DaveGamble/cJSON)
(MIT License, Copyright (c) 2009-2017 Dave Gamble and cJSON contributors;
full license text is embedded at the top of `cJSON.h`).

- Source: ESP-IDF v5.5 `components/json/cJSON` (identical upstream cJSON),
  copied so that `./tools/validate.sh --static` can compile
  `tests/test_glm_usage_parse.c` on the host without ESP-IDF installed.
- Firmware builds link the managed `json` component instead; the application
  source `main/app_glm_usage.c` is compiled against the same cJSON API on
  both sides, so host tests exercise the exact code that runs on device.
- Do not modify these files; bump by re-copying from upstream when needed.
