# tests/thirdparty/cJSON

<p align="right"><a href="README.md">English</a> · <strong>简体中文</strong></p>

仅供主机测试使用的 vendored 副本 of [cJSON](https://github.com/DaveGamble/cJSON)
(MIT 许可,Copyright (c) 2009-2017 Dave Gamble and cJSON contributors;
完整许可文本内嵌于 `cJSON.h` 文件头)。

- 来源:ESP-IDF v5.5 `components/json/cJSON`(与上游 cJSON 相同),复制至此
  使 `./tools/validate.sh --static` 在未安装 ESP-IDF 的主机上也能编译
  `tests/test_glm_usage_parse.c`。
- 固件构建改用托管的 `json` 组件;应用源码 `main/app_glm_usage.c` 在两端
  针对同一 cJSON API 编译,主机测试覆盖的正是设备上运行的代码。
- 不要修改这些文件;需要升级时从上游重新复制。
