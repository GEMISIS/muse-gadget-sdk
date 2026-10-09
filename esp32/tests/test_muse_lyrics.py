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

"""A reply's captions as lyrics (muse_lyrics.h, in muse_chat_text.c): the
reply wrapped as the pages are, the line the speech is on and the words of it
lit, and where the speech is through the text, holding a beat at commas and
sentences' ends."""

from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PAUSE_COMMA = 4   # muse_lyrics.h
PAUSE_STOP = 8
CLOSING = "、。，．：；！？）」』】》"
REPLY = (
    "Good morning! It's 72 degrees and sunny in Seattle, with a light breeze. "
    "Later, around 3:30, clouds roll in... and there's a 40% chance of rain by 9.5 tonight."
)


class LyricsTest(unittest.TestCase):
    binary: Path

    @classmethod
    def setUpClass(cls) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            raise unittest.SkipTest("C compiler not available")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.tmp.name) / "muse_serial_chat_harness"
        proc = subprocess.run(
            [
                *cc,
                "-include",
                str(ROOT / "tests" / "host_compat.h"),
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(ROOT / "components" / "muse"),
                str(ROOT / "tests" / "muse_serial_chat_harness.c"),
                str(ROOT / "components" / "muse" / "muse_chat_text.c"),
                str(ROOT / "components" / "muse" / "muse_text.c"),
                "-o",
                str(cls.binary),
            ],
            cwd=ROOT,
            text=True,
            capture_output=True,
        )
        if proc.returncode:
            raise AssertionError(proc.stdout + proc.stderr)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def run_harness(self, text: str, *args: str) -> list[list[str]]:
        proc = subprocess.run(
            [str(self.binary), *args], input=text.encode(), capture_output=True, check=True
        )
        return [line.split(" ") for line in proc.stdout.decode().splitlines()]

    def lyrics(self, text: str, cols: int) -> tuple[bytes, list[bytes], list[tuple[int, int, int, int]]]:
        raw = text.encode()
        rows = self.run_harness(text, "lyrics", str(cols))
        lines = [raw[int(r[1]):int(r[1]) + int(r[2])] for r in rows if r[0] == "L"]
        at = [(int(r[1]), int(r[2]), int(r[3]), int(r[4])) for r in rows if r[0] == "A"]
        return raw, lines, at

    def pace(self, text: str) -> tuple[int, list[int]]:
        rows = self.run_harness(text, "pace")
        weight = int(rows[0][1])
        return weight, [int(r[2]) for r in rows[1:]]

    def wrap(self, text: str, cols: int) -> list[str]:
        proc = subprocess.run(
            [str(self.binary), "caption", str(cols)], input=text.encode(), capture_output=True, check=True
        )
        return proc.stdout.decode().split("\n")

    # ---- the lines ----

    def test_lines_are_the_pages_lines(self) -> None:
        for text in (REPLY, "广州今天雷阵雨，30°/24°，还伴有8级左右的雷雨大风和短时强降水，出门小心。",
                     "Here you go!\nA second message, after the first. ![red pa"):
            for cols in (12, 16, 24):
                _, lines, _ = self.lyrics(text, cols)
                self.assertEqual([l.decode() for l in lines], self.wrap(text, cols), (text, cols))

    def test_an_image_still_arriving_isnt_a_line(self) -> None:
        _, lines, _ = self.lyrics("Here you go! ![red panda](sandbox://workspace/a", 40)
        self.assertEqual(lines, [b"Here you go!"])

    # ---- the line the speech is on ----

    def test_the_line_turns_as_its_first_word_is_said(self) -> None:
        raw, lines, at = self.lyrics(REPLY, 16)
        starts = [raw.index(l, sum(len(x) for x in lines[:i])) for i, l in enumerate(lines)]
        for pos, line, _, _ in at:
            if line:
                self.assertLess(starts[line], pos, "turned once its first byte's said")
            if line + 1 < len(starts):
                self.assertLessEqual(pos, starts[line + 1], "never ahead of the speech")
        # Never back, and every line has its turn.
        seen = [line for _, line, _, _ in at]
        self.assertEqual(seen, sorted(seen))
        self.assertEqual(sorted(set(seen)), list(range(len(lines))))

    # ---- the words lit ----

    def test_a_word_lights_whole_as_its_begun(self) -> None:
        raw, lines, at = self.lyrics(REPLY, 16)
        offsets, o = [], 0
        for l in lines:
            o = raw.index(l, o)
            offsets.append(o)
        last = (-1, -1)
        for pos, line, lit, word in at:
            text = lines[line]
            shown = text[:lit]
            if lit:
                self.assertTrue(lit == len(text) or text[lit:lit + 1] == b" ", (pos, shown))
                self.assertNotEqual(shown[-1:], b" ")
                self.assertTrue(word == 0 or text[word - 1:word] == b" ")
                self.assertLess(offsets[line] + word, pos, "lit as it's begun, not before")
            # The word being said is lit: everything up to the speech.
            said = max(0, min(pos - offsets[line], len(text)))
            self.assertGreaterEqual(lit, len(text[:said].rstrip()), (pos, shown))
            if pos == 0:
                self.assertEqual(lit, 0, "nothing lit before the speech")
            self.assertGreaterEqual((line, lit), last, "words never go out")
            last = (line, lit)

    def test_cjk_lights_a_character_at_a_time(self) -> None:
        text = "还伴有八级左右的雷雨大风，出门小心。"
        raw, lines, at = self.lyrics(text, 40)
        lit_runs = sorted({raw[:lit].decode() for _, _, lit, _ in at})
        # Each step is one more character, its closing punctuation with it.
        for shown in lit_runs[1:]:
            after = text[len(shown):len(shown) + 1]
            self.assertTrue(not after or after not in CLOSING, shown)
        self.assertEqual(len(lit_runs), len(text) - 2 + 1, "a character at a time, and nothing lit")
        self.assertIn("还伴有八级左右的雷雨大风，", lit_runs)
        self.assertNotIn("还伴有八级左右的雷雨大风", lit_runs)
        self.assertEqual(lit_runs[-1], text)

    def test_mixed_words_and_cjk(self) -> None:
        raw, _, at = self.lyrics("Muse 说 hello world", 40)
        lit_runs = sorted({raw[:lit].decode() for _, _, lit, _ in at}, key=len)
        self.assertEqual(lit_runs, ["", "Muse", "Muse 说", "Muse 说 hello", "Muse 说 hello world"])

    # ---- the pace ----

    def test_pauses_follow_clause_and_sentence_ends(self) -> None:
        text = "One, two. Three"
        weight, at = self.pace(text)
        self.assertEqual(weight, len(text) + PAUSE_COMMA + PAUSE_STOP)
        held = lambda i: at.count(i)
        self.assertEqual(held(text.index(",") + 1), 1 + PAUSE_COMMA)   # its byte's moment, then the pause
        self.assertEqual(held(text.index(".") + 1), 1 + PAUSE_STOP)
        self.assertEqual(held(text.index("w")), 1)
        self.assertEqual(at[weight], len(text))
        self.assertEqual(at, sorted(at), "the speech never goes back")

    def test_no_pause_inside_numbers_or_a_run_of_dots(self) -> None:
        for text in ("It's 3.5 or 12:30 or 1,000 now", "Wait... ok", "He said \"go.\" Then"):
            weight, at = self.pace(text)
            pauses = weight - len(text.encode())
            want = {"It's 3.5 or 12:30 or 1,000 now": 0, "Wait... ok": PAUSE_STOP,
                    "He said \"go.\" Then": PAUSE_STOP}[text]
            self.assertEqual(pauses, want, text)
            if want:
                # The pause after the run, not inside it.
                stop = text.index("...") + 3 if "..." in text else text.index(".\"") + 2
                self.assertEqual(at.count(stop), 1 + PAUSE_STOP, text)   # after the quote that closes it

    def test_cjk_takes_its_bytes_time_and_its_pauses(self) -> None:
        text = "你好，世界。"
        weight, at = self.pace(text)
        self.assertEqual(weight, len(text.encode()) + PAUSE_COMMA + PAUSE_STOP)
        comma_end = len("你好，".encode())
        self.assertEqual(at.count(comma_end), 1 + PAUSE_COMMA)

    def test_line_breaks_between_messages_pause(self) -> None:
        weight, _ = self.pace("Hi there\nNext one")
        self.assertEqual(weight, len("Hi there\nNext one") + PAUSE_STOP)

    def test_the_whole_reply_holds_the_line_at_a_sentences_end(self) -> None:
        # Speech at a steady rate through the weight: the line doesn't turn
        # while the full stop's pause lasts, and the next line's first word
        # isn't lit until it's said.
        text = "Good morning to you. Sunny today"
        raw, lines, lyr = self.lyrics(text, 20)
        weight, at = self.pace(text)
        by_at = {pos: (line, lit) for pos, line, lit, _ in lyr}
        stop = text.index(".") + 1
        during = [by_at[a] for a in at if a == stop]
        self.assertEqual(len(during), 1 + PAUSE_STOP)
        self.assertTrue(all(d == during[0] for d in during))
        self.assertEqual(during[0][0], 0, "still on the first line")


class LyricsContractTest(unittest.TestCase):
    def test_bench_captions_wait_for_the_lock(self) -> None:
        # ">caption=" shows reply-like text: refused while locked, as the rest are.
        src = (ROOT / "components" / "muse" / "muse_input.c").read_text()
        body = src[src.index("static bool console_command("):]
        self.assertLess(body.index("muse_lock_policy_blocks_console(line)"), body.index('"caption='))
        policy = (ROOT / "components" / "muse" / "muse_lock_policy.c").read_text()
        allowed = policy[policy.index("ALLOWED[] = {"):]
        self.assertNotIn("caption", allowed[:allowed.index("};")])

    def test_dropped_when_locked_and_for_a_new_turn(self) -> None:
        ui = (ROOT / "components" / "muse" / "muse_ui.c").read_text()
        lock = ui[ui.index("static void lock_face("):]
        locking = lock[lock.index("if (on) {"):]
        self.assertIn("muse_lyrics_ui_drop();", locking[:locking.index("return;")])
        self.assertIn("muse_lyrics_ui_drop();", ui[ui.index("if (mode == MUSE_MODE_LISTENING && !muse_lock_locked())"):])

    def test_no_new_internal_ram(self) -> None:
        src = (ROOT / "components" / "muse" / "muse_lyrics_ui.c").read_text()
        for line in src.splitlines():
            if line.startswith("static ") and "(" not in line and "const" not in line:
                self.fail(f"internal RAM: {line}")


if __name__ == "__main__":
    unittest.main()
