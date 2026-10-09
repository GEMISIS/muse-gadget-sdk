# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""muse_jpeg.c: progressive JPEGs (the web's, which the ROM's decoder
refuses) decoded on the host and compared with Pillow's decode of the same
file: at full scale (PSNR), reduced (6/8, 4/8, 2/8) and from the DC
coefficients alone (1/8: its size and rough colours), the scale falling back
as the memory budget shrinks. Baseline JPEGs are left to the ROM's decoder
(muse_jpeg_is_baseline). Damaged and cut-short files are refused or shown in
part, never read past (AddressSanitizer). The images are made here with
Pillow; the tests skip without it and numpy."""
from pathlib import Path
import io
import os
import random
import shlex
import subprocess
import tempfile
import unittest

try:
    import numpy as np
    from PIL import Image
except ImportError:   # pragma: no cover
    np = Image = None

ROOT = Path(__file__).resolve().parents[1]
BIG = 1 << 30   # a budget that's never the limit


def scene(w, h, seed=1):
    """Something photo-like: gradients, discs, texture and a little noise."""
    rng = np.random.default_rng(seed)
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    img = np.stack([40 + 160 * x / w, 60 + 120 * y / h, 200 - 150 * (x + y) / (w + h)], axis=-1)
    for _ in range(6):
        cx, cy, r = rng.uniform(0, w), rng.uniform(0, h), rng.uniform(min(w, h) / 12, min(w, h) / 4)
        col = rng.uniform(0, 255, 3)
        inside = (x - cx) ** 2 + (y - cy) ** 2 < r * r
        img[inside] = img[inside] * 0.3 + col * 0.7
    img += 18 * np.sin(x / 7.0)[..., None] * np.cos(y / 11.0)[..., None]
    img += rng.normal(0, 4, img.shape)
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), 'RGB')


def jpeg(img, **kw):
    b = io.BytesIO()
    img.save(b, 'JPEG', **kw)
    return b.getvalue()


def to_rgb(px565, w, h):
    v = np.frombuffer(px565, dtype=np.uint16).reshape(h, w).astype(np.int32)
    r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], axis=-1)


def quantize565(rgb):
    """Pillow's decode as it would look in RGB565."""
    a = np.asarray(rgb, dtype=np.int32)
    r, g, b = a[..., 0] >> 3, a[..., 1] >> 2, a[..., 2] >> 3
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], axis=-1)


def psnr(a, b):
    mse = np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2)
    return 99.0 if mse == 0 else 10 * np.log10(255 * 255 / mse)


@unittest.skipIf(np is None, 'needs numpy and Pillow')
class MuseJpeg(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.dir = Path(cls.tmp.name)
        cls.binary = cls.dir / 'muse_jpeg'
        cc = shlex.split(os.environ.get('CC', 'cc'))
        compiled = subprocess.run(
            [*cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
             '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-DMUSE_JPEG_HOST',
             '-I', str(ROOT / 'components/muse'),
             str(ROOT / 'tests/muse_jpeg_harness.c'), str(ROOT / 'components/muse/muse_jpeg.c'),
             '-lm', '-o', str(cls.binary)],
            capture_output=True, text=True)
        if compiled.returncode:
            raise AssertionError(compiled.stdout + compiled.stderr)

    def run_decoder(self, data, fit=(4096, 4096), budget=BIG, eighths=8):
        src, out = self.dir / 'in.jpg', self.dir / 'out.565'
        src.write_bytes(data)
        if out.exists():
            out.unlink()
        ran = subprocess.run([str(self.binary), str(src), str(out), str(fit[0]), str(fit[1]), str(budget),
                              str(eighths)], capture_output=True, text=True,
                             env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0:abort_on_error=1',
                                  'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'})
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        info, result = ran.stdout.splitlines()
        info = info.split()[1:]
        if result.startswith('error '):
            return info, result[6:], None
        w, h, sw, sh, eighths, prog, need = (int(v) for v in result.split()[1:8])
        meta = dict(w=w, h=h, src_w=sw, src_h=sh, eighths=eighths, progressive=bool(prog), need=need)
        return info, meta, to_rgb(out.read_bytes(), w, h)

    def full_matches_pillow(self, data, min_db):
        info, meta, rgb = self.run_decoder(data)
        self.assertIsInstance(meta, dict, meta)
        ref = Image.open(io.BytesIO(data)).convert('RGB')
        self.assertEqual((meta['w'], meta['h'], meta['eighths']), (ref.width, ref.height, 8))
        db = psnr(rgb, quantize565(ref))
        self.assertGreater(db, min_db, f'{db:.1f} dB')
        return info, meta

    # ---- Full scale, against Pillow --------------------------------------------

    def test_progressive_420(self):
        info, meta = self.full_matches_pillow(jpeg(scene(320, 240), quality=88, progressive=True), 32)
        self.assertEqual(info, ['320', '240', '3', 'c2', '0'])   # not the ROM decoder's
        self.assertTrue(meta['progressive'])

    def test_progressive_444(self):
        self.full_matches_pillow(jpeg(scene(256, 200, 2), quality=90, progressive=True, subsampling=0), 38)

    def test_progressive_422_optimized(self):
        self.full_matches_pillow(jpeg(scene(200, 160, 3), quality=80, progressive=True, subsampling=1,
                                      optimize=True), 32)

    def test_progressive_grey(self):
        info, _ = self.full_matches_pillow(jpeg(scene(250, 190, 4).convert('L'), quality=85, progressive=True), 40)
        self.assertEqual(info[2], '1')

    def test_progressive_odd_size_with_restarts(self):
        data = jpeg(scene(301, 217, 5), quality=85, progressive=True, restart_marker_blocks=5)
        self.assertIn(b'\xff\xdd', data)
        self.full_matches_pillow(data, 32)
        data = jpeg(scene(77, 333, 6), quality=75, progressive=True, subsampling=0, restart_marker_rows=1)
        self.full_matches_pillow(data, 36)

    def test_baseline_is_the_rom_decoders_but_decodes_here_too(self):
        data = jpeg(scene(160, 120, 7), quality=85)
        info, meta = self.full_matches_pillow(data, 32)
        self.assertEqual(info, ['160', '120', '3', 'c0', '1'])   # muse_present.c: tjpgd first
        self.assertFalse(meta['progressive'])

    # ---- Reduced, and the DC alone -------------------------------------------

    def test_reduced_scales_match_pillow_shrunk(self):
        data = jpeg(scene(400, 300, 8), quality=90, progressive=True)
        ref = quantize565(Image.open(io.BytesIO(data)).convert('RGB'))
        for eighths, min_db in ((4, 28), (2, 25)):
            f = 8 // eighths
            _, meta, rgb = self.run_decoder(data, eighths=eighths)
            self.assertEqual((meta['eighths'], meta['w'], meta['h']), (eighths, 400 // f, 300 // f))
            small = ref.reshape(300 // f, f, 400 // f, f, 3).mean(axis=(1, 3))
            db = psnr(rgb, small)
            self.assertGreater(db, min_db, f'{eighths}/8: {db:.1f} dB')
        # Not a power of two: 6/8, a 6-point IDCT.
        _, meta, rgb = self.run_decoder(data, eighths=6)
        self.assertEqual((meta['eighths'], meta['w'], meta['h']), (6, 300, 225))
        small = Image.open(io.BytesIO(data)).convert('RGB').resize((300, 225), Image.BOX)
        db = psnr(rgb, quantize565(small))
        self.assertGreater(db, 28, f'6/8: {db:.1f} dB')

    def test_dc_only_size_and_colours(self):
        img = scene(1001, 603, 9)
        data = jpeg(img, quality=85, progressive=True)
        _, meta, rgb = self.run_decoder(data, eighths=1)
        self.assertEqual((meta['eighths'], meta['w'], meta['h']), (1, 126, 76))   # each 8x8 block a pixel
        ref = np.asarray(img.resize((126, 76), Image.BOX), dtype=np.float64)
        self.assertGreater(psnr(rgb, ref), 24)
        self.assertLess(np.abs(rgb.mean(axis=(0, 1)) - ref.mean(axis=(0, 1))).max(), 4)

    def test_dc_only_skips_the_ac_and_takes_far_less(self):
        data = jpeg(scene(1920, 1080, 10), quality=85, progressive=True)
        _, full, _ = self.run_decoder(data, fit=(1920, 1080))
        _, dc, _ = self.run_decoder(data, fit=(1920, 1080), eighths=1)
        self.assertEqual((full['eighths'], dc['eighths'], dc['w'], dc['h']), (8, 1, 240, 135))
        self.assertLess(dc['need'] * 20, full['need'])

    # ---- Fitting the screen, and the memory there is -------------------------

    def test_scale_follows_the_screen_then_the_budget(self):
        data = jpeg(scene(1920, 1080, 11), quality=85, progressive=True)
        # As much as the screen shows: 2/8 of 1920 is 480.
        _, meta, _ = self.run_decoder(data, fit=(480, 480))
        self.assertEqual((meta['eighths'], meta['w'], meta['h'], meta['src_w']), (2, 480, 270, 1920))
        roomy = meta['need']
        # Less memory than that: down to the DC alone, a 240 px image.
        _, meta, _ = self.run_decoder(data, fit=(480, 480), budget=roomy - 1)
        self.assertEqual((meta['eighths'], meta['w'], meta['h']), (1, 240, 135))
        self.assertLess(meta['need'], roomy)
        # Not even that: refused, for Muse to push it.
        _, why, _ = self.run_decoder(data, fit=(480, 480), budget=meta['need'] - 1)
        self.assertEqual(why, 'out of memory')

    def test_a_cdn_copy_fits_whole(self):
        data = jpeg(scene(640, 427, 12), quality=82, progressive=True)
        _, meta, rgb = self.run_decoder(data, fit=(480, 480))
        self.assertEqual((meta['eighths'], meta['w'], meta['h']), (6, 480, 321))   # 6/8 of 640: just the screen's
        self.assertLess(meta['need'], 900 * 1024)
        ref = quantize565(Image.open(io.BytesIO(data)).convert('RGB').resize((480, 321), Image.BOX))
        self.assertGreater(psnr(rgb, ref), 28)

    # ---- What it refuses, and damage ------------------------------------------

    def test_refusals(self):
        _, why, _ = self.run_decoder(b'<html>no</html>')
        self.assertEqual(why, 'not a JPEG')
        cmyk = jpeg(Image.new('CMYK', (32, 32), (10, 20, 30, 40)), progressive=True)
        _, why, _ = self.run_decoder(cmyk)
        self.assertEqual(why, 'unsupported JPEG: CMYK')
        _, why, _ = self.run_decoder(b'\xff\xd8\xff\xd9')
        self.assertEqual(why, 'not a JPEG with a frame')

    def test_cut_short_shows_what_came(self):
        data = jpeg(scene(320, 240, 13), quality=85, progressive=True)
        _, meta, rgb = self.run_decoder(data[:len(data) // 2])
        self.assertIsInstance(meta, dict, meta)   # the early scans: a softer copy
        ref = quantize565(Image.open(io.BytesIO(data)).convert('RGB'))
        self.assertGreater(psnr(rgb, ref), 18)

    def test_damage_is_never_read_past(self):
        data = bytearray(jpeg(scene(160, 120, 14), quality=85, progressive=True, restart_marker_blocks=3))
        rng = random.Random(15)
        for _ in range(40):
            bad = bytearray(data)
            for _ in range(rng.randint(1, 12)):
                bad[rng.randrange(2, len(bad))] = rng.randrange(256)
            cut = bad[:rng.randrange(4, len(bad) + 1)] if rng.random() < 0.3 else bad
            self.run_decoder(bytes(cut), fit=(120, 120), eighths=rng.choice((8, 6, 4, 2, 1)))


if __name__ == '__main__':
    unittest.main()
