# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Exercise production PSRAM reply handlers with host-side event sinks, and the
gadget mode a message carries (muse_gadget_mode.c's contracts, appended by
send_chat for a chat that last heard another mode, told once the Muse acks),
and the background requests (bg_t): their own streams, their reply kept and
never emitted, waiting out a turn, and a new chat's 404 subscription."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
JSON = Path(os.environ.get(
    'CJSON_SOURCE_DIR', ROOT / 'managed_components/espressif__cjson/cJSON'
))


class ChatSession(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        out = Path(cls.tmp.name)
        source = (ROOT / 'components/muse/muse_chat_session.cpp').read_text()
        constants = source[source.index('#define MIC_RATE'):source.index('/* ---- Voice task')]
        kinds = source[source.index('enum kind_t'):source.index('#define MAX_STREAMS')]
        types = source[source.index('enum phase_t'):source.index('/* 10 KB')]
        handlers = source[source.index('static int find_msg('):source.index('/* ---- Turn: speech')]
        background = source[source.index('/* ---- Background requests'):source.index('/* ---- Inbound dispatch')]
        reset = source[source.index('static bool turn_start('):source.index('/* A dictated turn (DICTATE_EVERY_TURN)')]
        message_to = source[source.index('static void message_to('):source.index('/* Base64-encodes the staged PCM')]
        send_chat = source[source.index('static void send_chat('):source.index('static void post_chat(')]
        mode_source = (ROOT / 'components/muse/muse_gadget_mode.c').read_text()
        contexts = mode_source[mode_source.index('static const char *const NAMES['):
                               mode_source.index('/* ---- The schedule ---- */')]
        # The mode side in C (designated initializers), with what chats were told kept in a table.
        mode_code = r'''
#include <stdio.h>
#include <string.h>
#include "host_compat.h"
typedef enum { MUSE_GADGET_DESK, MUSE_GADGET_NIGHT, MUSE_GADGET_ON_THE_GO, MUSE_GADGET_MODE_COUNT } muse_gadget_mode_t;
#define ESP_LOGI(tag, fmt, ...) fprintf(stderr, "%s: " fmt "\n", tag, ##__VA_ARGS__)
static const char *TAG = "muse_mode";
static int s_mode;
static struct { char sid[40]; int mode; } s_told[8];
static int s_told_n;
muse_gadget_mode_t muse_gadget_mode(void) { return (muse_gadget_mode_t)s_mode; }
int muse_settings_chat_told(const char *sid) {
    for (int i = 0; i < s_told_n; i++) if (!strcmp(s_told[i].sid, sid)) return s_told[i].mode;
    return -1;
}
void muse_settings_chat_set_told(const char *sid, int mode) {
    int i = 0;
    while (i < s_told_n && strcmp(s_told[i].sid, sid)) i++;
    if (i == s_told_n) strlcpy(s_told[s_told_n++].sid, sid, sizeof(s_told[0].sid));
    s_told[i].mode = mode;
}
const char *muse_gadget_mode_context(const char *sid, int *mode);
void muse_gadget_mode_told(const char *sid, int mode);
''' + contexts + r'''
const char *muse_gadget_mode_name(muse_gadget_mode_t mode) { return NAMES[mode]; }
const char *muse_gadget_mode_key(muse_gadget_mode_t mode) { return KEYS[mode]; }
void test_set_mode(int mode) { s_mode = mode; }
'''
        (out / 'mode.c').write_text(mode_code)
        code = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include "host_compat.h"
#include "cJSON.h"
#include "minimp3.h"
#include "muse_chat_priv.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(tag, ...) ((void)snprintf(nullptr, 0, __VA_ARGS__))
#define EXT_RAM_BSS_ATTR
#define MUSE_CHAT_SID_MAX 36
typedef enum { MUSE_GADGET_DESK, MUSE_GADGET_NIGHT, MUSE_GADGET_ON_THE_GO, MUSE_GADGET_MODE_COUNT } muse_gadget_mode_t;
extern "C" {
const char *muse_gadget_mode_context(const char *sid, int *mode);
void muse_gadget_mode_told(const char *sid, int mode);
const char *muse_gadget_mode_name(muse_gadget_mode_t mode);
int muse_settings_chat_told(const char *sid);
void test_set_mode(int mode);
}
''' + constants + kinds + types + r'''
static turn_t s_turn;
static char s_reply_shown[EV_TEXT];
static int64_t s_last_seq, s_marks[4];
static int captions, console_events, sent_events;
enum mark_t { M_TEXT, M_DONE, M_ACK };
static void mark(mark_t) {}
static void muse_gadget_mode_retitle(const char *) {}
static char s_voice_new_sid[MUSE_CHAT_SID_MAX + 1];
static bool muse_settings_chat_retitle(const char *, const char *, bool *) { return false; }
static void muse_settings_chat_set_titling(const char *) {}
static void muse_settings_chat_titled(const char *) {}
static int64_t now_us() { return 12345; }
static char chat_sid[MUSE_CHAT_SID_MAX + 1], posted[4096];
static void muse_settings_chat_sid(char out[MUSE_CHAT_SID_MAX + 1]) { strlcpy(out, chat_sid, MUSE_CHAT_SID_MAX + 1); }
static void *psram_alloc(size_t n) { return malloc(n); }
static void heap_caps_free(void *p) { free(p); }
static void free_rec() {}
/* The streams a background request opens, as the session would keep them. */
static stream_t streams[4];
static int64_t next_id = 100;
static char bg_sub_body[256], bg_chat_body[1024];
static int bg_subs, bg_posts, resets, disconnects;
static int64_t open_stream(kind_t kind, const char *, const char *path, const char *, const char *, const char *body,
                           bool end_body) {
    assert(body && end_body);
    if (kind == K_CHAT) {
        strlcpy(posted, body, sizeof(posted));
        return 7;
    }
    assert(kind == K_BG_SUB ? !strcmp(path, "/chat/subscribe") : kind == K_BG_CHAT && !strcmp(path, "/chat/stream"));
    strlcpy(kind == K_BG_SUB ? bg_sub_body : bg_chat_body, body, kind == K_BG_SUB ? sizeof(bg_sub_body) : sizeof(bg_chat_body));
    (kind == K_BG_SUB ? bg_subs : bg_posts)++;
    for (auto &s : streams) {
        if (s.kind == K_NONE) {
            s = stream_t{};
            s.id = next_id++;
            s.kind = kind;
            return s.id;
        }
    }
    abort();
}
static stream_t *find_stream(int64_t id) {
    for (auto &s : streams) if (s.kind != K_NONE && s.id == id) return &s;
    return nullptr;
}
static void close_stream(stream_t *s) { if (s) s->kind = K_NONE; }
static bool send_reset(int64_t id) { resets++; close_stream(find_stream(id)); return true; }
static bool send_body(int64_t, const uint8_t *, size_t, bool) { return false; }
static void emit(muse_hatch_ev_t type, const char *) {
    if (type == MUSE_HATCH_EV_REPLY) captions++;
    if (type == MUSE_HATCH_EV_SENT) sent_events++;
}
void muse_hatch_console(const char *, const char *, const char *, ...) { console_events++; }
void muse_hatch_tail_words(const char *text, char *out, size_t cap) { strlcpy(out, text, cap); }
bool muse_hatch_caption_at(const char *text, size_t, char *out, size_t cap) {
    strlcpy(out, text, cap); return text[0];
}
static void turn_finish() { s_turn.phase = P_IDLE; }
static void turn_fail(const char *) { turn_finish(); }
static bool ensure_connected() { return true; }
static bool subscription_stale() { return false; }
static void disconnect(const char *) { disconnects++; }
bool muse_hatch_configured() { return true; }
static void resampler_init(resampler_t *, int, int) {}
''' + reset + message_to + send_chat + handlers + background + r'''
static void begin(bool typed = false) {
    assert(turn_start(s_turn.gen + 1, typed));
    s_turn.phase = P_WAIT_REPLY;
    s_turn.acked = true;
    strlcpy(s_turn.user_ids[0], "note", sizeof(s_turn.user_ids[0]));
    strlcpy(s_turn.user_ids[1], "parent", sizeof(s_turn.user_ids[1]));
    captions = console_events = 0;
}
static void event(const char *kind, const char *id, const char *parent = "", const char *text = "") {
    cJSON *root = cJSON_CreateObject(), *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "event");
    cJSON_AddStringToObject(root, "event", kind);
    cJSON_AddItemToObject(root, "payload", payload);
    cJSON_AddStringToObject(payload, "message_id", id);
    if (parent[0]) cJSON_AddStringToObject(payload, "reply_to_message_id", parent);
    cJSON_AddStringToObject(payload, "text", text);
    cJSON_AddStringToObject(payload, "display_text", text);
    on_event(root);
    cJSON_Delete(root);
}
static void rejected_deltas() {
    for (bool typed : {false, true}) {
        begin(typed);
        event("delta.message_start", "other", "elsewhere");
        event("delta.text_append", "other", "", "Wrong reply");
        event("delta.message_done", "other");
        event("message.assistant", "other", "", "Wrong final");
        assert(!s_turn.nmsgs && !captions && !console_events);
        assert(!s_turn.last_content_us && !s_turn.last_event_us);
        event("delta.message_start", "reply", "note");
        event("delta.text_append", "reply", "", "Our reply");
        event("delta.message_done", "reply");
        assert(s_turn.nmsgs == 1 && s_turn.msgs[0].done && s_turn.msgs[0].len == 9);
        assert(typed ? console_events == 2 : captions == 1);
        begin(typed);
        event("message.assistant", "other", "", "New turn");
        assert(s_turn.nmsgs == 1 && s_turn.msgs[0].done);
    }
}
static void valid_parents() {
    begin();
    event("message.assistant", "first", "note", "First");
    event("message.assistant", "second", "parent", "Second");
    event("message.assistant", "third", "first", "Third");
    event("message.assistant", "fourth", "", "Live");
    assert(s_turn.nmsgs == 4);
    for (int i = 0; i < s_turn.nmsgs; i++) assert(s_turn.msgs[i].done);
    cJSON *payload = cJSON_Parse("{\"parent_message_id\":\"elsewhere\"}");
    assert(bind_msg("fallback", payload) == -1);
    cJSON_Delete(payload);
    event("message.assistant", "fallback", "", "Wrong final");
    assert(s_turn.nmsgs == 4);
}
static void bounded_rejections() {
    begin();
    event("delta.message_start", "reply", "note");
    char id[20];
    for (int i = 0; i < 9; i++) { /* exceed the eight rejected IDs retained per turn */
        snprintf(id, sizeof(id), "other%d", i);
        event("delta.message_start", id, "elsewhere");
    }
    event("message.assistant", "other0", "", "Wrong final");
    event("message.assistant", id, "", "Overflow final");
    assert(s_turn.nmsgs == 1 && !captions && !console_events);
    event("delta.text_append", "reply", "", "Our reply");
    event("delta.message_done", "reply");
    event("message.assistant", "second", "note", "Second");
    assert(s_turn.nmsgs == 2 && s_turn.msgs[0].done && s_turn.msgs[1].done);
}
/* A typed message to the chat picked now: the "message" it posts. */
static const char *send(const char *text) {
    assert(turn_start(s_turn.gen + 1, true));
    send_chat(text, "text");
    assert(s_turn.phase == P_WAIT_REPLY);
    static char message[4096];
    cJSON *body = cJSON_Parse(posted);
    assert(body);
    strlcpy(message, cJSON_GetStringValue(cJSON_GetObjectItem(body, "message")), sizeof(message));
    cJSON *sid = cJSON_GetObjectItem(body, "session_id");
    assert(chat_sid[0] ? !strcmp(cJSON_GetStringValue(sid), chat_sid) : !sid);
    cJSON_Delete(body);
    return message;
}
static void ack() {
    char line[64] = "{\"message_id\":\"mine\"}";
    stream_t s = {};
    s.line = line;
    s.len = strlen(line);
    s.cap = sizeof(line);
    on_chat_ack(&s);
}
static bool has_mode(const char *message, const char *words, const char *mode) {
    char want[256];
    snprintf(want, sizeof(want), "%s\n\n[gadget mode: %s] %s mode is on.", words, mode, mode);
    return !strncmp(message, want, strlen(want));
}
static void inline_mode() {
    test_set_mode(MUSE_GADGET_DESK);
    /* Never told: the contract follows the words, which come first for the title. */
    const char *m = send("What's a good hangboard warm-up?");
    assert(has_mode(m, "What's a good hangboard warm-up?", "DESK"));
    assert(strstr(m, "full detail is fine.") && s_turn.tells == MUSE_GADGET_DESK);
    assert(muse_settings_chat_told("") == -1);   /* not until the Muse takes it */
    ack();
    assert(muse_settings_chat_told("") == MUSE_GADGET_DESK && sent_events == 1);
    /* Told: just the words. */
    m = send("And after bouldering?");
    assert(!strcmp(m, "And after bouldering?") && s_turn.tells == -1);
    ack();
    /* A change sends nothing by itself; the next message carries it, once. */
    test_set_mode(MUSE_GADGET_ON_THE_GO);
    m = send("Where's the gym?");
    assert(has_mode(m, "Where's the gym?", "ON-THE-GO") && strstr(m, "two sentences max"));
    ack();
    assert(!strcmp(send("Thanks"), "Thanks"));
    /* Each chat has its own: a side chat hasn't heard it yet. */
    strcpy(chat_sid, "7d3f2a10-5b6c-4e8d-9a1f-288485906f44");
    m = send("Plan my week");
    assert(has_mode(m, "Plan my week", "ON-THE-GO"));
    /* No ack (refused, or the connection dropped): the next message tells it again. */
    m = send("Plan my week");
    assert(has_mode(m, "Plan my week", "ON-THE-GO"));
    ack();
    assert(muse_settings_chat_told(chat_sid) == MUSE_GADGET_ON_THE_GO);
    assert(!strcmp(send("Plan my week"), "Plan my week"));
    /* Back in the main chat, which heard the same. */
    chat_sid[0] = '\0';
    test_set_mode(MUSE_GADGET_NIGHT);
    m = send("Remind me tomorrow");
    assert(has_mode(m, "Remind me tomorrow", "NIGHT") && strstr(m, "Do not speak replies aloud."));
    test_set_mode(MUSE_GADGET_ON_THE_GO);   /* changed back before the ack: the ack tells what was sent */
    ack();
    assert(muse_settings_chat_told("") == MUSE_GADGET_NIGHT);
    assert(has_mode(send("Ok"), "Ok", "ON-THE-GO"));
}
/* ---- Background requests ---- */
#define BG_SID "6d757365-7570-4e78-8000-a1b2c3d4e5f6"
static void bg_event(const char *kind, const char *id, const char *parent = "", const char *text = "",
                     const char *display = nullptr) {
    cJSON *root = cJSON_CreateObject(), *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "event");
    cJSON_AddStringToObject(root, "event", kind);
    cJSON_AddItemToObject(root, "payload", payload);
    cJSON_AddStringToObject(payload, "message_id", id);
    if (parent[0]) cJSON_AddStringToObject(payload, "reply_to_message_id", parent);
    cJSON_AddStringToObject(payload, "text", text);
    if (display) cJSON_AddStringToObject(payload, "display_text", display);
    on_bg_event(root);
    cJSON_Delete(root);
}
static stream_t *bg_stream(kind_t kind) {
    for (auto &s : streams) if (s.kind == kind) return &s;
    return nullptr;
}
static void bg_ack(bool ok, const char *line = "{\"message_id\":\"asked\"}") {
    stream_t *s = bg_stream(K_BG_CHAT);
    assert(s);
    static char buf[128];
    strlcpy(buf, line, sizeof(buf));
    s->line = buf;
    s->len = strlen(buf);
    s->cap = sizeof(buf);
    bg_chat_end(s, ok);
}
/* Asks, and gets as far as the message posted on a subscription that was taken. */
static void bg_ask_posted() {
    bg_want(BG_SID "\nWhat's next?");
    assert(s_bg.phase == BG_WANTED && s_bg_state == MUSE_CHAT_BG_NONE);
    bg_poll();
    assert(s_bg.phase == BG_SUBSCRIBING && !strcmp(bg_sub_body, "{\"session_id\":\"" BG_SID "\"}"));
    bg_poll();
    assert(s_bg.phase == BG_SUBSCRIBING);   /* not taken yet */
    bg_stream(K_BG_SUB)->status = 200;
    bg_poll();
    assert(s_bg.phase == BG_WAITING);
    cJSON *body = cJSON_Parse(bg_chat_body);
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(body, "message")), "What's next?"));
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(body, "output_modality")), "text"));
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(body, "session_id")), BG_SID));
    cJSON_Delete(body);
}
static void quiet() {
    assert(!captions && !console_events && !sent_events && !disconnects);
    assert(s_turn.nmsgs == 0 && s_turn.phase == P_IDLE && !s_turn.last_event_us);
}
static void background() {
    begin();
    s_turn.phase = P_IDLE;
    sent_events = 0;
    /* A turn under way: it waits. */
    s_turn.phase = P_WAIT_REPLY;
    bg_want(BG_SID "\nWhat's next?");
    bg_poll();
    assert(s_bg.phase == BG_WANTED && !bg_subs);
    s_turn.phase = P_IDLE;
    bg_end(false, "test");   /* start over below */
    s_bg_state = MUSE_CHAT_BG_NONE;

    bg_ask_posted();
    bg_event("delta.message_start", "early");   /* before the ack: no parent to check yet */
    assert(!strcmp(s_bg.reply_id, "early"));
    bg_end(false, "test");
    s_bg_state = MUSE_CHAT_BG_NONE;
    bg_ask_posted();
    bg_ack(true);
    assert(!strcmp(s_bg.user_id, "asked") && !bg_stream(K_BG_CHAT));
    resets = 0;
    bg_event("delta.text_append", "asked", "", "my own words");       /* our message, echoed */
    bg_event("delta.text_append", "other", "elsewhere", "Not ours");  /* answers something else */
    bg_event("delta.message_start", "reply", "asked");
    bg_event("delta.text_append", "reply", "", "Standup ");
    bg_event("delta.text_append", "second", "asked", "A second message");   /* only the first counts */
    bg_event("delta.text_append", "reply", "", "at 10");
    assert(!strcmp(s_bg.text, "Standup at 10") && s_bg_state == MUSE_CHAT_BG_NONE);
    bg_event("delta.message_done", "reply");
    assert(s_bg.phase == BG_IDLE && s_bg_state == MUSE_CHAT_BG_DONE && !strcmp(s_bg.text, "Standup at 10"));
    assert(!bg_stream(K_BG_SUB) && resets == 1);   /* its subscription closed; the turns' streams untouched */
    quiet();
    s_bg_state = MUSE_CHAT_BG_NONE;

    /* A chat the Muse hasn't seen: 404, so the message goes first, and the subscription after its ack. */
    bg_subs = bg_posts = 0;
    bg_want(BG_SID "\nWhat's next?");
    bg_poll();
    bg_sub_missing(bg_stream(K_BG_SUB));
    assert(bg_posts == 1 && s_bg.phase == BG_WAITING && s_bg.sub_missing && !bg_stream(K_BG_SUB));
    bg_ack(true);
    assert(bg_subs == 2 && bg_stream(K_BG_SUB) && !s_bg.sub_missing);
    bg_event("message.assistant", "reply2", "asked", "", "Gym at 6, pack shoes");
    assert(s_bg_state == MUSE_CHAT_BG_DONE && !strcmp(s_bg.text, "Gym at 6, pack shoes"));
    quiet();
    s_bg_state = MUSE_CHAT_BG_NONE;

    /* Refused by the Muse, or the connection going: it just fails. */
    bg_ask_posted();
    bg_ack(false);
    assert(s_bg.phase == BG_IDLE && s_bg_state == MUSE_CHAT_BG_FAILED && !bg_stream(K_BG_SUB));
    s_bg_state = MUSE_CHAT_BG_NONE;
    bg_ask_posted();
    for (auto &s : streams) s.kind = K_NONE;   /* as disconnect() leaves them */
    bg_dropped();
    assert(s_bg.phase == BG_IDLE && s_bg_state == MUSE_CHAT_BG_FAILED);
    s_bg_state = MUSE_CHAT_BG_NONE;
    /* An empty reply is no answer. */
    bg_ask_posted();
    bg_ack(true);
    bg_event("delta.message_done", "reply3", "asked");
    assert(s_bg_state == MUSE_CHAT_BG_FAILED);
    /* Malformed requests are refused. */
    s_bg_state = MUSE_CHAT_BG_NONE;
    bg_want("not-a-uuid\nhi");
    assert(s_bg.phase == BG_IDLE && s_bg_state == MUSE_CHAT_BG_FAILED);
    quiet();
}
int main(int argc, char **argv) {
    assert(argc == 2);
    switch (atoi(argv[1])) {
    case 0: rejected_deltas(); break;
    case 1: valid_parents(); break;
    case 2: bounded_rejections(); break;
    case 3: inline_mode(); break;
    case 4: background(); break;
    default: return 2;
    }
}
'''
        (out / 'session.cpp').write_text(code)
        flags = ['-Wall', '-Wextra', '-Werror', '-I', str(JSON),
                 '-I', str(ROOT / 'tests'), '-I', str(ROOT / 'components/muse'),
                 '-I', str(ROOT / 'components/minimp3/include')]
        commands = [
            [*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', *flags,
             '-c', str(JSON / 'cJSON.c'), '-o', str(out / 'cjson.o')],
            [*shlex.split(os.environ.get('CC', 'cc')), '-std=gnu11', *flags,
             '-c', str(out / 'mode.c'), '-o', str(out / 'mode.o')],
            [*shlex.split(os.environ.get('CXX', 'c++')), '-std=gnu++17', *flags,
             str(out / 'session.cpp'), str(out / 'cjson.o'), str(out / 'mode.o'), '-o', str(out / 'session')],
        ]
        for command in commands:
            result = subprocess.run(command, capture_output=True, text=True)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)
        cls.binary = out / 'session'

    def run_case(self, case):
        result = subprocess.run([str(self.binary), str(case)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_rejected_deltas_do_not_emit_or_complete_voice_or_typed_replies(self):
        self.run_case(0)

    def test_ack_parent_reply_chain_and_parentless_messages_still_work(self):
        self.run_case(1)

    def test_rejection_capacity_preserves_correlation(self):
        self.run_case(2)

    def test_mode_follows_the_words_until_the_chat_has_it(self):
        self.run_case(3)

    def test_background_request_keeps_its_reply_and_never_emits(self):
        self.run_case(4)
