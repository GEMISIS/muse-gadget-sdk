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

"""How a press answers (muse_style_pressable): the source's promises. The
passcode's digits all look and click alike; a tap's click never plays over
Muse listening or speaking, nor with Touch sounds off; and the presses cost
no internal RAM."""

from __future__ import annotations

import re
import unittest
from pathlib import Path

MUSE = Path(__file__).resolve().parents[1] / "components/muse"


def read(name: str) -> str:
    return (MUSE / name).read_text()


def function(src: str, signature: str) -> str:
    start = src.index(signature)
    return src[start:src.index("\n}\n", start)]


class MusePressTest(unittest.TestCase):
    def test_digits_alike(self):
        key = function(read("muse_lock_ui.c"), "static lv_obj_t *key(")
        self.assertEqual(key.count("muse_style_pressable("), 1)
        self.assertIn("muse_style_pressable(b, MUSE_PRESS_KEY, false)", key)
        self.assertNotIn("muse_style_click", read("muse_lock_ui.c"))   # no click but the keys' own

    def test_tap_gated(self):
        earcon = function(read("muse_voice.c"), "void muse_voice_earcon(")
        self.assertLess(earcon.index("MUSE_MODE_IDLE"), earcon.index("s_earcon = which"))
        self.assertIn("muse_settings_speaker_on()", earcon)
        self.assertIn("!muse_settings_touch_sounds()", earcon)

    def test_touch_sounds_kept(self):
        settings = read("muse_settings.c")
        self.assertIn('save_u8("touch_snd", on)', settings)
        self.assertIn('nvs_get_u8(s_nvs, "touch_snd", &b)', settings)
        self.assertIn(".touch_sounds = true", settings)
        self.assertIn('"Touch sounds"', read("muse_settings_ui.c"))

    def test_presses_in_flash(self):
        src = read("muse_style.c")
        for line in src.splitlines():
            if re.match(r"static (?!const|LV_STYLE_CONST_INIT|void|lv_obj_t|bool)", line):
                self.fail(f"internal RAM: {line}")


if __name__ == "__main__":
    unittest.main()
