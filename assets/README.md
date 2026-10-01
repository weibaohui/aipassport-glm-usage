<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Assets

This directory stores reusable fonts, images, music, and sound effects, organized by asset type.

Keep each asset in the matching subdirectory and document its destination, naming, integration method, and source/license. Do not mix binary assets with Markdown documentation.

## Fonts

Store reusable font files and generated font sources in `fonts/`.

| File | Specs | Use and source |
| --- | --- | --- |
| [`fonts/glm_charset.txt`](fonts/glm_charset.txt) | Plain text, 3610 codepoints | Character set of the GLM usage-meter app: printable ASCII + common fullwidth punctuation + top 3500 hanzi by frequency (derived from [FrequencyWords zh_cn 50k](https://github.com/hermitdave/FrequencyWords)) + extra glyphs found in UI strings. Dynamic content (Wi-Fi SSIDs) beyond this subset falls back to placeholders. |
| [`fonts/app_font_16.c`](fonts/app_font_16.c) | 16 px / 4 bpp / LVGL C source | App body font (Chinese + ASCII subset), compiled into the `main` component. Generated from `NotoSansSC-Regular.otf` with `lv_font_conv@1.5.3 --no-compress` over `glm_charset.txt`. |
| [`fonts/app_font_24.c`](fonts/app_font_24.c) | 24 px / 4 bpp / LVGL C source | App title/percentage font, generated the same way. |
| [`fonts/NotoSansSC-Regular.otf`](fonts/NotoSansSC-Regular.otf) | OpenType, SubsetOTF SC Regular | Source font for generation. [Noto Sans CJK SC](https://github.com/notofonts/noto-cjk) (SIL OFL 1.1, see [`fonts/OFL.txt`](fonts/OFL.txt)), sha256 `faa6c9df652116dde789d351359f3d7e5d2285a2b2a1f04a2d7244df706d5ea9`. |

- Use descriptive names that include the family, weight, size, and format when relevant.
- Document the source, license, character range, conversion command, and expected destination.
- Check Flash and internal-RAM impact before adding a font; the ESP32-C3 has no PSRAM.
  The two generated fonts add ~1.6MB of Flash (bitmap data) and no internal-RAM heap.
- Regeneration command (run at the repository root, tool pinned to 1.5.3):

  ```bash
  npx lv_font_conv@1.5.3 \
    --font assets/fonts/NotoSansSC-Regular.otf \
    --size 16 --bpp 4 --format lvgl --no-compress \
    --lv-font-name app_font_16 --lv-include lvgl.h \
    --symbols "$(cat assets/fonts/glm_charset.txt)" \
    --output assets/fonts/app_font_16.c
  # use --size 24 to regenerate app_font_24.c
  ```

- Glyph coverage acceptance: static UI strings must be a subset of
  `glm_charset.txt`; `tests/test_ui_charset.py` enforces this in the host gate.
  Regenerate affected sizes whenever strings change.
- Do not commit fonts whose license does not permit redistribution.

## Images

Store reusable source images and generated display assets in `images/`.

| File | Dimensions and format | Use and source |
| --- | --- | --- |
| [`images/home.jpg`](images/home.jpg) | 3840 × 2160, JPEG | Product hero image embedded in both project README files to foreground AI Passport and its open, maker-oriented identity. |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724, PNG RGBA | Optional technical infographic retained as a reference asset; it is no longer used as the homepage hero. Generated for this repository with the built-in image generation tool on 2026-09-17; the six labels and values were checked against the documented hardware contract. |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336, PNG RGBA | Transparent black wordmark extracted from the repository's original `images/logo.png`; embedded in both project README files for light backgrounds. |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336, PNG RGBA | White version of the extracted wordmark, used by the README `<picture>` element when GitHub is in dark mode. |

- Use descriptive names and document dimensions, pixel format, conversion steps, and destination.
- Prefer formats suitable for the 240 × 320 RGB565 display and account for Flash and internal RAM.
- Preserve editable sources where licensing permits, and record the source and license.
- Never commit device QR secrets, credentials, or personal data in images.

## Music and sound effects

Store reusable music and sound-effect sources in `music/`.

- Document the source, license, sample rate, bit depth, channels, conversion command, and destination.
- Prefer 16 kHz, 16-bit mono PCM when it matches the current BSP audio path.
- Check Flash and internal-RAM cost before embedding audio; stream or chunk long recordings.
- Do not commit media without redistribution permission.
