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

"""Markdown images in a reply's text (muse_chat_md.h): taken out as the text
streams in, even split across pieces, never shown half-arrived, and the first
one's workspace file kept for Muse to push."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PANDA = "![red panda](sandbox://workspace/muse-gadget-216/images/red-panda-480.jpg)"
PANDA_FILE = "workspace/muse-gadget-216/images/red-panda-480.jpg"


class MuseChatMdTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / "muse_chat_md"
        cc = shlex.split(os.environ.get("CC", "cc"))
        compiled = subprocess.run(
            [*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
             "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
             "-I", str(ROOT / "components/muse"),
             str(ROOT / "tests/muse_chat_md_harness.c"),
             "-o", str(cls.binary)],
            capture_output=True, text=True,
        )
        if compiled.returncode:
            raise AssertionError(compiled.stdout + compiled.stderr)

    def run_harness(self, *args):
        env = {**os.environ,
               "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1",
               "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"}
        ran = subprocess.run([str(self.binary), *args], capture_output=True, env=env)
        self.assertEqual(ran.returncode, 0, ran.stderr.decode())
        return ran.stdout.decode()

    def stream(self, *pieces):
        """[(shown, kept) after each piece], (path, label)."""
        *steps, last = self.run_harness(*pieces).split("\x1e")
        return [tuple(step.split("\x1f")) for step in steps], tuple(last.split("\x1f"))

    def test_whole_image_goes_and_its_file_is_kept(self):
        steps, img = self.stream(f"Here you go! {PANDA} Cute, right?")
        self.assertEqual(steps[-1], ("Here you go! Cute, right?",) * 2)
        self.assertEqual(img, (PANDA_FILE, "red panda"))

    def test_image_on_its_own_line_leaves_no_empty_line(self):
        steps, _ = self.stream(f"Here:\n\n{PANDA}\n\nCute.")
        self.assertEqual(steps[-1][1], "Here:\n\nCute.")
        steps, _ = self.stream(f"{PANDA}\nCute.")
        self.assertEqual(steps[-1][1], "Cute.")
        steps, _ = self.stream(PANDA)
        self.assertEqual(steps[-1], ("", ""))

    def test_split_across_pieces_never_shows_half(self):
        pieces = ["Here you go! ![red pa", "nda](sandbox://workspace/muse-gad",
                  "get-216/images/red-panda-480.jpg)", " Cute, right?"]
        steps, img = self.stream(*pieces)
        self.assertEqual([shown for shown, _ in steps],
                         ["Here you go! ", "Here you go! ", "Here you go! ", "Here you go! Cute, right?"])
        self.assertEqual(steps[0][1], "Here you go! ![red pa")   # kept, for the rest to finish
        self.assertEqual(img, (PANDA_FILE, "red panda"))
        # Every split point gives the same text.
        for cut in range(2, len(PANDA)):
            steps, img = self.stream("A ", PANDA[:cut], PANDA[cut:], " B")
            self.assertNotIn("[", "".join(shown for shown, _ in steps), cut)
            self.assertEqual(steps[-1], ("A B", "A B"), cut)
            self.assertEqual(img, (PANDA_FILE, "red panda"), cut)

    def test_not_an_image_is_left_alone(self):
        for text in ["Wow![sic] that's it", "Go! [link](https://x.io) ok", "a ![b\nc](d) e", "Hi!"]:
            steps, img = self.stream(text)
            self.assertEqual(steps[-1], (text, text))
            self.assertEqual(img, ("", ""))
        # Too long to be one still arriving: shown.
        long = "![" + "x" * 500
        steps, _ = self.stream(long)
        self.assertEqual(steps[-1][0], long)

    def test_workspace_files_and_web_images_are_kept(self):
        # A web image is kept whole: Muse downloads it to push it.
        _, img = self.stream("![chart](https://example.com/a.png) and "
                             "![](https://b58af2c5.metaaivm.com/media/raw/workspace/x/y.jpg)")
        self.assertEqual(img, ("https://example.com/a.png", "chart"))
        _, img = self.stream("![](https://b58af2c5.metaaivm.com/media/raw/workspace/x/y.jpg) and "
                             "![chart](https://example.com/a.png)")
        self.assertEqual(img, ("workspace/x/y.jpg", "image"))
        _, img = self.stream("![x](ftp://example.com/a.png)")
        self.assertEqual(img, ("", ""))

    def test_widget_token_goes_and_never_shows_half(self):
        token = "[[hatch_widget:widget-31f0904a-89f7-4071-a169-8006fd06d4fe]]"
        steps, img = self.stream(f"What's it going to be tonight?\n\n{token}")
        self.assertEqual(steps[-1][1].rstrip(), "What's it going to be tonight?")
        self.assertEqual(img, ("", ""))
        steps, _ = self.stream(f"Pick one {token} please.")
        self.assertEqual(steps[-1], ("Pick one please.",) * 2)
        steps, _ = self.stream(f"{token}\nThen this.")
        self.assertEqual(steps[-1][1], "Then this.")
        # Every split point: no part of it is ever shown, nor kept once it's whole.
        for cut in range(1, len(token)):
            steps, _ = self.stream("A ", token[:cut], token[cut:], " B")
            for shown, _ in steps:
                self.assertNotIn("[", shown, cut)
            self.assertEqual(steps[-1], ("A B", "A B"), cut)
        # With an image beside it.
        steps, img = self.stream(f"Here {PANDA} and {token} done")
        self.assertEqual(steps[-1][1], "Here and done")
        self.assertEqual(img, (PANDA_FILE, "red panda"))

    def test_brackets_that_are_not_a_token_stay(self):
        for text in ["[[other]] stays", "a [link](https://x.io)", "[x] y", "[[hatch_widget:\nno"]:
            steps, _ = self.stream(text)
            self.assertEqual(steps[-1], (text, text))
        # A '[' at the end waits for what follows it.
        steps, _ = self.stream("Hi [", "x] there")
        self.assertEqual([shown for shown, _ in steps], ["Hi ", "Hi [x] there"])

    def test_find_and_file(self):
        self.assertEqual(self.run_harness("find", f"ok {PANDA}").split("\x1f"),
                         [PANDA_FILE, "red panda", "-"])
        self.assertEqual(self.run_harness("find", "sandbox://workspace/a.jpg").split("\x1f"),
                         ["-", "-", "workspace/a.jpg"])
        self.assertEqual(self.run_harness("find", "https://h/media/raw/workspace/b.jpg").split("\x1f"),
                         ["-", "-", "workspace/b.jpg"])
        self.assertEqual(self.run_harness("find", "sandbox://").split("\x1f"), ["-", "-", "-"])
        self.assertEqual(self.run_harness("find", "https://example.com/a.png").split("\x1f"),
                         ["-", "-", "https://example.com/a.png"])
        self.assertEqual(self.run_harness("find", "https://").split("\x1f"), ["-", "-", "-"])


if __name__ == "__main__":
    unittest.main()
