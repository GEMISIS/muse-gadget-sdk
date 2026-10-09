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

"""The passcode (muse_lock.h): its rules (muse_lock_policy.c) on the host, and
its hash (muse_lock_hash.c) on the host's PSA Crypto (pkg-config mbedcrypto)
or, without it, a PSA stand-in over OpenSSL (tests/lock_fakes), held against
Python's own PBKDF2-HMAC-SHA256. The hash's tests are skipped with neither."""

from __future__ import annotations

import hashlib
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components/muse"
ENV = {**os.environ, "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1",
       "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"}
FLAGS = ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O1", "-g", "-fsanitize=address,undefined",
         "-fno-omit-frame-pointer"]


def pkg_flags(name: str) -> list[str] | None:
    if not shutil.which("pkg-config"):
        return None
    got = subprocess.run(["pkg-config", "--cflags", "--libs", name], capture_output=True, text=True)
    return shlex.split(got.stdout) if got.returncode == 0 and got.stdout.strip() else None


def psa_flags() -> list[str] | None:
    """The real PSA Crypto if the host has it, else the stand-in on OpenSSL."""
    real = pkg_flags("mbedcrypto")
    if real:
        return real
    ssl = openssl_flags()
    if ssl:
        return ["-I", str(ROOT / "tests/lock_fakes"), str(ROOT / "tests/lock_fakes/psa_openssl.c"), *ssl]
    return None


def openssl_flags() -> list[str] | None:
    """Compile and link flags for OpenSSL's libcrypto, if it's there."""
    if shutil.which("pkg-config"):
        got = subprocess.run(["pkg-config", "--cflags", "--libs", "libcrypto"], capture_output=True, text=True)
        if got.returncode == 0 and got.stdout.strip():
            return shlex.split(got.stdout)
    for prefix in ("/opt/homebrew/opt/openssl@3", "/usr/local/opt/openssl@3", "/usr"):
        if Path(prefix, "include/openssl/evp.h").exists():
            return [f"-I{prefix}/include", f"-L{prefix}/lib", "-lcrypto"]
    return None


def compile_harness(out: Path, extra: list[str]) -> None:
    cc = shlex.split(os.environ.get("CC", "cc"))
    built = subprocess.run([*cc, *FLAGS, "-I", str(MUSE), *extra, "-o", str(out)], capture_output=True, text=True)
    if built.returncode:
        raise AssertionError(built.stdout + built.stderr)


class MuseLockPolicyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / "muse_lock"
        compile_harness(cls.binary, [str(ROOT / "tests/muse_lock_harness.c"), str(MUSE / "muse_lock_policy.c")])

    def test_rules(self):
        ran = subprocess.run([str(self.binary)], capture_output=True, text=True, env=ENV)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        self.assertEqual(ran.stdout.strip(), "PASS muse_lock_policy", ran.stdout + ran.stderr)
        self.assertEqual(ran.stderr, "")

    def test_no_device_in_the_rules(self):
        """The rules stay plain C: nothing of ESP-IDF, so they run here."""
        text = (MUSE / "muse_lock_policy.c").read_text() + (MUSE / "muse_lock_policy.h").read_text()
        for word in ("esp_", "nvs_", "freertos", "lvgl"):
            self.assertNotIn(word, text)


@unittest.skipUnless(psa_flags(), "no PSA Crypto, nor OpenSSL for the stand-in")
class MuseLockHashTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / "muse_lock_hash"
        compile_harness(cls.binary, ["-DLOCK_HASH", "-Wno-deprecated-declarations",
                                     str(ROOT / "tests/muse_lock_harness.c"), str(MUSE / "muse_lock_policy.c"),
                                     str(MUSE / "muse_lock_hash.c"), *psa_flags()])

    def derive(self, pin: str, salt: bytes, rounds: int) -> str:
        ran = subprocess.run([str(self.binary), "hash", pin, salt.hex(), str(rounds)], capture_output=True,
                             text=True, env=ENV)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        return ran.stdout.strip()

    def test_pbkdf2_sha256(self):
        for pin, salt, rounds in (("123456", bytes(range(16)), 10000),
                                  ("000000", b"\xff" * 16, 12345),
                                  ("482913", bytes.fromhex("8e4a1f0c9b7d2e63a5f01c47d9b38e21"), 1)):
            want = hashlib.pbkdf2_hmac("sha256", pin.encode(), salt, rounds, 32).hex()
            self.assertEqual(self.derive(pin, salt, rounds), want)

    def test_records(self):
        ran = subprocess.run([str(self.binary), "hashcheck"], capture_output=True, text=True, env=ENV)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        self.assertEqual(ran.stdout.strip(), "PASS muse_lock_hash", ran.stdout + ran.stderr)


class MuseLockContractTest(unittest.TestCase):
    """What the firmware must keep doing around the passcode, read from its source."""

    def read(self, rel: str) -> str:
        return (ROOT / rel).read_text()

    def test_attempt_saved_before_checked(self):
        src = self.read("components/muse/muse_lock.c")
        body = src[src.index("muse_lock_result_t muse_lock_try("):]
        body = body[:body.index("\n}\n")]
        self.assertLess(body.index("muse_lock_policy_wrong(&s_policy)"), body.index("save_state()"))
        self.assertLess(body.index("save_state()"), body.index("muse_lock_hash_check("))

    def test_right_one_saves_its_reset(self):
        # The attempt was saved as wrong before the check, so the right one's
        # reset must be saved too, whatever came before: else a stale miss
        # comes back after a restart.
        src = self.read("components/muse/muse_lock.c")
        body = src[src.index("muse_lock_result_t muse_lock_try("):]
        body = body[:body.index("\n}\n")]
        right = body[body.index("if (right) {"):]
        right = right[:right.index("return MUSE_LOCK_RIGHT;")]
        self.assertLess(right.index("muse_lock_policy_reset(&s_policy);"), right.index("save_state();"))
        self.assertNotIn("if (before", right)

    def test_no_pin_kept(self):
        src = self.read("components/muse/muse_lock.c")
        self.assertNotIn("KEY_PIN, pin", src)
        self.assertIn("nvs_set_blob(s_nvs, KEY_PIN, &rec, sizeof(rec))", src)

    def test_console_has_no_bypass(self):
        src = self.read("components/muse/muse_input.c")
        self.assertIn('muse_lock_bench_pin(line + 4)', src)
        self.assertNotIn("muse_lock_clear_pin", src)
        self.assertNotIn("unlock", src.lower().replace("unlocked", "").replace("unlocking", ""))

    def test_remote_and_ble_refused_locked(self):
        self.assertIn("muse_lock_policy_blocks_remote(command)", self.read("main/app.c"))
        self.assertIn('"error_locked"', self.read("main/ble_server.c"))
        self.assertIn('"error: locked"', self.read("components/muse/muse_ble.c"))

    def test_no_new_internal_ram(self):
        for rel in ("components/muse/muse_lock.c", "components/muse/muse_lock_ui.c"):
            for line in self.read(rel).splitlines():
                if line.startswith("static ") and "(" not in line and "const" not in line:
                    self.fail(f"{rel}: internal RAM: {line}")


if __name__ == "__main__":
    unittest.main()
