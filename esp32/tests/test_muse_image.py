# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""muse_image.c: a reply's PNG and WebP decoded to RGB565, fitted to the
screen as they decode (libwebp's own scaling; a PNG averaged down a row at a
time), transparency over white, and what it refuses. Built against the
vendored libwebp (components/libwebp) and the host's libpng, if there is one
(pkg-config libpng; the PNG cases skip without it). The images were made with
Pillow, once, and are kept here as base64."""
import base64
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
WEBP = ROOT / 'components/libwebp'

WEBP_LOSSY_BIG = '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'  # 1868 bytes
WEBP_ALPHA = 'UklGRiYAAABXRUJQVlA4TBkAAAAvJ8AEEA8wyBHzHwyBAOHAf60BARH9jxL2AA=='  # 46 bytes
WEBP_ANIMATED = 'UklGRsoAAABXRUJQVlA4WAoAAAACAAAADwAADwAAQU5JTQYAAAAAAAAAAABBTk1GSgAAAAAAAAAAAA8AAA8AAGQAAAJWUDggMgAAADABAJ0BKhAAEAABQCYloAADcAD+8ut///mwP/bz/wR6Af//0uD//pcH//S4P/SkAAAAQU5NRkwAAAAAAAAAAAAPAAAPAABkAAAAVlA4IDQAAAA0AQCdASoQABAAAAAmJaAAA3AA/ukiH//3nz//ufP/+58/6M///yn7//I4//8jj/5QIAAA'  # 210 bytes
PNG_BIG = 'iVBORw0KGgoAAAANSUhEUgAAA+gAAAJYCAIAAAB+fFtyAAAKLUlEQVR42u3WIREAAAgAMUDQPy+KFghui/DqcyYAeKZbA4BvSgIAADDuAACAcQcAAOMOAAAYdwAAwLgDAIBxBwAAjDsAAGDcAQDAuAMAAMYdAACMOwAAYNwBAADjDgAAxh0AADDuAACAcQcAAOMOAAAYdwAAwLgDAIBxBwAAjDsAABh3AADAuAMAAMYdAACMOwAAYNwBAADjDgAAxh0AADDuAABg3AEAAOMOAAAYdwAAMO4AAIBxBwAAjDsAABh3AADAuAMAAMYdAACMOwAAYNwBAMC4AwAAxh0AADDuAABg3AEAAOMOAAAYdwAAMO4AAIBxBwAA4w4AABh3AADAuAMAgHEHAACMOwAAYNwBAMC4AwAAxh0AADDuAABg3AEAAOMOAADGHQAAMO4AAIBxBwAA4w4AABh3AADAuAMAgHEHAACMOwAAGHcAAMC4AwAAxh0AAIw7AABg3AEAAOMOAADGHQAAMO4AAIBxBwAA4w4AABh3AAAw7gAAgHEHAACMOwAAGHcAAMC4AwAAxh0AAIw7AABg3AEAwLgDAADGHQAAMO4AAGDcAQAA4w4AABh3AAAw7gAAgHEHAACMOwAAGHcAAMC4AwCAcQcAAIw7AABg3AEAwLgDAADGHQAAMO4AAGDcAQAA4w4AAMYdAAAw7gAAgHEHAADjDgAAGHcAAMC4AwCAcQcAAIw7AABg3AEAwLgDAADGHQAAjDsAAGDcAQAA4w4AAMYdAAAw7gAAgHEHAADjDgAAGHcAADDuAACAcQcAAIw7AAAYdwAAwLgDAADGHQAAjDsAAGDcAQAA4w4AAMYdAAAw7gAAYNwBAADjDgAAGHcAADDuAACAcQcAAIw7AAAYdwAAwLgDAIBxBwAAjDsAAGDcAQDAuAMAAMYdAAAw7gAAYNwBAADjDgAAGHcAADDuAACAcQcAAOMOAAAYdwAAwLgDAIBxBwAAjDsAAGDcAQDAuAMAAMYdAACMOwAAYNwBAADjDgAAxh0AADDuAACAcQcAAOMOAAAYdwAAwLgDAIBxBwAAjDsAABh3AADAuAMAAMYdAACMOwAAYNwBAADjDgAAxh0AADDuAABg3AEAAOMOAAAYdwAAMO4AAIBxBwAAjDsAABh3AADAuAMAAMYdAACMOwAAYNwBAMC4AwAAxh0AADDuAABg3AEAAOMOAAAYdwAAMO4AAIBxBwAA4w4AABh3AADAuAMAgHEHAACMOwAAYNwBAMC4AwAAxh0AADDuAABg3AEAAOMOAADGHQAAMO4AAIBxBwAA4w4AABh3AADAuAMAgHEHAACMOwAAGHcAAMC4AwAAxh0AAIw7AABg3AEAAOMOAADGHQAAMO4AAIBxBwAA4w4AABh3AAAw7gAAgHEHAACMOwAAGHcAAMC4AwAAxh0AAIw7AABg3AEAwLgDAADGHQAAMO4AAGDcAQAA4w4AABh3AAAw7gAAgHEHAACMOwAAGHcAAMC4AwCAcQcAAIw7AABg3AEAwLgDAADGHQAAMO4AAGDcAQAA4w4AAMZdAgAAMO4AAIBxBwAA4w4AABh3AADAuAMAgHEHAACMOwAAYNwBAMC4AwAAxh0AAIw7AABg3AEAAOMOAADGHQAAMO4AAIBxBwAA4w4AABh3AADAuAMAgHEHAACMOwAAGHcAAMC4AwAAxh0AAIw7AABg3AEAAOMOAADGHQAAMO4AAGDcAQAA4w4AABh3AAAw7gAAgHEHAACMOwAAGHcAAMC4AwAAxh0AAIw7AABg3AEAwLgDAADGHQAAMO4AAGDcAQAA4w4AABh3AAAw7gAAgHEHAADjDgAAGHcAAMC4AwCAcQcAAIw7AABg3AEAwLgDAADGHQAAMO4AAGDcAQAA4w4AAMYdAAAw7gAAgHEHAADjDgAAGHcAAMC4AwCAcQcAAIw7AAAYdwAAwLgDAADGHQAAjDsAAGDcAQAA4w4AAMYdAAAw7gAAgHEHAADjDgAAGHcAADDuAACAcQcAAIw7AAAYdwAAwLgDAADGHQAAjDsAAGDcAQDAuAMAAMYdAAAw7gAAYNwBAADjDgAAGHcAADDuAACAcQcAAIw7AAAYdwAAwLgDAIBxBwAAjDsAAGDcAQDAuAMAAMYdAAAw7gAAYNwBAADjDgAAxh0AADDuAACAcQcAAOMOAAAYdwAAwLgDAIBxBwAAjDsAAGDcAQDAuAMAAMYdAACMOwAAYNwBAADjDgAAxh0AADDuAACAcQcAAOMOAAAYdwAAMO4AAIBxBwAAjDsAABh3AADAuAMAAMYdAACMOwAAYNwBAADjDgAAxh0AADDuAABg3AEAAOMOAAAYdwAAMO4AAIBxBwAAjDsAABh3AADAuAMAgHEHAACMOwAAYNwBAMC4AwAAxh0AADDuAABg3AEAAOMOAAAYdwAAMO4AAIBxBwAA4w4AABh3AADAuAMAgHEHAACMOwAAYNwBAMC4AwAAxh0AAIw7AABg3AEAAOMOAADGHQAAMO4AAIBxBwAA4w4AABh3AADAuAMAgHEHAACMOwAAGHcAAMC4AwAAxh0AAIw7AABg3AEAAOMOAADGHQAAMO4AAGDcAQAA4w4AABh3AAAw7gAAgHEHAACMOwAAGHcAAMC4AwAAxh0AAIw7AABg3AEAwLgDAADGHQAAMO4AAGDcAQAA4w4AABh3AAAw7gAAgHEHAADjDgAAGHcAAMC4AwCAcQcAAIw7AABg3AEAwLgDAADGHQAAMO4AAGDcAQAA4w4AAMYdAAAw7gAAgHEHAADjDgAAGHcAAMC4AwCAcQcAAIw7AAAYdwAAwLgDAADGHQAAjDsAAGDcAQAA4w4AAMYdAAAw7gAAgHEHAADjDgAAGHcAADDuAACAcQcAAIw7AAAYdwAAwLgDAADGHQAAjDsAAGDcAQDAuAMAAMYdAAAw7gAAYNwBAADjDgAAGHcAADDuAACAcQcAAIw7AAAYdwAAwLgDAIBxBwAAjDsAAGDcAQDAuAMAAMYdAAAw7gAAYNwBAADjDgAAxl0CAAAw7gAAgHEHAADjDgAAGHcAAMC4AwCAcQcAAIw7AABg3AEAwLgDAADGHQAAjDsAAGDcAQAA4w4AAMYdAAAw7gAAgHEHAADjDgAAGHcAAMC4AwCAcQcAAIw7AAAYdwAAwLgDAADGHQAAjDsAAGDcAQAA4w4AAMYdAAAw7gAAYNwBAADjDgAAGHcAADDuAACAcQcAAIw7AAAYdwAAwLgDAADGHQAAjDsAAGDcAQDAuAMAAMYdAAAw7gAAYNwBAADjDgAAGHcAADDuAACAcQcAAOMOAAAYdwAAwLgDAIBxBwAAjDsAAGDcAQDAuAMAAMYdAAAw7gAAYNwBAADjDgAAxh0AADDuAACAcQcAAOMOAAAYdwAAwLgDAIBxBwAAjDsAABh3AADAuAMAAMYdAACMOwAAYNwBAADjDgAAxh0AADDuAACAcQcAAOMOAAAYdwAAMO4AAIBxBwAAjDsAABh3AADg2gJcrgawoN/v0AAAAABJRU5ErkJggg=='  # 2662 bytes
PNG_PALETTE_INTERLACED = 'iVBORw0KGgoAAAANSUhEUgAAAB4AAAAUCAMAAACtdX32AAADAFBMVEX/AP8AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABjigeHAAAAAnRSTlP/AOW3MEoAAAAZSURBVHicY2BABYyoAE12VBoCRqUhgDRpAGT5AS0engTaAAAAAElFTkSuQmCC'  # 876 bytes
PNG_GRAY16 = 'iVBORw0KGgoAAAANSUhEUgAAAAoAAAAKEAAAAAD4yUwiAAAAE0lEQVR4nGNsaGDAAEyYQkNHEADZhgEUo28FmwAAAABJRU5ErkJggg=='  # 76 bytes


HARNESS = r"""
#include <stdio.h>
#include <stdlib.h>
#include "muse_image.h"
int main(int argc, char **argv) {
    if (argc != 4) return 2;
    FILE *f = fopen(argv[1], "rb");
    static unsigned char buf[1 << 20];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    muse_image_t img;
    char err[64];
    printf("%s|", muse_image_kind_name(muse_image_kind(buf, n)));
    if (!muse_image_decode(buf, n, atoi(argv[2]), atoi(argv[3]), &img, err, sizeof(err))) {
        printf("error %s\n", err);
        return 0;
    }
    /* Size, then the pixels a quarter and three quarters across, halfway down. */
    printf("%d %d %d %d %04x %04x\n", img.w, img.h, img.src_w, img.src_h,
           img.px[(img.h / 2) * img.w + img.w / 4], img.px[(img.h / 2) * img.w + img.w * 3 / 4]);
    free(img.px);
    return 0;
}
"""


def rgb565(r, g, b):
    return (r & 0xF8) << 8 | (g & 0xFC) << 3 | b >> 3


def near(value, r, g, b, slack=24):
    """Lossy decoding and RGB565 are both approximate: each channel within slack."""
    vr, vg, vb = (value >> 11) << 3, ((value >> 5) & 63) << 2, (value & 31) << 3
    return abs(vr - r) <= slack and abs(vg - g) <= slack and abs(vb - b) <= slack


class MuseImage(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        out = Path(cls.tmp.name)
        (out / 'harness.c').write_text(HARNESS)
        try:
            png = subprocess.run(['pkg-config', '--cflags', '--libs', 'libpng'], capture_output=True, text=True)
            cls.png = png.returncode == 0
        except FileNotFoundError:
            cls.png = False
        srcs = [str(p) for d in ('dec', 'dsp', 'utils') for p in sorted((WEBP / 'src' / d).glob('*.c'))]
        cc = shlex.split(os.environ.get('CC', 'cc'))
        cmd = [*cc, '-std=gnu11', '-O1', '-Wall', '-Wno-unused-function', '-Wno-unused-but-set-variable',
               '-DMUSE_IMAGE_HOST', '-DHAVE_CONFIG_H', f'-DMUSE_IMAGE_WITH_PNG={int(cls.png)}', '-DWEBP_SWAP_16BIT_CSP=1',
               '-include', str(ROOT / 'tests/host_compat.h'),
               '-I', str(ROOT / 'components/muse'), '-I', str(WEBP),
               str(out / 'harness.c'), str(ROOT / 'components/muse/muse_image.c'), *srcs,
               '-o', str(out / 'image')]
        if cls.png:
            cmd += shlex.split(png.stdout)
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        cls.binary = out / 'image'
        cls.dir = out

    def decode(self, b64, box=(480, 480)):
        path = self.dir / 'in.bin'
        path.write_bytes(base64.b64decode(b64))
        ran = subprocess.run([str(self.binary), str(path), str(box[0]), str(box[1])], capture_output=True, text=True)
        self.assertEqual(ran.returncode, 0, ran.stderr)
        kind, rest = ran.stdout.strip().split('|', 1)
        if rest.startswith('error '):
            return kind, rest[6:]
        w, h, sw, sh, left, right = rest.split()
        return kind, (int(w), int(h), int(sw), int(sh), int(left, 16), int(right, 16))

    def test_webp_is_scaled_to_fit_as_it_decodes(self):
        kind, (w, h, sw, sh, left, right) = self.decode(WEBP_LOSSY_BIG)
        self.assertEqual((kind, w, h, sw, sh), ('WebP', 480, 320, 1200, 800))
        self.assertTrue(near(left, 220, 20, 20), hex(left))
        self.assertTrue(near(right, 20, 20, 220), hex(right))

    def test_webp_transparency_goes_over_white(self):
        kind, (w, h, sw, sh, left, right) = self.decode(WEBP_ALPHA)
        self.assertEqual((kind, w, h), ('WebP', 40, 20))   # smaller than the box: as it is
        self.assertEqual(left, rgb565(0, 200, 0))
        self.assertEqual(right, 0xFFFF)

    def test_animated_webp_is_refused(self):
        self.assertEqual(self.decode(WEBP_ANIMATED), ('WebP', 'an animated WebP'))

    def test_not_an_image_is_refused(self):
        kind, why = self.decode(base64.b64encode(b'<html>nope</html>').decode())
        self.assertEqual((kind, why), ('not an image', "a format this can't decode"))

    def test_png_is_averaged_down_a_row_at_a_time(self):
        if not self.png:
            self.skipTest('no libpng on this host')
        kind, (w, h, sw, sh, left, right) = self.decode(PNG_BIG, (466, 466))
        self.assertEqual((kind, w, h, sw, sh), ('PNG', 466, 279, 1000, 600))
        self.assertEqual(left, rgb565(250, 250, 0))
        self.assertEqual(right, 0)

    def test_png_palette_interlaced_and_16_bit_grey(self):
        if not self.png:
            self.skipTest('no libpng on this host')
        kind, (w, h, _, _, left, right) = self.decode(PNG_PALETTE_INTERLACED)
        self.assertEqual((kind, w, h), ('PNG', 30, 20))
        self.assertEqual(left, rgb565(255, 0, 255))
        self.assertEqual(right, 0xFFFF)   # its transparent index, over white
        kind, (w, h, _, _, left, _) = self.decode(PNG_GRAY16)
        self.assertEqual((kind, w, h, left), ('PNG', 10, 10, rgb565(0x80, 0x80, 0x80)))

    def test_truncated_png_fails_cleanly(self):
        if not self.png:
            self.skipTest('no libpng on this host')
        cut = base64.b64encode(base64.b64decode(PNG_BIG)[:300]).decode()
        kind, why = self.decode(cut)
        self.assertEqual(kind, 'PNG')
        self.assertIsInstance(why, str)


if __name__ == '__main__':
    unittest.main()
