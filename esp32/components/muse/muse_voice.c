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

#include "muse_voice.h"
#include "muse_up_next.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "muse_adpcm.h"
#include "muse_audio.h"
#include "muse_board.h"
#include "muse_chat.h"
#include "muse_gadget_mode.h"
#include "muse_input.h"
#include "muse_lock.h"
#include "muse_mem.h"
#include "muse_sd.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_wifi.h"
#if CONFIG_MUSE_TTS_PICO
#include "muse_tts.h"
#endif

static const char *TAG = "muse_voice";

#define MAX_SECS 15
#define TAIL_FRAMES (MUSE_AUDIO_RATE * 12 / 100)   /* capture lag + poll interval, stops before the release click */
#define MAX_FRAMES (MUSE_AUDIO_RATE * MAX_SECS)
#define MIN_HELD_FRAMES (MUSE_AUDIO_RATE * 3 / 10)   /* shorter presses are taps, not speech */
#if CONFIG_MUSE_BOARD_M5STACK_CARDPUTER_ADV || CONFIG_MUSE_BOARD_AI_PASSPORT
#define PRE_CHUNKS 6                                   /* 120 ms; a 320 ms upload burst stalls on the ADV and C3 */
#else
#define PRE_CHUNKS 16                                  /* 320 ms of audio kept from before the press */
#endif
#define SETTLE_CHUNKS 10   /* after Muse makes a sound, 200 ms of capture is its own tail */
#define EARCON_DROP_CHUNKS 8   /* 160 ms of capture after the listening ping: its echo, not speech */
#define EARCON_STALE_MS 300    /* an earcon not played by then isn't */
#define EARCON_LEVEL 3000.0f   /* peak, of 32767: well under a reply's level */
#define REST_BACKSTOP_MS 60000

#if CONFIG_MUSE_HATCH
/* A note recorded while Hatch is out of reach is saved in PSRAM, and goes once it's back. */
#define HOLD_NOTES 1
#else
#define HOLD_NOTES 0   /* no PSRAM: a press needs Hatch */
#endif
#define HELD_MAX 4
#define HELD_TRIES 8                            /* then a saved note is dropped */
#define HELD_MIN_FRAMES (MUSE_AUDIO_RATE / 2)    /* shorter isn't worth saving: a brush of the key */
#define HELD_STALE_US (120LL * 1000000)          /* in reach, a saved note this old has missed its moment */
#define HELD_POLL_MS 5000                       /* resting with notes saved: check for Wi-Fi this often */
#define HELD_KEEP_US (30LL * 60 * 1000000)      /* muse_voice_notes_waiting() */
#define RETRY_MIN_US (15LL * 1000000)
#define RETRY_MAX_US (120LL * 1000000)
#define STALL_US (30LL * 1000000)               /* Hatch took none of a note this long: give up */
#define ACK_WAIT_US (30LL * 1000000)            /* asleep, waiting for the VM to have a note */

static QueueHandle_t s_queue;
static volatile bool s_monitor;
static volatile float s_monitor_db = -100.0f;
static volatile bool s_chirp;
static volatile bool s_loopback;
static volatile bool s_mp3test;
static volatile int s_earcon = -1;         /* muse_earcon_t asked for, -1 for none */
static volatile uint32_t s_earcon_ms;      /* when, in ms since boot */

/*
 * Pre-roll: while idle the mic keeps running into this ring, so a recording
 * can start a little before the press. People start talking as they press,
 * and the power key reports the press late.
 */
typedef struct {
#if MUSE_LOW_MEM
    muse_adpcm_t start;   /* encoder state before this chunk, to decode it alone */
    uint8_t adpcm[MUSE_AUDIO_CHUNK / 2];
#else
    int16_t pcm[MUSE_AUDIO_CHUNK];
#endif
} pre_chunk_t;

static pre_chunk_t *s_pre;   /* PRE_CHUNKS, a ring */
static size_t s_pre_next, s_pre_fill;
#if MUSE_LOW_MEM
static muse_adpcm_t s_pre_enc;
#endif
static int16_t s_chunk[MUSE_AUDIO_CHUNK];
static int s_settle;

/*
 * The note being recorded (HOLD_NOTES), kept whole in case it can't go now.
 * Streaming, Hatch has had s_sent frames of it.
 */
static int16_t *s_rec;   /* MAX_FRAMES */
static size_t s_rec_n, s_sent;
static bool s_live = true;
static bool s_tried;     /* it went to Hatch */

typedef struct {
    int16_t *pcm;
    size_t frames;
    int64_t at_us;   /* when it was recorded */
    int tries;
} held_note_t;

/* An answer given on a reply's widget (muse_hatch_reply_text): "<chat>\n<words>",
 * in PSRAM, for the loop to take; a reply playing stops for it. */
#if CONFIG_MUSE_HATCH
EXT_RAM_BSS_ATTR static char *s_answer;

static bool answer_waiting(void)
{
    return __atomic_load_n(&s_answer, __ATOMIC_ACQUIRE) != NULL;
}
#else
static bool answer_waiting(void)
{
    return false;
}
#endif

static int s_held_count;
static volatile bool s_waiting;        /* muse_voice_notes_waiting() */
#if HOLD_NOTES
static held_note_t s_held[HELD_MAX];   /* oldest first */
static int64_t s_next_send_us;
static int64_t s_send_backoff_us = RETRY_MIN_US;
#endif

/* The i-th oldest pre-roll chunk, into out. */
static void pre_get(size_t i, int16_t *out)
{
    const pre_chunk_t *c = &s_pre[(s_pre_next + PRE_CHUNKS - s_pre_fill + i) % PRE_CHUNKS];
#if MUSE_LOW_MEM
    muse_adpcm_t dec = c->start;
    muse_adpcm_decode_block(&dec, c->adpcm, MUSE_AUDIO_CHUNK, out);
#else
    memcpy(out, c->pcm, sizeof(c->pcm));
#endif
}

/* Call after Muse plays anything: its own sound must not become pre-roll. */
static void pre_reset(void)
{
    s_pre_fill = 0;
    s_settle = SETTLE_CHUNKS;
}

/* One 20 ms idle read: feeds the pre-roll ring and the settings mic meter. */
static void idle_capture(void)
{
    if (muse_audio_read(s_chunk, MUSE_AUDIO_CHUNK) != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(20));
        return;
    }
    if (s_monitor) {
        float db = muse_audio_dbfs(s_chunk, MUSE_AUDIO_CHUNK);
        s_monitor_db = db > s_monitor_db ? db : s_monitor_db * 0.9f + db * 0.1f;
    }
    if (s_settle > 0) {
        s_settle--;
        return;
    }
    pre_chunk_t *c = &s_pre[s_pre_next];
#if MUSE_LOW_MEM
    c->start = s_pre_enc;
    muse_adpcm_encode_block(&s_pre_enc, s_chunk, MUSE_AUDIO_CHUNK, c->adpcm);
#else
    memcpy(c->pcm, s_chunk, sizeof(c->pcm));
#endif
    s_pre_next = (s_pre_next + 1) % PRE_CHUNKS;
    s_pre_fill = s_pre_fill < PRE_CHUNKS ? s_pre_fill + 1 : PRE_CHUNKS;
}

/* One earcon note through s_chunk: a sine gliding from f0 to f1 over ms, with a quick attack and a fast decay. */
static void earcon_note(float f0, float f1, int ms, float level)
{
    int n = MUSE_AUDIO_RATE * ms / 1000;
    float df = f1 - f0, phase = 0;
    for (int at = 0; at < n; at += MUSE_AUDIO_CHUNK) {
        int len = n - at < MUSE_AUDIO_CHUNK ? n - at : MUSE_AUDIO_CHUNK;
        for (int i = 0; i < len; i++) {
            float x = (float)(at + i) / n;
            phase += 2.0f * (float)M_PI * (f0 + df * x) / MUSE_AUDIO_RATE;
            float env = (x < 0.08f ? x / 0.08f : 1.0f) * (1.0f - x) * (1.0f - x);
            s_chunk[i] = (int16_t)(sinf(phase) * env * level);
        }
        muse_audio_write(s_chunk, len);
    }
}

/*
 * Plays an earcon (muse_voice.h) through s_chunk, which is free between
 * reads: one note (earcon_note), or the charge jingle's few. Nothing with
 * the speaker off; half as loud at night. Returns whether it played, so a
 * caller can skip the echo it leaves in the capture.
 */
static bool earcon_play(muse_earcon_t which)
{
    static const struct {
        uint16_t ms, f0, f1;
    } SHAPES[] = {
        [MUSE_EARCON_TICK] = { 25, 1800, 1700 },
        [MUSE_EARCON_LISTEN] = { 70, 880, 1320 },
        [MUSE_EARCON_STOP] = { 70, 740, 440 },
        [MUSE_EARCON_CLICK] = { 15, 2400, 1600 },
        [MUSE_EARCON_CHARGE] = { 0, 0, 0 },   /* CHARGE, below */
        /* A tap: high and gone at once, under the others' level, as a
         * keyboard's is; the primary one lower and a little longer. */
        [MUSE_EARCON_TAP] = { 10, 2100, 1700 },
        [MUSE_EARCON_TAP_PRIMARY] = { 16, 1400, 1050 },
    };
    /* Plugged in: E5 G#5 B5 plucked, up to an E6 that rings on, 0.5 s in all. */
    static const struct {
        uint16_t ms, f;
    } CHARGE[] = { { 75, 659 }, { 75, 831 }, { 75, 988 }, { 275, 1319 } };
    if ((unsigned)which >= sizeof(SHAPES) / sizeof(SHAPES[0]) || !muse_settings_speaker_on()
        || !muse_settings_volume()) {
        return false;
    }
    float level = EARCON_LEVEL * (muse_gadget_mode() == MUSE_GADGET_NIGHT ? 0.5f : 1.0f);
    if (which == MUSE_EARCON_CHARGE) {
        for (size_t k = 0; k < sizeof(CHARGE) / sizeof(CHARGE[0]); k++) {
            /* A touch of upward glide on each, and softer than a key's: it goes on longer. */
            earcon_note(CHARGE[k].f, CHARGE[k].f * 1.01f, CHARGE[k].ms, level * 0.8f);
        }
        return true;
    }
    if (which == MUSE_EARCON_TAP || which == MUSE_EARCON_TAP_PRIMARY) {
        level *= which == MUSE_EARCON_TAP ? 0.5f : 0.65f;
    }
    earcon_note(SHAPES[which].f0, SHAPES[which].f1, SHAPES[which].ms, level);
    return true;
}

/* Non-blocking: returns true if an event of `type` arrived (others dropped). */
static bool got_event(muse_ptt_t type)
{
    muse_input_event_t ev;
    bool hit = false;
    while (xQueueReceive(s_queue, &ev, 0) == pdTRUE) {
        muse_state_poke();
        hit |= ev.type == type;
    }
    return hit;
}

/* Level stats of a recording, gathered chunk by chunk for the log. */
typedef struct {
    double acc;
    size_t frames;
    int peak, clipped;
    float floor_db;
    float tail_db[25];   /* the last 0.5 s, to see where speech ends and the release click lands */
    size_t chunks;
} rec_stats_t;

/* Adds one captured chunk (up to MUSE_AUDIO_CHUNK frames) to the stats. */
static void keep(rec_stats_t *st, const int16_t *pcm, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int v = abs(pcm[i]);
        st->peak = v > st->peak ? v : st->peak;
        st->clipped += v >= 32000;
        st->acc += (double)pcm[i] * pcm[i];
    }
    st->frames += n;
    if (n == MUSE_AUDIO_CHUNK) {
        float db = muse_audio_dbfs(pcm, n);
        st->floor_db = db < st->floor_db ? db : st->floor_db;
        st->tail_db[st->chunks++ % 25] = db;
    }
}

/* Gives Hatch what it has room for of the note being recorded. */
static void feed_live(void)
{
#if HOLD_NOTES
    if (s_live && s_sent < s_rec_n) {
        s_sent += muse_hatch_turn_audio_wait(s_rec + s_sent, s_rec_n - s_sent, 0);
    }
#endif
}

/* Streams the note being recorded from its start, then the rest as it comes. */
static void go_live(void)
{
    muse_hatch_turn_begin();
    s_live = s_tried = true;
    s_sent = 0;
    feed_live();
}

/* One chunk of speech: into the stats, and on to Hatch, by way of the kept note if there is one. */
static void take(rec_stats_t *st, const int16_t *pcm)
{
    keep(st, pcm, MUSE_AUDIO_CHUNK);
    if (!s_rec) {
        if (s_live) {
            muse_hatch_turn_audio(pcm, MUSE_AUDIO_CHUNK);
        }
        return;
    }
    memcpy(s_rec + s_rec_n, pcm, MUSE_AUDIO_CHUNK * sizeof(int16_t));
    s_rec_n += MUSE_AUDIO_CHUNK;
    feed_live();
}

/*
 * Records speech until release or MAX_SECS, starting with the pre-roll; *held
 * gets the part after the press. It streams to Hatch as it goes if it can.
 * If not, the note is kept in s_rec to send later, and streams from partway
 * if Hatch comes within reach. Returns false if Hatch failed the turn with no
 * kept note to fall back on (why says what failed). It starts with the
 * listening ping (an earcon), and drops the capture's first 160 ms after it,
 * which would be the ping's own echo; the pre-roll, from before the ping,
 * keeps what was said as the key went down.
 */
static bool record(bool barge_in, size_t *held, char *why, size_t cap)
{
    muse_state_set_mode(MUSE_MODE_LISTENING);
    muse_state_set_progress(0);
    s_rec_n = s_sent = 0;
    s_live = s_tried = false;
    if (!s_rec || (muse_hatch_ready() && !s_held_count)) {
        go_live();
    }
    muse_state_set_caption(s_live ? "LISTENING..." : "RECORDING...");
    bool heard = false, ok = true;
    bool gave_up = false;   /* Hatch failed this note: it's kept, and goes later */
    char text[96];
    rec_stats_t st = { 0 };

    size_t n = 0;
    /* The listening ping, before any of the live capture: what the mic hears
     * of it is dropped below, so it never reaches the note. */
    bool pinged = earcon_play(MUSE_EARCON_LISTEN);
    if (barge_in) {
        /* Pressed during playback: the queued tail of the reply (and the
         * ping) is still sounding. */
        for (int i = 0; i < SETTLE_CHUNKS; i++) {
            muse_audio_read(s_chunk, MUSE_AUDIO_CHUNK);
        }
    } else {
        for (size_t i = 0; i < s_pre_fill; i++) {
            pre_get(i, s_chunk);
            take(&st, s_chunk);
            n += MUSE_AUDIO_CHUNK;
        }
    }
    size_t pre = n;
    for (int i = 0; pinged && !barge_in && i < EARCON_DROP_CHUNKS; i++) {
        /* The ping's echo: counted as held, but not kept. */
        muse_audio_read(s_chunk, MUSE_AUDIO_CHUNK);
        n += MUSE_AUDIO_CHUNK;
    }
    bool released = false;
    size_t stop_at = MAX_FRAMES;
    while (n + MUSE_AUDIO_CHUNK <= stop_at) {
        if (muse_audio_read(s_chunk, MUSE_AUDIO_CHUNK) != ESP_OK) {
            break;
        }
        muse_state_set_level(muse_audio_level(s_chunk, MUSE_AUDIO_CHUNK));
        take(&st, s_chunk);
        /* Live transcript as the caption. A failure stops the streaming; the
         * kept note goes later, or without one the failure is the caption. */
        muse_hatch_ev_t ev;
        while (s_live && (ev = muse_hatch_turn_event(text, sizeof(text))) != MUSE_HATCH_EV_NONE) {
            if (ev == MUSE_HATCH_EV_HEARD && text[0]) {
                heard = true;
                muse_state_set_caption("%s", text);
            } else if (ev == MUSE_HATCH_EV_ERROR) {
                ESP_LOGW(TAG, "muse: %s", text);
                muse_hatch_turn_cancel();
                s_live = false;
                gave_up = true;
                if (!s_rec) {
                    ok = false;
                    strlcpy(why, text, cap);
                    muse_state_set_caption("%s", text);
                }
            }
        }
        n += MUSE_AUDIO_CHUNK;
        bool tick = n % (MUSE_AUDIO_CHUNK * 5) == 0;
        if (tick && s_rec && !s_live && !gave_up && !s_held_count && muse_hatch_ready()) {
            ESP_LOGI(TAG, "Muse in reach: streaming the note so far");
            go_live();
        }
        muse_state_set_progress((float)n / MAX_FRAMES);
        if (!heard && ok && tick) {
            muse_state_set_caption("%s %.1fs", s_live ? "LISTENING" : "RECORDING", (double)n / MUSE_AUDIO_RATE);
        }
        /*
         * Capture runs 60-80 ms behind real time and people let go on their
         * last syllable, so keep going briefly after release.
         */
        if (!released && got_event(MUSE_PTT_UP)) {
            released = true;
            stop_at = n + TAIL_FRAMES < MAX_FRAMES ? n + TAIL_FRAMES : MAX_FRAMES;
        }
    }
    muse_state_set_level(0);
    *held = n - pre;

    char tail[160];
    int tl = 0;
    size_t shown = st.chunks < 25 ? st.chunks : 25;
    for (size_t i = st.chunks - shown; i < st.chunks; i++) {
        tl += snprintf(tail + tl, sizeof(tail) - tl, " %.0f", st.tail_db[i % 25]);
    }
    ESP_LOGI(TAG, "end levels:%s", tail);
    float rms_db = st.frames ? 10.0f * log10f((float)(st.acc / st.frames / (32768.0 * 32768.0)) + 1e-10f) : -100.0f;
    ESP_LOGI(TAG, "recorded %.2fs%s: rms %.1f dBFS, peak %.1f dBFS, floor %.1f dBFS, %d clipped, gain %d dB",
             (double)n / MUSE_AUDIO_RATE, released ? "" : " (max)", rms_db < -100.0f ? -100.0f : rms_db,
             20.0 * log10((st.peak + 1) / 32768.0), st.floor_db, st.clipped, muse_settings_mic_gain());
    return ok;
}

static void go_idle(const char *caption);

/*
 * Plays Hatch's reply as it arrives, with its text as the caption. Returns
 * true if interrupted by a new press. *delivered: the VM has the note.
 */
static bool hatch_reply(bool *delivered)
{
    muse_state_set_mode(MUSE_MODE_THINKING);
    muse_state_set_caption("SENDING VOICE NOTE");   /* until there's a transcript or reply */
    static int16_t buf[MUSE_AUDIO_CHUNK];
    static const int16_t silence[MUSE_AUDIO_CHUNK];
    char text[96];
    static char page[MUSE_CAPTION_MAX];
    bool done = false, speaking = false, replied = false;
    size_t played = 0;
    int64_t t0 = esp_timer_get_time();
    *delivered = false;
    for (;;) {
        muse_hatch_ev_t ev;
        while ((ev = muse_hatch_turn_event(text, sizeof(text))) != MUSE_HATCH_EV_NONE) {
            muse_sd_caption_event(ev, text);   /* the SD card's caption log, if any */
            if (ev == MUSE_HATCH_EV_HEARD || ev == MUSE_HATCH_EV_REPLY) {
                muse_up_next_turn_text(text);
            } else if (ev == MUSE_HATCH_EV_DONE) {
                muse_up_next_turn_done();
            }
            /* The face's phone (muse_state_turn): to his ear once the note's there. */
            if ((ev == MUSE_HATCH_EV_HEARD || ev == MUSE_HATCH_EV_SENT || ev == MUSE_HATCH_EV_IMAGE)
                && muse_state_turn() == MUSE_TURN_SENDING) {
                muse_state_set_turn(MUSE_TURN_SENT);
            }
            switch (ev) {
            case MUSE_HATCH_EV_HEARD:
                if (!speaking && !replied) {
                    muse_state_set_caption("\"%s\"", text);
                }
                break;
            case MUSE_HATCH_EV_SENT:
                *delivered = true;
                if (!speaking && !replied) {
#if CONFIG_MUSE_GADGET_HOME_EXTRAS
                    muse_state_set_caption("%s", "");   /* Muse's phone says it */
#else
                    muse_state_set_caption("NOTE SENT - WAITING FOR MUSE");
#endif
                }
                break;
            case MUSE_HATCH_EV_REPLY:
                replied = *delivered = true;
                muse_state_set_turn(MUSE_TURN_ANSWERED);
                /* Once speech starts, the caption follows it. The event only
                 * has room for the page's start; the page itself comes below. */
                if (!speaking && !muse_hatch_turn_caption(played, page, sizeof(page))) {
                    muse_state_set_caption("%s", text);
                }
                break;
            case MUSE_HATCH_EV_IMAGE:
                /* Muse thinking, "DOWNLOADING IMAGE..." up (muse_hatch_turn_caption
                 * says it below) until the image is held up and he speaks. */
                replied = *delivered = true;
                if (!speaking) {
                    muse_state_set_caption("%s", text);
                }
                break;
            case MUSE_HATCH_EV_DONE:
                done = *delivered = true;
                break;
            case MUSE_HATCH_EV_ERROR:
                ESP_LOGW(TAG, "muse: %s", text);
                muse_state_set_level(0);
                go_idle(text);
                return false;
            default:
                break;
            }
        }
        if (muse_lock_locked()) {
            ESP_LOGI(TAG, "reply stopped: locked");   /* nothing more said or shown (muse_lock.h) */
            muse_hatch_turn_cancel();
            muse_state_set_level(0);
            go_idle("");
            return false;
        }
        if (got_event(MUSE_PTT_DOWN)) {
            ESP_LOGI(TAG, "reply interrupted");
            muse_hatch_turn_cancel();
            muse_state_set_level(0);
            return true;
        }
        if (answer_waiting()) {
            ESP_LOGI(TAG, "reply cut short: a widget was answered");
            muse_hatch_turn_cancel();
            muse_state_set_level(0);
            return false;   /* the loop sends the answer next */
        }
        size_t n = muse_hatch_turn_read(buf, MUSE_AUDIO_CHUNK, speaking || done ? 0 : 20);
        if (n) {
            if (!speaking) {
                speaking = true;
                muse_state_set_mode(MUSE_MODE_SPEAKING);
                ESP_LOGI(TAG, "reply audio after %.2fs", (esp_timer_get_time() - t0) / 1e6);
            }
            muse_state_set_level(muse_audio_level(buf, n));
            muse_audio_write(buf, n);
            played += n;
        } else if (done) {
            break;
        } else if (speaking) {
            /* Between messages: keep the speaker fed so it doesn't replay stale DMA. */
            muse_state_set_level(0);
            muse_audio_write(silence, MUSE_AUDIO_CHUNK);
        }
        /* The page being said, or before the speech the reply's opening page. */
        if ((speaking || replied) && muse_hatch_turn_caption(played, page, sizeof(page))) {
            muse_state_set_caption("%s", page);
            muse_sd_caption_page(page);
        }
    }
    muse_sd_caption_flush();
    muse_state_set_level(0);
    ESP_LOGI(TAG, "muse reply: %.2fs of audio, %.2fs total", (double)played / MUSE_AUDIO_RATE,
             (esp_timer_get_time() - t0) / 1e6);
    if (!played) {
        /* No speech (TTS unavailable): leave the reply text up for a moment. */
        vTaskDelay(pdMS_TO_TICKS(2500));
    }
    return false;
}

static void go_idle(const char *caption)
{
    muse_state_set_progress(0);
    muse_state_set_mode(MUSE_MODE_IDLE);
    muse_state_set_caption("%s", caption);
}

#if CONFIG_MUSE_TTS_PICO
EXT_RAM_BSS_ATTR static char s_replay_page[MUSE_CAPTION_MAX];

/*
 * The last reply again without speech (the speaker off, or a mode that
 * doesn't speak replies): its pages, at the pace Pico would have said them,
 * the last held a moment. Any press stops it, as in replay_reply().
 */
static void replay_captions(const char *text)
{
    int64_t t0 = esp_timer_get_time();
    int64_t end_us = t0 + (int64_t)muse_tts_caption_frames(text) * 1000000 / MUSE_AUDIO_RATE + 2500000;
    muse_state_set_mode(MUSE_MODE_SPEAKING);   /* the reply's layout */
    for (int64_t now = t0; now < end_us; now = esp_timer_get_time()) {
        muse_input_event_t ev;
        if (xQueuePeek(s_queue, &ev, 0) == pdTRUE || answer_waiting()) {
            break;
        }
        size_t at = (size_t)((now - t0) * MUSE_AUDIO_RATE / 1000000);
        if (muse_tts_caption(text, at, s_replay_page, sizeof(s_replay_page))) {
            muse_state_set_caption("%s", s_replay_page);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "showed the last reply again (%u chars, not spoken)", (unsigned)strlen(text));
}

/*
 * Says the last reply again (muse_tts_replay_last), captions following, or
 * shows it again if replies aren't spoken now. Any press stops it and stays
 * queued for the loop, so talking cuts it short.
 */
static void replay_reply(void)
{
    char *text = heap_caps_malloc(MUSE_TTS_TEXT_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!text || !muse_tts_last(text, MUSE_TTS_TEXT_MAX)) {
        free(text);
        return;
    }
    if (!muse_tts_wanted() || !muse_tts_say(text)) {
        replay_captions(text);
        free(text);
        go_idle("");
        return;
    }
    EXT_RAM_BSS_ATTR static int16_t buf[MUSE_AUDIO_CHUNK];
    static const int16_t silence[MUSE_AUDIO_CHUNK];
    char *page = s_replay_page;
    const size_t page_cap = sizeof(s_replay_page);
    size_t played = 0;
    int64_t t0 = esp_timer_get_time();
    muse_state_set_mode(MUSE_MODE_SPEAKING);
    for (;;) {
        muse_input_event_t ev;
        if (xQueuePeek(s_queue, &ev, 0) == pdTRUE || answer_waiting()) {
            break;
        }
        /* Half a second ahead before the first word, as in a turn. */
        size_t n = 0;
        if (played || muse_tts_status() != MUSE_TTS_SPEAKING || muse_tts_buffered() >= MUSE_AUDIO_RATE / 2) {
            n = muse_tts_read(buf, MUSE_AUDIO_CHUNK);
        }
        if (n) {
            muse_state_set_level(muse_audio_level(buf, n));
            muse_audio_write(buf, n);
            played += n;
        } else if (muse_tts_status() != MUSE_TTS_SPEAKING || esp_timer_get_time() - t0 > 10 * 1000000LL + played * 1000000LL / MUSE_AUDIO_RATE) {
            break;   /* done, failed, or stuck */
        } else if (played) {
            muse_state_set_level(0);
            muse_audio_write(silence, MUSE_AUDIO_CHUNK);   /* keep the speaker fed */
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));   /* the engine loading */
        }
        if (muse_tts_caption(text, played, page, page_cap)) {
            muse_state_set_caption("%s", page);
        }
    }
    muse_tts_stop();
    free(text);
    muse_state_set_level(0);
    ESP_LOGI(TAG, "replayed %.2fs of the last reply", (double)played / MUSE_AUDIO_RATE);
    go_idle("");
}
#endif

/* Why a press can't go to Hatch; voice notes only go there. */
static const char *not_ready_reason(void)
{
    muse_hatch_status_t st;
    muse_hatch_status(&st);
    switch (st.state) {
    case MUSE_HATCH_NOT_SET: return "SET UP MUSE FIRST";
    case MUSE_HATCH_OFFLINE: return "NO WI-FI";
    default: return "CAN'T REACH MUSE";
    }
}

static volatile bool s_resting;

/*
 * Asleep on battery with nothing to do: power the codecs down (the loop rests
 * Wi-Fi alongside). They come back within one wait on waking, and a talk press
 * that woke Muse is kept for the loop (a note may follow). On USB power
 * nothing changes asleep.
 */
static void set_resting(bool rest)
{
    if (rest == s_resting) {
        return;
    }
    s_resting = rest;
    muse_audio_power(!rest);
    muse_hatch_set_resting(rest);
    if (rest) {
        /* Presses from before sleeping are stale, but not one that just woke Muse. */
        muse_input_event_t ev;
        while (xQueueReceive(s_queue, &ev, 0) == pdTRUE) {
            if (ev.wake) {
                xQueueSendToFront(s_queue, &ev, 0);
                break;
            }
        }
    } else {
        pre_reset();
    }
    ESP_LOGI(TAG, "%s", rest ? "resting: codecs off, Wi-Fi modem sleep" : "awake: codecs on");
}

/*
 * A press that woke the screen: listen from now, into the pre-roll, and
 * record only if it's still held after a tap's length. False if it was let
 * go sooner: it only woke Muse.
 */
static bool held_on_waking(void)
{
    s_settle = 0;   /* nothing played: no tail of Muse's own to skip */
    for (size_t n = 0; n < MIN_HELD_FRAMES; n += MUSE_AUDIO_CHUNK) {
        if (got_event(MUSE_PTT_UP)) {
            return false;
        }
        idle_capture();
    }
    return !got_event(MUSE_PTT_UP);
}

static void drop_rec(void)
{
    free(s_rec);
    s_rec = NULL;
}

#if HOLD_NOTES
/* True if a press is queued, left there for the loop; releases ahead of it are dropped. */
static bool press_waiting(void)
{
    muse_input_event_t ev;
    while (xQueuePeek(s_queue, &ev, 0) == pdTRUE) {
        if (ev.type == MUSE_PTT_DOWN) {
            return true;
        }
        xQueueReceive(s_queue, &ev, 0);
    }
    return false;
}

static void back_off(void)
{
    s_next_send_us = esp_timer_get_time() + s_send_backoff_us;
    s_send_backoff_us = s_send_backoff_us * 2 > RETRY_MAX_US ? RETRY_MAX_US : s_send_backoff_us * 2;
}

/* Saves the note just recorded to send later. `tried`: Hatch failed it, so it waits out the backoff. */
static void hold_rec(bool tried)
{
    if (s_held_count >= HELD_MAX) {   /* can_record() leaves room: not expected */
        drop_rec();
        go_idle("COULDN'T SAVE THE NOTE");
        return;
    }
    if (s_rec_n < HELD_MIN_FRAMES) {   /* none (realloc to 0 would free s_rec), or a brush of the key */
        drop_rec();
        go_idle("HOLD LONGER TO TALK");
        return;
    }
    int16_t *pcm = heap_caps_realloc(s_rec, s_rec_n * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_held[s_held_count++] = (held_note_t){
        .pcm = pcm ? pcm : s_rec,
        .frames = s_rec_n,
        .at_us = esp_timer_get_time(),
        .tries = tried,
    };
    s_rec = NULL;
    if (tried) {
        back_off();
    }
    ESP_LOGI(TAG, "saved a %.1fs note to send later (%d waiting)", (double)s_rec_n / MUSE_AUDIO_RATE, s_held_count);
    go_idle(tried ? "SAVED, WILL TRY AGAIN" : "SAVED, SENDS WHEN ONLINE");
}

static void drop_oldest(void)
{
    free(s_held[0].pcm);
    s_held_count--;
    memmove(s_held, s_held + 1, s_held_count * sizeof(s_held[0]));
    if (!s_held_count) {
        s_next_send_us = 0;
        s_send_backoff_us = RETRY_MIN_US;
    }
}

typedef enum {
    FED,
    FEED_FAILED,    /* the turn failed, or Hatch stopped taking audio */
    FEED_PRESSED,   /* a press is queued: it goes first */
} feed_t;

/*
 * Gives Hatch a note from *sent on, waiting for room as it goes, then ends
 * the turn. Stops for a press only if `yield`.
 */
static feed_t feed_rest(const int16_t *pcm, size_t frames, size_t *sent, bool yield)
{
    char text[96];
    int64_t moved = esp_timer_get_time();
    for (;;) {
        muse_hatch_ev_t ev;
        while ((ev = muse_hatch_turn_event(text, sizeof(text))) != MUSE_HATCH_EV_NONE) {
            if (ev == MUSE_HATCH_EV_ERROR) {
                ESP_LOGW(TAG, "muse: %s", text);
                return FEED_FAILED;
            }
        }
        if (*sent == frames) {
            break;
        }
        if (yield && press_waiting()) {
            return FEED_PRESSED;
        }
        size_t n = muse_hatch_turn_audio_wait(pcm + *sent, frames - *sent, 100);
        *sent += n;
        int64_t now = esp_timer_get_time();
        if (n) {
            moved = now;
        } else if (now - moved > STALL_US) {
            ESP_LOGW(TAG, "Muse stopped taking the note");
            return FEED_FAILED;
        }
    }
    muse_hatch_turn_end();
    return FED;
}

/* Asleep: waits for the VM to have the note just sent. */
static bool wait_delivered(void)
{
    char text[96];
    int64_t give_up = esp_timer_get_time() + ACK_WAIT_US;
    while (esp_timer_get_time() < give_up) {
        muse_hatch_ev_t ev;
        while ((ev = muse_hatch_turn_event(text, sizeof(text))) != MUSE_HATCH_EV_NONE) {
            if (ev == MUSE_HATCH_EV_SENT || ev == MUSE_HATCH_EV_REPLY || ev == MUSE_HATCH_EV_DONE) {
                return true;
            }
            if (ev == MUSE_HATCH_EV_ERROR) {
                ESP_LOGW(TAG, "muse: %s", text);
                return false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGW(TAG, "no word that the note arrived");
    return false;
}

/*
 * Sends the oldest saved note. Awake, it's a turn like any other, reply and
 * all; asleep (quiet), it's done once the VM has the note, and the reply
 * waits in the app. A press while it goes up comes first, and the note stays
 * saved. Returns true if a press interrupted the reply.
 */
static bool send_held(bool quiet)
{
    held_note_t *h = &s_held[0];
    ESP_LOGI(TAG, "sending a saved note%s: %.1fs, from %llds ago, try %d", quiet ? " (asleep)" : "",
             (double)h->frames / MUSE_AUDIO_RATE, (long long)((esp_timer_get_time() - h->at_us) / 1000000),
             h->tries + 1);
    if (!quiet) {
        muse_state_set_mode(MUSE_MODE_THINKING);
        muse_state_set_caption("SENDING SAVED NOTE");
    }
    muse_hatch_turn_begin();
    size_t sent = 0;
    feed_t fed = feed_rest(h->pcm, h->frames, &sent, true);
    bool delivered = false, interrupted = false;
    if (fed == FED && quiet) {
        delivered = wait_delivered();
    } else if (fed == FED) {
        interrupted = hatch_reply(&delivered);
    }
    if (fed != FED || quiet) {
        muse_hatch_turn_cancel();   /* asleep, the reply is left for the app */
    }
    if (fed == FEED_PRESSED) {
        if (!quiet) {
            go_idle("");
        }
        return false;
    }
    const char *caption;
    if (delivered || interrupted) {
        drop_oldest();
        s_next_send_us = 0;   /* the next one right away */
        s_send_backoff_us = RETRY_MIN_US;
#if !CONFIG_MUSE_GADGET_HOME_EXTRAS
        if (quiet) {
            muse_state_set_caption("SAVED NOTE SENT");
        }
#endif
        return interrupted;
    } else if (++h->tries >= HELD_TRIES) {
        ESP_LOGW(TAG, "giving up on a saved note after %d tries", h->tries);
        drop_oldest();
        caption = "COULDN'T SEND A SAVED NOTE";
    } else {
        back_off();
        caption = "SAVED NOTE: WILL TRY AGAIN";
    }
    if (quiet) {
        muse_state_set_caption("%s", caption);
    } else {
        go_idle(caption);
    }
    return false;
}

/*
 * Whether to send a saved note now: Hatch within reach and the backoff over.
 * Coming within reach starts the backoff over. Keeps s_waiting too.
 */
static bool held_due(void)
{
    static bool was_ready = true;
    if (!s_held_count) {
        was_ready = true;
        s_waiting = false;
        return false;
    }
    int64_t now = esp_timer_get_time();
    bool ready = muse_hatch_ready();
    /* In reach, an old saved note is dropped rather than answered minutes late. */
    while (ready && s_held_count && now - s_held[0].at_us > HELD_STALE_US) {
        ESP_LOGW(TAG, "dropping a saved note from %llds ago", (long long)((now - s_held[0].at_us) / 1000000));
        drop_oldest();
    }
    if (!s_held_count) {
        was_ready = ready;
        s_waiting = false;
        return false;
    }
    s_waiting = now - s_held[0].at_us < HELD_KEEP_US;
    if (ready && !was_ready) {
        s_next_send_us = 0;
        s_send_backoff_us = RETRY_MIN_US;
    }
    was_ready = ready;
    return ready && now >= s_next_send_us;
}
#endif

/*
 * After the release: the rest of the note goes to Hatch and the reply plays,
 * or if it can't go now it's saved to send later. Returns true if a press
 * interrupted the reply.
 */
static bool finish_note(void)
{
    bool delivered;
#if HOLD_NOTES
    if (s_rec) {
        bool fed = false, interrupted = false;
        delivered = false;
        if (s_live && s_sent == s_rec_n) {
            muse_hatch_turn_end();   /* before the stop ping, which takes 70 ms */
            fed = true;
        }
        earcon_play(MUSE_EARCON_STOP);
        if (s_live && !fed) {
            /* Hatch is behind (still connecting, say): the rest from the kept note. */
            muse_state_set_mode(MUSE_MODE_THINKING);
            muse_state_set_caption("SENDING VOICE NOTE");
            fed = feed_rest(s_rec, s_rec_n, &s_sent, false) == FED;
            if (!fed) {
                muse_hatch_turn_cancel();
            }
        }
        if (fed) {
            interrupted = hatch_reply(&delivered);
        }
        if (delivered || interrupted) {
            drop_rec();
        } else {
            hold_rec(s_tried);
        }
        return interrupted;
    }
#endif
    muse_hatch_turn_end();   /* before the stop ping, which takes 70 ms */
    earcon_play(MUSE_EARCON_STOP);
    return hatch_reply(&delivered);
}

/*
 * Whether a press can record: Hatch within reach, or (HOLD_NOTES) set up
 * with room to save the note. If not, the caption says why.
 */
static bool can_record(void)
{
    if (!muse_wifi_connected()) {
        muse_wifi_apply();   /* retry now, not after the backoff */
    }
    bool ready = muse_hatch_ready();
#if HOLD_NOTES
    muse_hatch_status_t st;
    muse_hatch_status(&st);
    if (st.state == MUSE_HATCH_NOT_SET) {
        go_idle("SET UP MUSE FIRST");
        return false;
    }
    if ((!ready || s_held_count) && s_held_count >= HELD_MAX) {
        go_idle("NOTES STILL WAITING TO SEND");
        return false;
    }
    if (ready && s_held_count) {
        s_next_send_us = 0;   /* the saved ones go first, right after */
    }
    s_rec = heap_caps_malloc(MAX_FRAMES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rec) {
        ESP_LOGE(TAG, "no memory to keep a note");
    }
#endif
    if (!ready && !s_rec) {
        go_idle(not_ready_reason());
        return false;
    }
    return true;
}

#if CONFIG_MUSE_HATCH
bool muse_hatch_reply_text(const char *sid, const char *text)
{
    if (!text || !text[0] || !muse_hatch_ready()) {
        return false;
    }
    sid = sid ? sid : "";
    size_t n = strlen(sid) + 1 + strlen(text) + 1;
    char *a = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!a) {
        return false;
    }
    snprintf(a, n, "%s\n%s", sid, text);
    free(__atomic_exchange_n(&s_answer, a, __ATOMIC_ACQ_REL));   /* a newer tap wins */
    muse_state_poke();
    muse_state_nudge();
    return true;
}

/*
 * The answer's turn: a talk press and release with the words typed in place
 * of the mic's (muse_hatch_typed_voice_to), then the reply as after a note.
 * True if a press cut the reply short.
 */
static bool answer_turn(void)
{
    char *a = __atomic_exchange_n(&s_answer, NULL, __ATOMIC_ACQ_REL);
    char *words = a ? strchr(a, '\n') : NULL;
    if (!words) {
        free(a);
        return false;
    }
    *words++ = '\0';
    ESP_LOGI(TAG, "answering a widget: \"%s\"", words);
    muse_wifi_power(MUSE_WIFI_FULL);
    bool interrupted = false, delivered;
    if (!muse_hatch_ready()) {
        go_idle(not_ready_reason());
    } else if (!muse_hatch_typed_voice_to(a, words)) {
        go_idle("CAN'T REACH MUSE");
    } else {
        muse_hatch_turn_begin();
        muse_hatch_turn_end();
        earcon_play(MUSE_EARCON_STOP);   /* off it goes, as at a release */
        interrupted = hatch_reply(&delivered);
    }
    free(a);
    return interrupted;
}
#endif

static void voice_task(void *arg)
{
    bool pending_down = false;
    muse_audio_selftest();
    for (;;) {
        bool wake = false;
        if (!pending_down) {
            muse_input_event_t ev;
            bool asleep = muse_state_asleep();
            bool battery = muse_state_on_battery();
            bool rest = asleep && battery && !s_chirp && !s_mp3test && !s_loopback;
#if HOLD_NOTES
            /* A press goes first: send_held() leaves it queued and returns
             * without backing off, so retrying before it's read would spin. */
            if (held_due() && !press_waiting() && !muse_lock_locked()) {   /* its reply waits for unlocking */
                set_resting(false);   /* full power while it goes, even asleep */
                muse_wifi_power(MUSE_WIFI_FULL);
                pending_down = send_held(asleep);
                pre_reset();
                if (!pending_down && !asleep && !answer_waiting() && muse_state_mode(NULL) != MUSE_MODE_IDLE) {
                    muse_state_make_happy();
                    go_idle("");
                }
                continue;
            }
#endif
            set_resting(rest);
            /* On battery Wi-Fi rests with the codecs, and between turns dozes
             * between beacons as xiaozhi's idle does; a press wakes it fully. */
            muse_wifi_power(rest ? MUSE_WIFI_REST : battery ? MUSE_WIFI_DOZE : MUSE_WIFI_FULL);
            if (rest) {
                /* Wait for waking, USB power or a bench request without
                 * polling (the timeout is only a backstop, or a look for
                 * Wi-Fi to send saved notes), so the CPU can stay asleep. */
                muse_state_wait_awake(s_waiting ? HELD_POLL_MS : REST_BACKSTOP_MS);
                continue;
            }
            if (s_chirp) {
                s_chirp = false;
                muse_audio_chirp(1);
                pre_reset();
            }
            int earcon = s_earcon;
            if (earcon >= 0) {
                s_earcon = -1;
                uint32_t age = (uint32_t)(esp_timer_get_time() / 1000) - s_earcon_ms;
                if (age < EARCON_STALE_MS && earcon_play((muse_earcon_t)earcon)) {
                    pre_reset();
                }
            }
            if (s_mp3test) {
                s_mp3test = false;
                int16_t *pcm = NULL;
                size_t n = muse_hatch_mp3_selftest(&pcm);
                for (size_t i = 0; i < n; i += MUSE_AUDIO_CHUNK) {
                    muse_audio_write(pcm + i, n - i < MUSE_AUDIO_CHUNK ? n - i : MUSE_AUDIO_CHUNK);
                }
                free(pcm);
                pre_reset();
            }
            if (s_loopback) {
                s_loopback = false;
                muse_audio_loopback_test(muse_settings_volume());
                pre_reset();
            }
#if CONFIG_MUSE_TTS_PICO
            if (muse_tts_replay_take()) {
                replay_reply();
                pre_reset();
            }
#endif
#if CONFIG_MUSE_HATCH
            if (answer_waiting()) {
                pending_down = answer_turn();
                pre_reset();
                if (!pending_down && !answer_waiting() && muse_state_mode(NULL) != MUSE_MODE_IDLE) {
                    muse_state_make_happy();
                    go_idle("");
                }
                continue;
            }
#endif
            /* The 20 ms read paces this loop. */
            idle_capture();
            if (xQueueReceive(s_queue, &ev, 0) != pdTRUE) {
                continue;
            }
            muse_state_poke();
            if (ev.type != MUSE_PTT_DOWN) {
                continue;
            }
            wake = ev.wake;
        }
        if (wake && !held_on_waking()) {
            continue;   /* a tap: it only woke Muse */
        }
        muse_wifi_power(MUSE_WIFI_FULL);
        if (!can_record()) {
            pending_down = false;
            continue;
        }
#if HOLD_NOTES
        /* In reach, what's said now goes first: older saved notes would only
         * hold it up, and be answered after it anyway. */
        if (s_held_count && muse_hatch_ready()) {
            ESP_LOGW(TAG, "a new note goes first: dropping %d saved", s_held_count);
            while (s_held_count) {
                drop_oldest();
            }
        }
#endif
        size_t held;
        char why[96];
        bool ok = record(pending_down, &held, why, sizeof(why));
        pending_down = false;
        if (held < MIN_HELD_FRAMES && !wake) {
            muse_hatch_turn_cancel();
            drop_rec();
            pre_reset();
            go_idle("HOLD LONGER TO TALK");
            continue;
        }
        if (!ok) {
            pre_reset();
            go_idle(why);   /* Hatch failed it while recording */
            continue;
        }
        pending_down = finish_note();
        pre_reset();
        if (!pending_down && !answer_waiting() && muse_state_mode(NULL) != MUSE_MODE_IDLE) {
            muse_state_make_happy();
            go_idle("");
        }
    }
}

esp_err_t muse_voice_start(QueueHandle_t queue)
{
    s_queue = queue;
    s_pre = heap_caps_malloc(PRE_CHUNKS * sizeof(pre_chunk_t), MUSE_BIG_CAPS);
    if (!s_pre || muse_audio_init(muse_settings_volume(), muse_settings_mic_gain()) != ESP_OK) {
        muse_state_set_mode(MUSE_MODE_ERROR);
        muse_state_set_caption("AUDIO INIT FAILED");
        return ESP_FAIL;
    }
    /* Stack in PSRAM if there is any (this task never writes flash) to spare internal RAM for Wi-Fi/BLE. */
    if (xTaskCreatePinnedToCoreWithCaps(voice_task, "muse_voice", 6144, NULL, 6, NULL, MUSE_AUDIO_CORE,
                                        MUSE_BIG_CAPS) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void muse_voice_set_monitor(bool on)
{
    s_monitor_db = -100.0f;
    s_monitor = on;
}

float muse_voice_monitor_db(void)
{
    return s_monitor_db;
}

void muse_voice_request_chirp(void)
{
    s_chirp = true;
    muse_state_nudge();
}

void muse_voice_earcon(muse_earcon_t which)
{
    /* Only between turns: the voice task is busy with the speaker otherwise. */
    if (muse_state_mode(NULL) != MUSE_MODE_IDLE || !muse_settings_speaker_on()) {
        return;
    }
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    bool tap = which == MUSE_EARCON_TAP || which == MUSE_EARCON_TAP_PRIMARY;
    if (tap) {
        int waiting = s_earcon;
        if (!muse_settings_touch_sounds()
            || (waiting >= 0 && waiting != MUSE_EARCON_TAP && waiting != MUSE_EARCON_TAP_PRIMARY
                && now - s_earcon_ms < EARCON_STALE_MS)) {
            return;   /* a volume step's or the charger's comes first */
        }
    }
    s_earcon_ms = now;
    s_earcon = which;
    muse_state_nudge();
}

void muse_voice_request_loopback(void)
{
    s_loopback = true;
    muse_state_nudge();
}

void muse_voice_request_mp3test(void)
{
    s_mp3test = true;
    muse_state_nudge();
}

bool muse_voice_resting(void)
{
    return s_resting;
}

bool muse_voice_notes_waiting(void)
{
    return s_waiting;
}
