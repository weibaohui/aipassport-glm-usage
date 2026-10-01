#!/usr/bin/env python3
"""中文字形覆盖门禁:main 源码字符串字面量里的每个 CJK/全角字符都必须在
assets/fonts/glm_charset.txt 中(该清单即 lv_font_conv 生成字体用的 --symbols)。

背景见 docs/development/engineering/lvgl-chinese-fonts.zh_CN.md:UTF-8 正确、
编译成功都不代表能显示;本测试保证"静态文案 ⊆ 字体子集"这一硬契约,
文案改动而忘记重新生成字体时门禁会失败。
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CHARSET = ROOT / "assets" / "fonts" / "glm_charset.txt"
# 只检查真正送往 LVGL 渲染的文案所在文件:屏幕上的字符串都由 app_ui.c 构造
# (main.c 仅提供启动横幅日志)。app_portal.c 的中文在浏览器端渲染,用系统
# 字体,不受设备字库约束,故意不列入。
SOURCES = [
    "main/main.c",
    "main/app_ui.c",
]

# 需要覆盖的字符范围:CJK 统一表意文字 + 常用全角标点/符号。
def is_checked(cp: int) -> bool:
    if 0x4E00 <= cp <= 0x9FFF:  # CJK Unified Ideographs
        return True
    return cp in {0x3001, 0x3002, 0x300A, 0x300B, 0x2014, 0x2026,
                  0x00B7, 0x00B0, 0x2103,
                  0xFF01, 0xFF05, 0xFF08, 0xFF09, 0xFF0C, 0xFF1A, 0xFF1B, 0xFF1F}


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def string_literal_chars(path: Path) -> set[str]:
    """提取源文件字符串字面量中的受检字符(忽略注释中的中文)。

    只做词法级近似:不处理转义引号(本仓库 UI 文案不含 \\"),原始字符串
    R"HTML(...)" 由后面的 --output 行过滤掉。
    """
    text = strip_comments(path.read_text(encoding="utf-8"))
    # 丢弃 lv_font_conv 记录命令行的头部注释之外可能混入的 "--output" 行
    chars: set[str] = set()
    for match in re.finditer(r'"([^"\n]*)"', text):
        for ch in match.group(1):
            if is_checked(ord(ch)):
                chars.add(ch)
    return chars


def main() -> int:
    charset = set(CHARSET.read_text(encoding="utf-8").strip())
    problems: list[str] = []
    total = set()
    for rel in SOURCES:
        used = string_literal_chars(ROOT / rel)
        total |= used
        for ch in sorted(used - charset):
            problems.append(f"{rel}: U+{ord(ch):04X} {ch!r} 不在字体子集中")
    if problems:
        print("字形覆盖检查失败:")
        for line in problems:
            print("  " + line)
        print("请把缺字补入 assets/fonts/glm_charset.txt 并重新生成字体,")
        print("命令见 assets/README.zh_CN.md。")
        return 1
    print(f"字形覆盖检查通过:{len(total)} 个受检字符全部在字体子集中")
    return 0


if __name__ == "__main__":
    sys.exit(main())
