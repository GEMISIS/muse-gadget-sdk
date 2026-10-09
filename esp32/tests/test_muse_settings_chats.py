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

"""The named chats muse_settings.c keeps in NVS: the blob from before each chat
kept the gadget mode it last heard still loads, and what chats are told is
saved where the next boot reads it (muse_settings_chats_harness.c)."""

import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# The IDF headers muse_settings.c includes; the harness fakes what they declare.
EMPTY_HEADERS = ('esp_attr.h', 'esp_err.h', 'esp_log.h', 'esp_mac.h', 'esp_random.h', 'freertos/FreeRTOS.h',
                 'freertos/semphr.h', 'nvs.h', 'nvs_flash.h', 'muse_link.h')


class SettingsChats(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        out = Path(cls.tmp.name)
        for name in EMPTY_HEADERS:
            (out / name).parent.mkdir(parents=True, exist_ok=True)
            (out / name).write_text('#pragma once\n')
        cls.binary = out / 'settings'
        cc = shlex.split(os.environ.get('CC', 'cc'))
        result = subprocess.run(
            [*cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
             '-I', str(out), '-I', str(ROOT / 'tests'), '-I', str(ROOT / 'components/muse'),
             str(ROOT / 'tests/muse_settings_chats_harness.c'), '-o', str(cls.binary)],
            capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_case(self, case):
        result = subprocess.run([str(self.binary), str(case)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stderr

    def test_old_blob_is_migrated_and_its_chats_kept(self):
        log = self.run_case(0)
        self.assertIn('chats: 2 moved to the layout', log)

    def test_told_modes_are_saved_and_read_back(self):
        self.run_case(1)

    def test_new_chat_keeps_the_mode_its_first_message_told(self):
        self.run_case(2)

    def test_chat_picked_by_id_keeps_its_mode_in_ram(self):
        self.run_case(3)

    def test_blob_in_no_known_layout_is_ignored(self):
        log = self.run_case(4)
        self.assertIn('no layout known', log)

    def test_chats_told_older_contract_words_hear_the_new_ones(self):
        log = self.run_case(5)
        self.assertIn('every chat hears them again', log)


if __name__ == '__main__':
    unittest.main()
