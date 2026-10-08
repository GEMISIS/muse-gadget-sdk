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

"""What on-device speech (muse_tts_clean) hands SVOX Pico for a reply's text."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MuseTtsTextTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / "muse_tts_text"
        cc = shlex.split(os.environ.get("CC", "cc"))
        compiled = subprocess.run(
            [*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
             "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
             "-I", str(ROOT / "components/muse"),
             str(ROOT / "tests/muse_tts_text_harness.c"),
             str(ROOT / "components/muse/muse_tts_text.c"),
             "-o", str(cls.binary)],
            capture_output=True, text=True,
        )
        if compiled.returncode:
            raise AssertionError(compiled.stdout + compiled.stderr)

    def clean(self, text, cap=256):
        env = {**os.environ,
               "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1",
               "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"}
        ran = subprocess.run([str(self.binary), text, str(cap)],
                             capture_output=True, env=env)
        self.assertEqual(ran.returncode, 0, ran.stderr.decode())
        return ran.stdout.decode()

    def test_plain_text_is_unchanged(self):
        self.assertEqual(self.clean("Hello there. It's 5 o'clock!"),
                         "Hello there. It's 5 o'clock!")

    def test_markdown_is_dropped(self):
        self.assertEqual(self.clean("**Bold** and _soft_ `code` ## Title"),
                         "Bold and soft code Title")

    def test_lines_and_list_items_become_pauses(self):
        self.assertEqual(self.clean("- milk\n- eggs\n\n* bread"),
                         "milk. eggs. bread")
        self.assertEqual(self.clean("Done:\nyes"), "Done: yes")

    def test_links_are_said_as_link(self):
        self.assertEqual(self.clean("See [the docs](https://example.com/a) or https://x.io/b now"),
                         "See the docs or link now")

    def test_images_and_workspace_files_are_not_said(self):
        panda = "![red panda](sandbox://workspace/muse-gadget-216/images/red-panda-480.jpg)"
        self.assertEqual(self.clean(f"Here you go! {panda} Cute, right?"),
                         "Here you go! Cute, right?")
        self.assertEqual(self.clean(f"Here:\n\n{panda}\n\nCute."), "Here: Cute.")
        self.assertEqual(self.clean(panda), "")
        self.assertEqual(self.clean("![](https://x.io/a.png)Done"), "Done")
        self.assertEqual(self.clean("Saved to sandbox://workspace/a/b.jpg for you"),
                         "Saved to for you")
        self.assertEqual(self.clean("It's in `workspace/muse-gadget-216/images/x.jpg` now"),
                         "It's in now")
        self.assertEqual(self.clean("See [the panda](sandbox://workspace/x.jpg)."), "See the panda.")
        # Not an image: said as before.
        self.assertEqual(self.clean("Wow![sic] ok"), "Wow!sic ok")
        self.assertEqual(self.clean("my workspace/desk"), "my workspace/desk")

    def test_typography_and_emoji(self):
        self.assertEqual(self.clean("It’s “fine” — really… \U0001F600 café"),
                         "It's \"fine\", really... café")

    def test_stays_within_the_buffer(self):
        self.assertEqual(self.clean("abcdefghij", 5), "abcd")
        # Cut off for good: a later, shorter piece isn't squeezed in.
        self.assertEqual(self.clean("abcéz", 5), "abc")
        out = self.clean("word " * 100, 32)
        self.assertLess(len(out), 32)
        self.assertEqual(self.clean("anything", 1), "")


if __name__ == "__main__":
    unittest.main()
