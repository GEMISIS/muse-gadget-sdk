# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""What Muse says he's at, sorted for the face to act out (muse_activity.h):
the words seen from the device, by keyword at the start of a word in any
case, in order so the more particular wins, and anything else left to the
phone. Built as C and as C++ (the chat session's)."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

CASES = [
    # Seen from the device.
    ("working|is working", "none"),
    ("working|Generating image", "image"),
    ("working|Searching", "search"),
    ("working|Searching news", "news"),
    ("working|Checking calendar", "calendar"),
    ("working|Scheduling reminder", "reminder"),
    ("working|Canceling reminder", "reminder_cancel"),
    ("working|Loading tools", "tools"),
    ("working|Scanning inbox", "mail"),
    ("working|Listing messages", "mail"),
    ("working|Reading emails", "mail"),
    ("working|Fetching emails", "mail"),
    ("working|Calculating", "calc"),
    ("responding|is responding", "respond"),
    ("online|is online", "none"),
    # The rest of the kinds.
    ("|Checking the weather forecast", "weather"),
    ("|Getting directions", "map"),
    ("|Playing your playlist", "music"),
    ("|Drafting a document", "write"),
    ("|Writing code", "write"),
    ("|Saving to memory", "memory"),
    ("|Setting a timer", "reminder"),
    ("|Stopping the alarm", "reminder_cancel"),
    ("|Looking for an image", "search"),
    ("|Searching images", "search"),
    ("|Drawing a cat", "image"),
    ("|Drafting an email", "mail"),
    ("|Searching calendar", "calendar"),
    ("|Upcoming events", "calendar"),
    ("|COMPUTING TOTALS", "calc"),
    # Only at the start of a word: not "display", "remap", "prevent".
    ("|Displaying results", "none"),
    ("|Remapping keys", "none"),
    ("|Preventing errors", "none"),
    ("|Thinking", "none"),
    ("|", "none"),
]


class MuseActivityTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binaries = []
        for lang, compiler, std in (("c", os.environ.get("CC", "cc"), "-std=c11"),
                                    ("c++", os.environ.get("CXX", "c++"), "-std=c++17")):
            binary = Path(cls.temp.name) / f"muse_activity_{lang.replace('+', 'p')}"
            compiled = subprocess.run(
                [*shlex.split(compiler), "-x", lang, std, "-Wall", "-Wextra", "-Werror", "-O1",
                 "-I", str(ROOT / "components/muse"),
                 str(ROOT / "tests/muse_activity_harness.c"), "-o", str(binary)],
                capture_output=True, text=True,
            )
            if compiled.returncode:
                raise AssertionError(compiled.stdout + compiled.stderr)
            cls.binaries.append(binary)

    def test_each_kind_of_words_is_sorted(self):
        for binary in self.binaries:
            ran = subprocess.run([str(binary), *[c for c, _ in CASES]], capture_output=True, text=True)
            self.assertEqual(ran.returncode, 0, ran.stderr)
            for (words, want), got in zip(CASES, ran.stdout.splitlines()):
                self.assertEqual(got, want, f"{words!r} ({binary.name})")


if __name__ == "__main__":
    unittest.main()
