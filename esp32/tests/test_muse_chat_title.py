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

"""A new chat's title (muse_chat_title.c): the stand-ins the Muse gives one
before its real title ("Processing audio", "Audio file", the voice note's file
name, an id) are never a name, a real title always is; and a chat the Muse
never titles is named by a few of its first words."""

import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

STAND_INS = [
    '', ' ', '   \t ', 'New chat', 'new chat', 'NEW CHAT', 'New Chat.', '"New chat"', 'New conversation',
    'Untitled', 'Untitled chat', '(untitled)', 'Untitled conversation', 'No title', 'Chat', 'Session',
    'Processing audio', 'Processing audio...', 'Processing audio…', 'Processing file', 'Processing the file',
    'Processing your request', 'Processing...', 'Processing', 'processing voice note',
    'Audio file', 'audio file', 'Transcribe audio file', 'Summarize audio file content', 'Audio File Transcription',
    'Voice note', 'Voice message', 'Voice memo', 'Audio message', 'Audio recording', 'Recording', 'Audio',
    'Transcribing...', 'Uploading file', 'Generate session title', 'Generating title', 'Generating title...',
    'Title generating...', 'voice_note.wav', 'recording-20261010.m4a', 'audio.mp3', 'note 1.webm', 'IMG_1234.jpg',
    '6d757365-6761-4467-8000-0a1b2c3d4e5f', 'Chat 6d757365-6761-4467-8000-0a1b2c3d4e5f', 'audio_1712345678',
    'rec-20261010-091512', 'Attachment', 'File',
]

REAL = [
    'Lisbon weekend weather', 'Hangboard session warm-up', 'Pasta for four', 'Tea or coffee?', 'Rename my files',
    'Node.js build errors', 'Processor comparison: M3 vs M4', 'Session musicians for hire', 'The 1975 tour dates',
    'Fixing a leaky faucet', 'Gift ideas for Mom', "What's on today", 'Chat GPT alternatives', 'Audiobook picks',
    'Recording studio booking', 'New chat app ideas', 'Untie a knot', 'UUID vs ULID', 'Version 2.0 release notes',
]


class ChatTitle(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shlex.split(os.environ.get('CC', 'cc'))
        if not cc or shutil.which(cc[0]) is None:
            raise unittest.SkipTest('C compiler not available')
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.binary = Path(cls.tmp.name) / 'title'
        result = subprocess.run(
            [*cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'components/muse'),
             str(ROOT / 'tests/muse_chat_title_harness.c'), str(ROOT / 'components/muse/muse_chat_title.c'),
             '-o', str(cls.binary)],
            capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_lines(self, args, lines):
        result = subprocess.run([str(self.binary), *args], input=''.join(f'{l}\n' for l in lines),
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        out = result.stdout.split('\n')[:-1]
        self.assertEqual(len(out), len(lines))
        return out

    def test_stand_ins_are_never_a_name(self):
        out = self.run_lines(['placeholder'], STAND_INS)
        self.assertEqual([t for t, o in zip(STAND_INS, out) if o != '1'], [])

    def test_real_titles_are_kept(self):
        out = self.run_lines(['placeholder'], REAL)
        self.assertEqual([t for t, o in zip(REAL, out) if o != '0'], [])

    def test_a_newline_in_a_stand_in_still_reads_as_one(self):
        self.assertEqual(self.run_lines(['placeholder'], ['Processing\\naudio']), ['1'])

    def test_excerpt_of_the_first_words(self):
        words = [
            "what's the weather like in lisbon this weekend",
            '  pasta   for four ',
            'hi',
            '',
            'supercalifragilisticexpialidociousness',
            'line one\\nline two',
            'a, b, c, d, e, f, g, h, i, j, k',
            'café café café café café café',
            'exactly twenty four byt',
            '\u00e9' * 15,
        ]
        out = self.run_lines(['excerpt', '25'], words)
        self.assertEqual(out, [
            "What's the weather...",
            'Pasta for four',
            'Hi',
            '',
            'Supercalifragilistice...',
            'Line one line two',
            'A, b, c, d, e, f, g...',
            'Café café café...',   # cut before a word, never inside its letters
            'Exactly twenty four byt',
            '\u00e9' * 10 + '...',   # one long word: cut between its letters' bytes, not through one
        ])
        for o in out:
            self.assertLessEqual(len(o.encode()), 24)


if __name__ == '__main__':
    unittest.main()
