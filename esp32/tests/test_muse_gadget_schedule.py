# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""The gadget mode's schedule (muse_gadget_mode.c): which mode a local time
falls in for a Night window set to the minute, across midnight or not, and
the next boundary a mode picked by hand holds until, through month ends and a
daylight saving change."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class GadgetSchedule(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        out = Path(cls.tmp.name)
        source = (ROOT / 'components/muse/muse_gadget_mode.c').read_text()
        modes = source[source.index('/* Whether minute-of-day m falls'):source.index('/* On the On-the-go network')]
        boundary = source[source.index("/* The schedule's next switch"):source.index('void muse_gadget_mode_night(')]
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef enum { MUSE_GADGET_DESK, MUSE_GADGET_NIGHT, MUSE_GADGET_ON_THE_GO } muse_gadget_mode_t;
static int s_from = 21 * 60, s_to = 5 * 60;
static void muse_settings_night(int *from, int *to) { *from = s_from; *to = s_to; }
''' + modes + boundary + r'''
static time_t local(int y, int mo, int d, int h, int mi, int s) {
    struct tm tm = { .tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d, .tm_hour = h, .tm_min = mi,
                     .tm_sec = s, .tm_isdst = -1 };
    return mktime(&tm);
}
static muse_gadget_mode_t at(int h, int mi) {
    struct tm tm = { .tm_hour = h, .tm_min = mi };
    return scheduled(&tm);
}
static void window(int from, int to) { s_from = from; s_to = to; }
static void expect_next(time_t from, time_t want) {
    uint32_t got = next_boundary(from);
    if (got != (uint32_t)want) {
        fprintf(stderr, "next boundary from %lld: got %lu, want %lld\n", (long long)from, (unsigned long)got,
                (long long)want);
        abort();
    }
}
int main(int argc, char **argv) {
    assert(argc == 2);
    setenv("TZ", "PST8PDT,M3.2.0,M11.1.0", 1);
    tzset();
    switch (atoi(argv[1])) {
    case 0:   /* the default, across midnight */
        assert(at(20, 59) == MUSE_GADGET_DESK && at(21, 0) == MUSE_GADGET_NIGHT);
        assert(at(0, 0) == MUSE_GADGET_NIGHT && at(4, 59) == MUSE_GADGET_NIGHT && at(5, 0) == MUSE_GADGET_DESK);
        window(22 * 60 + 35, 6 * 60 + 10);   /* to the minute */
        assert(at(22, 34) == MUSE_GADGET_DESK && at(22, 35) == MUSE_GADGET_NIGHT);
        assert(at(6, 9) == MUSE_GADGET_NIGHT && at(6, 10) == MUSE_GADGET_DESK);
        window(13 * 60 + 30, 14 * 60 + 45);   /* a nap, not crossing midnight */
        assert(at(13, 29) == MUSE_GADGET_DESK && at(13, 30) == MUSE_GADGET_NIGHT);
        assert(at(14, 44) == MUSE_GADGET_NIGHT && at(14, 45) == MUSE_GADGET_DESK && at(2, 0) == MUSE_GADGET_DESK);
        window(6 * 60, 6 * 60);   /* empty: never Night */
        assert(at(6, 0) == MUSE_GADGET_DESK && at(23, 0) == MUSE_GADGET_DESK);
        break;
    case 1:
        expect_next(local(2026, 10, 7, 12, 0, 0), local(2026, 10, 7, 21, 0, 0));
        expect_next(local(2026, 10, 7, 20, 59, 30), local(2026, 10, 7, 21, 0, 0));
        expect_next(local(2026, 10, 7, 21, 0, 30), local(2026, 10, 8, 5, 0, 0));   /* this minute's has passed */
        expect_next(local(2026, 10, 8, 3, 0, 0), local(2026, 10, 8, 5, 0, 0));
        expect_next(local(2026, 10, 31, 22, 0, 0), local(2026, 11, 1, 5, 0, 0));   /* month end, DST ends at 2:00 */
        assert(local(2026, 11, 1, 5, 0, 0) - local(2026, 10, 31, 22, 0, 0) == 8 * 3600);
        window(13 * 60 + 30, 14 * 60 + 45);
        expect_next(local(2026, 10, 7, 14, 0, 0), local(2026, 10, 7, 14, 45, 0));
        expect_next(local(2026, 12, 31, 15, 0, 0), local(2027, 1, 1, 13, 30, 0));   /* year end */
        window(6 * 60, 6 * 60);
        expect_next(local(2026, 10, 7, 7, 0, 0), local(2026, 10, 8, 6, 0, 0));
        break;
    default:
        return 2;
    }
    return 0;
}
'''
        (out / 'schedule.c').write_text(code)
        cc = shlex.split(os.environ.get('CC', 'cc'))
        result = subprocess.run([*cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                                 str(out / 'schedule.c'), '-o', str(out / 'schedule')],
                                capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        cls.binary = out / 'schedule'

    def run_case(self, case):
        result = subprocess.run([str(self.binary), str(case)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_night_window_to_the_minute_across_midnight_or_not(self):
        self.run_case(0)

    def test_next_boundary_through_month_year_and_dst(self):
        self.run_case(1)


if __name__ == '__main__':
    unittest.main()
