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

from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
# CI supplies pinned upstream sources; local IDF builds already have cJSON.
JSON = Path(os.environ.get(
    "CJSON_SOURCE_DIR", ROOT / "managed_components/espressif__cjson/cJSON"
))
COMMANDS = ("show_text", "set_mode", "set_chat", "list_chats")


class LinkGadgetCommandsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        out = Path(cls.temp.name)
        source = (ROOT / "main/gadget_commands.c").read_text()
        (out / "gadget_commands.inc").write_text(source[source.index("// ---- Commands (host-tested)"):])
        cc = shlex.split(os.environ.get("CC", "cc"))
        flags = ["-Wall", "-Wextra", "-Werror", "-DCJSON_NESTING_LIMIT=16", "-I", str(JSON), "-I", str(out)]
        commands = [
            [*cc, "-std=c11", *flags, "-c", str(JSON / "cJSON.c"), "-o", str(out / "cjson.o")],
            [*cc, "-std=c11", *flags, str(ROOT / "tests/link_gadget_commands_harness.c"), str(out / "cjson.o"),
             "-lm", "-o", str(out / "gadget_commands")],
        ]
        for cmd in commands:
            compiled = subprocess.run(cmd, capture_output=True, text=True)
            if compiled.returncode:
                raise AssertionError(compiled.stdout + compiled.stderr)
        cls.out = out

    def test_params_are_validated_and_reach_the_ui(self):
        result = subprocess.run([str(self.out / "gadget_commands")], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_commands_are_advertised_and_dispatched_with_the_muse_ui(self):
        noise = (ROOT / "main/noise_control.cpp").read_text()
        register = noise[noise.index("static char *build_register_json("):]
        start = register.index("#if CONFIG_MUSE_ENABLED")
        advertised = register[start:register.index("#endif", start)]
        app = (ROOT / "main/app.c").read_text()
        start = app.index("static cJSON *on_ws_command(")
        dispatch = app[start:app.index("unsupported command", start)]
        block = dispatch[dispatch.index("#if CONFIG_MUSE_ENABLED"):]
        block = block[:block.index("#endif")]
        for command in COMMANDS:
            self.assertIn(f'add_command(commands, "{command}"', advertised)
            self.assertIn(f'"{command}"', block)
        cmake = (ROOT / "main/CMakeLists.txt").read_text()
        start = cmake.index("if(CONFIG_MUSE_ENABLED)")
        self.assertIn('"gadget_commands.c"', cmake[start:cmake.index("endif()", start)])

    def test_chat_limits_match_the_firmware(self):
        # The harness fakes muse_settings.h with these: a UUID's length, and
        # the named chats the device keeps.
        settings = (ROOT / "components/muse/muse_settings.h").read_text()
        self.assertRegex(settings, r"#define MUSE_CHAT_SID_MAX 36\b")
        self.assertRegex(settings, r"#define MUSE_CHAT_NAME_MAX 32\b")
        self.assertRegex(settings, r"#define MUSE_CHATS_MAX 8\b")

    def test_gadget_chat_id_is_a_uuid(self):
        # The harness's GADGET_SID has the firmware's prefix: "musegadg" as a
        # v4 UUID, then the MAC.
        settings = (ROOT / "components/muse/muse_settings.c").read_text()
        self.assertIn('"6d757365-6761-4467-8000-%02x%02x%02x%02x%02x%02x"', settings)
        harness = (ROOT / "tests/link_gadget_commands_harness.c").read_text()
        self.assertIn('#define GADGET_SID "6d757365-6761-4467-8000-', harness)

    def test_mode_names_match_the_firmware(self):
        # The harness fakes muse_gadget_mode_parse with these names.
        mode = (ROOT / "components/muse/muse_gadget_mode.c").read_text()
        keys = mode[mode.index("KEYS[MUSE_GADGET_MODE_COUNT]"):]
        keys = keys[:keys.index("};")]
        self.assertEqual(re.findall(r'"([a-z_]+)"', keys), ["desk", "night", "on_the_go"])


if __name__ == "__main__":
    unittest.main()
