/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * The passcode's rules (muse_lock_policy.c) and, built with tests/lock_fakes'
 * PSA on OpenSSL, its hash (muse_lock_hash.c). "policy" checks the rules and
 * prints PASS; "hash PIN SALTHEX ROUNDS" prints the hash's hex, for the test
 * to hold against Python's own PBKDF2; "hashcheck" checks a record made and
 * checked, and the rounds' sizing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "muse_lock_policy.h"
#ifdef LOCK_HASH
#include "muse_lock_hash.h"
#endif

static int s_fails;

#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);      \
            s_fails++;                                                   \
        }                                                                \
    } while (0)

/* What muse_lock.c does with an attempt: counted wrong first, then put
 * right if it was. */
static muse_lock_result_t attempt(muse_lock_policy_t *p, bool right)
{
    if (!muse_lock_policy_can_try(p)) {
        return MUSE_LOCK_REFUSED;
    }
    muse_lock_result_t r = muse_lock_policy_wrong(p);
    if (right) {
        muse_lock_policy_reset(p);
        return MUSE_LOCK_RIGHT;
    }
    return r;
}

static void rounds(void)
{
    muse_lock_policy_t p;
    muse_lock_policy_reset(&p);
    CHECK(muse_lock_policy_can_try(&p));
    CHECK(muse_lock_policy_left(&p) == 3);
    CHECK(!muse_lock_policy_final(&p));
    CHECK(attempt(&p, false) == MUSE_LOCK_WRONG && muse_lock_policy_left(&p) == 2);
    CHECK(attempt(&p, false) == MUSE_LOCK_WRONG && muse_lock_policy_left(&p) == 1);
    /* The third: an hour's lockout, and the final round after it. */
    CHECK(attempt(&p, false) == MUSE_LOCK_LOCKOUT);
    CHECK(p.lockout_s == 3600 && p.round == 1 && p.failed == 0);
    CHECK(muse_lock_policy_final(&p));
    CHECK(!muse_lock_policy_can_try(&p));
    /* Locked out: not tried, not counted, even the right one. */
    CHECK(attempt(&p, true) == MUSE_LOCK_REFUSED);
    CHECK(attempt(&p, false) == MUSE_LOCK_REFUSED);
    CHECK(p.failed == 0 && p.lockout_s == 3600);
    CHECK(!muse_lock_policy_elapse(&p, 3599));
    CHECK(p.lockout_s == 1 && !muse_lock_policy_can_try(&p));
    CHECK(muse_lock_policy_elapse(&p, 5));
    CHECK(p.lockout_s == 0 && muse_lock_policy_can_try(&p));
    CHECK(!muse_lock_policy_elapse(&p, 5));   /* nothing left to end */
    /* The final round: three more wrong erase it. */
    CHECK(attempt(&p, false) == MUSE_LOCK_WRONG && muse_lock_policy_left(&p) == 2);
    CHECK(attempt(&p, false) == MUSE_LOCK_WRONG && muse_lock_policy_left(&p) == 1);
    CHECK(!muse_lock_policy_wipe_due(&p));
    CHECK(attempt(&p, false) == MUSE_LOCK_WIPE);
    CHECK(muse_lock_policy_wipe_due(&p) && !muse_lock_policy_can_try(&p));
    CHECK(attempt(&p, true) == MUSE_LOCK_REFUSED);   /* too late */
    CHECK(muse_lock_policy_wipe_due(&p));
}

static void right_starts_over(void)
{
    muse_lock_policy_t p;
    muse_lock_policy_reset(&p);
    attempt(&p, false);
    attempt(&p, false);
    CHECK(attempt(&p, true) == MUSE_LOCK_RIGHT);
    CHECK(p.failed == 0 && p.round == 0 && p.lockout_s == 0);
    /* In the final round too, with two used. */
    for (int i = 0; i < 3; i++) {
        attempt(&p, false);
    }
    muse_lock_policy_elapse(&p, 3600);
    attempt(&p, false);
    attempt(&p, false);
    CHECK(p.round == 1 && p.failed == 2);
    CHECK(attempt(&p, true) == MUSE_LOCK_RIGHT);
    CHECK(p.failed == 0 && p.round == 0 && !muse_lock_policy_final(&p));
    /* The right one on the last try before a lockout: no lockout. */
    attempt(&p, false);
    attempt(&p, false);
    CHECK(attempt(&p, true) == MUSE_LOCK_RIGHT && p.lockout_s == 0);
}

/* What's saved goes back in as it was, or kept to the stricter. */
static void storage(void)
{
    muse_lock_policy_t p = { .failed = 2, .round = 0, .lockout_s = 0 };
    CHECK(muse_lock_policy_sane(&p) && p.failed == 2);
    p = (muse_lock_policy_t){ .failed = 9, .round = 7, .lockout_s = 99999 };
    CHECK(!muse_lock_policy_sane(&p));
    CHECK(p.round == 1 && p.failed == 3 && p.lockout_s == 3600);
    CHECK(muse_lock_policy_wipe_due(&p));
    /* A first round used up with no lockout (cut off between the two): the lockout. */
    p = (muse_lock_policy_t){ .failed = 3, .round = 0, .lockout_s = 0 };
    CHECK(!muse_lock_policy_sane(&p));
    CHECK(p.round == 1 && p.failed == 0 && p.lockout_s == 3600);
}

/*
 * The lockout across restarts, as muse_lock.c runs it: counted down by the
 * clock while it runs and saved every 30 s, resumed whole from what was saved.
 * A restart only ever adds to it: it never ends sooner than it would have.
 */
static void lockout_across_restarts(void)
{
    muse_lock_policy_t live, saved;
    muse_lock_policy_reset(&live);
    for (int i = 0; i < 3; i++) {
        attempt(&live, false);   /* saved before each one's result shows */
    }
    saved = live;
    uint32_t ran = 0, since_save = 0;
    const uint32_t restarts_at[] = { 45, 1000, 1001, 2999 };
    size_t next = 0;
    for (uint32_t t = 1; live.lockout_s; t++) {
        muse_lock_policy_elapse(&live, 1);
        ran++;
        if (++since_save >= 30 || !live.lockout_s) {
            saved = live;
            since_save = 0;
        }
        if (next < sizeof(restarts_at) / sizeof(restarts_at[0]) && t == restarts_at[next]) {
            next++;
            live = saved;   /* the power cut: back to what was saved */
            muse_lock_policy_sane(&live);
            since_save = 0;
        }
    }
    CHECK(ran >= 3600);
    CHECK(ran <= 3600 + 4 * 30);
    CHECK(live.round == 1 && muse_lock_policy_can_try(&live));
}

static void after_sleep(void)
{
    CHECK(MUSE_LOCK_AFTER_COUNT == 7);
    CHECK(!strcmp(MUSE_LOCK_AFTER_NAMES[MUSE_LOCK_AFTER_BATTERY_DEFAULT], "5 minutes"));
    CHECK(!strcmp(MUSE_LOCK_AFTER_NAMES[MUSE_LOCK_AFTER_CHARGER_DEFAULT], "1 hour"));
    const int bat = MUSE_LOCK_AFTER_BATTERY_DEFAULT, chg = MUSE_LOCK_AFTER_CHARGER_DEFAULT;
    /* On battery: 5 minutes. */
    CHECK(!muse_lock_policy_sleep_locks(299, true, false, bat, chg));
    CHECK(muse_lock_policy_sleep_locks(300, true, false, bat, chg));
    /* On a charger: an hour. */
    CHECK(!muse_lock_policy_sleep_locks(3599, false, true, bat, chg));
    CHECK(muse_lock_policy_sleep_locks(3600, false, true, bat, chg));
    /* Unplugged partway: the stricter, either way round. */
    CHECK(muse_lock_policy_sleep_locks(300, true, true, bat, chg));
    CHECK(muse_lock_policy_sleep_locks(300, true, true, chg, bat));
    CHECK(!muse_lock_policy_sleep_locks(299, true, true, bat, chg));
    /* Immediately, and Never. */
    CHECK(muse_lock_policy_sleep_locks(0, true, false, 0, chg));
    CHECK(!muse_lock_policy_sleep_locks(1000000, true, false, 6, chg));
    CHECK(muse_lock_policy_sleep_locks(300, true, true, 6, bat));   /* Never on battery, 5 min charging */
    CHECK(!muse_lock_policy_sleep_locks(100000, true, true, 6, 6));
    /* Not known how it slept: as on battery. An unknown choice: the battery default. */
    CHECK(muse_lock_policy_sleep_locks(300, false, false, bat, 6));
    CHECK(muse_lock_policy_after_s(99) == 300 && muse_lock_policy_after_s(-1) == 300);
    CHECK(muse_lock_policy_after_s(5) == 4 * 3600);
}

static void blocked(void)
{
    const char *remote[] = { "show_text", "set_mode", "set_chat", "list_chats", "display.show_image",
                             "display.draw_url", "bug.report" };
    for (size_t i = 0; i < sizeof(remote) / sizeof(remote[0]); i++) {
        CHECK(muse_lock_policy_blocks_remote(remote[i]));
    }
    CHECK(!muse_lock_policy_blocks_remote("device.list_vms"));
    CHECK(!muse_lock_policy_blocks_remote("device.unpair"));
    CHECK(!muse_lock_policy_blocks_remote("show_textx"));
    const char *console[] = { "chat=hi", "chat+=hi", "chat.cancel", "chats", "chat_sid", "chat_sid=x", "chat_new=",
                              "chat_forget=a", "say=hello", "widget=list", "face=happy", "activity=mail",
                              "brief", "brief?", "wifi.ssid=x", "hatch.token=x", "volume=5", "lockx", "pinx=1",
                              "statusx" };
    for (size_t i = 0; i < sizeof(console) / sizeof(console[0]); i++) {
        if (!muse_lock_policy_blocks_console(console[i])) {
            fprintf(stderr, "console \"%s\" not blocked\n", console[i]);
            s_fails++;
        }
    }
    const char *allowed[] = { "lock", "lockstate", "pin=123456", "status", "power", "power.reset", "nap", "crash",
                              "batt=50", "quake", "charge" };
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        if (muse_lock_policy_blocks_console(allowed[i])) {
            fprintf(stderr, "console \"%s\" blocked\n", allowed[i]);
            s_fails++;
        }
    }
}

#ifdef LOCK_HASH
static int unhex(const char *h, uint8_t *out, size_t cap)
{
    size_t n = strlen(h) / 2;
    for (size_t i = 0; i < n && i < cap; i++) {
        unsigned v;
        sscanf(h + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
    return (int)n;
}

static void hash_checks(void)
{
    uint8_t salt[MUSE_LOCK_SALT_LEN];
    for (int i = 0; i < MUSE_LOCK_SALT_LEN; i++) {
        salt[i] = (uint8_t)(i * 7 + 1);
    }
    muse_lock_record_t rec;
    CHECK(muse_lock_hash_make("482913", 6, salt, MUSE_LOCK_ROUNDS_MIN, &rec));
    CHECK(rec.version == MUSE_LOCK_RECORD_VERSION && rec.rounds == MUSE_LOCK_ROUNDS_MIN);
    CHECK(!memcmp(rec.salt, salt, sizeof(salt)));
    CHECK(muse_lock_hash_check(&rec, "482913", 6));
    CHECK(!muse_lock_hash_check(&rec, "482914", 6));
    CHECK(!muse_lock_hash_check(&rec, "48291", 5));
    CHECK(!muse_lock_hash_check(&rec, "", 0));
    /* The digits aren't in it. */
    CHECK(!memmem(&rec, sizeof(rec), "482913", 6));
    /* Another salt, another hash. */
    muse_lock_record_t other;
    salt[0] ^= 1;
    CHECK(muse_lock_hash_make("482913", 6, salt, MUSE_LOCK_ROUNDS_MIN, &other));
    CHECK(!muse_lock_hash_same(rec.hash, other.hash, MUSE_LOCK_HASH_LEN));
    /* A record that's not sane checks nothing. */
    muse_lock_record_t bad = rec;
    bad.rounds = 1;
    CHECK(!muse_lock_hash_check(&bad, "482913", 6));
    bad = rec;
    bad.version = 99;
    CHECK(!muse_lock_hash_check(&bad, "482913", 6));
    bad = rec;
    bad.hash[31] ^= 0x80;
    CHECK(!muse_lock_hash_check(&bad, "482913", 6));
    /* Equal, whatever byte differs. */
    uint8_t a[32] = { 0 }, b[32] = { 0 };
    CHECK(muse_lock_hash_same(a, b, 32));
    for (int i = 0; i < 32; i++) {
        b[i] = 1;
        CHECK(!muse_lock_hash_same(a, b, 32));
        b[i] = 0;
    }
    /* Sizing: 2000 rounds in 20 ms is 40000 for 400 ms; kept within bounds. */
    CHECK(muse_lock_hash_rounds_for(2000, 20000, 400) == 40000);
    CHECK(muse_lock_hash_rounds_for(2000, 2000000, 400) == MUSE_LOCK_ROUNDS_MIN);
    CHECK(muse_lock_hash_rounds_for(2000, 1, 400) == MUSE_LOCK_ROUNDS_MAX);
    CHECK(muse_lock_hash_rounds_for(2000, 0, 400) == MUSE_LOCK_ROUNDS_MAX);
}
#endif

int main(int argc, char **argv)
{
#ifdef LOCK_HASH
    if (argc == 5 && !strcmp(argv[1], "hash")) {
        uint8_t salt[MUSE_LOCK_SALT_LEN] = { 0 }, out[MUSE_LOCK_HASH_LEN];
        unhex(argv[3], salt, sizeof(salt));
        if (!muse_lock_hash_derive(argv[2], strlen(argv[2]), salt, (uint32_t)atol(argv[4]), out)) {
            return 3;
        }
        for (int i = 0; i < MUSE_LOCK_HASH_LEN; i++) {
            printf("%02x", out[i]);
        }
        printf("\n");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "hashcheck")) {
        hash_checks();
        printf(s_fails ? "FAIL muse_lock_hash\n" : "PASS muse_lock_hash\n");
        return s_fails != 0;
    }
#endif
    (void)argc;
    (void)argv;
    rounds();
    right_starts_over();
    storage();
    lockout_across_restarts();
    after_sleep();
    blocked();
    printf(s_fails ? "FAIL muse_lock_policy\n" : "PASS muse_lock_policy\n");
    return s_fails != 0;
}
