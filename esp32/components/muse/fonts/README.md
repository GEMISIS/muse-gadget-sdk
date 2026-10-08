# Fonts

`muse_font_cjk_16.c` is the CJK fallback for the caption font, built in only
with `CONFIG_MUSE_CJK_FONT`. It holds GNU Unifont 16.0.04's 16x16 bitmaps,
the same cell as unscii-16, for CJK punctuation, kana, every CJK Unified
Ideograph (U+4E00 to U+9FFF) and the fullwidth forms: about 850 KB of flash.
`tools/muse/gen_cjk_font.sh` regenerates it.

GNU Unifont is by Roman Czyborra, Paul Hardy and contributors
(https://unifoundry.com/unifont/). Its compiled fonts are licensed under the
SIL Open Font License, version 1.1 (https://openfontlicense.org), and under
the GNU GPL version 2 or later with the GNU font embedding exception. This
file is a conversion of an unaltered subset of the font.

`muse_font_clock_72.c` is the face's clock (muse_home_extras.c): Montserrat
Medium at 72 px, 4 bpp, only the digits, ':', '-', ' ', 'A', 'M' and 'P'.
Made with lv_font_conv from LVGL's own copy of the font
(`managed_components/lvgl__lvgl/scripts/built_in_font/Montserrat-Medium.ttf`):

    npx lv_font_conv --font Montserrat-Medium.ttf --size 72 --bpp 4 --format lvgl \
        --lv-font-name muse_font_clock_72 --no-compress \
        -r 0x20,0x2D,0x30-0x3A,0x41,0x4D,0x50 -o muse_font_clock_72.c

Montserrat is by Julieta Ulanovsky and is licensed under the SIL Open Font
License, version 1.1, as LVGL's built-in Montserrat fonts are.
