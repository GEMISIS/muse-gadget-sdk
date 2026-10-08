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
 * Replies spoken by SVOX Pico (picotts_engine.h). Each request is numbered;
 * the synthesis task works on the newest and gives up on it once a newer one
 * arrives or it's stopped, checking between steps of a few milliseconds. The
 * engine (1.1 MB of PSRAM) loads at boot and stays loaded, so a reply's first
 * words don't wait for it.
 */
#include "muse_tts.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "muse_chat_priv.h"
#include "muse_settings.h"
#include "picotts_engine.h"

static const char *TAG = "muse_tts";

#define PCM_BYTES (PICOTTS_SAMPLE_RATE * 2)       /* 1 s of speech ahead of the reader */
#define STEP_FRAMES 127                           /* the most the engine hands over per step */
#define STACK_BYTES (48 * 1024)                   /* in PSRAM: the task never writes flash; Pico's text
                                                   * pass (picopr) recursed past 16 KB on a long reply */
#define PRIORITY 3                                /* below the chat session (5) and the voice task (6) */
#define STOP_WAIT_MS 300                          /* for a stopped utterance to wind down */
#define CHARS_PER_S 14                            /* Pico's pace, for a replay's captions */

static TaskHandle_t s_task;
static SemaphoreHandle_t s_lock;
static StreamBufferHandle_t s_pcm;
static char *s_text;    /* cleaned text of the request being spoken */
static char *s_more;    /* muse_tts_more()'s cleaning space */
static char *s_last;    /* the last reply, for muse_tts_replay_last() */

static atomic_uint s_req;      /* the newest request */
static atomic_uint s_stop;     /* requests up to this one are stopped */
static atomic_uint s_done;     /* the last request whose speech is all in s_pcm */
static atomic_uint s_failed;   /* the last request the engine failed */
static atomic_bool s_busy;     /* the task is on a request */
static atomic_size_t s_len;    /* how much of s_text is there to say */
static atomic_bool s_final;    /* s_text is complete: no more will be added */
static atomic_bool s_replay;
static bool s_no_voice;        /* no voice partitions: never try */

__attribute__((weak)) bool muse_gadget_tts_allowed(void)
{
    return true;
}

static void log_heap(const char *when)
{
    ESP_LOGI(TAG, "%s: internal %u free (largest %u), PSRAM %u free (largest %u)", when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

static bool stopped(unsigned r)
{
    return atomic_load(&s_stop) >= r || atomic_load(&s_req) != r;
}

/* Hands speech to the reader, waiting while its buffer is full. False once stopped. */
static bool emit(unsigned r, const int16_t *pcm, size_t frames)
{
    const uint8_t *p = (const uint8_t *)pcm;
    size_t left = frames * sizeof(int16_t);
    while (left) {
        if (stopped(r)) {
            return false;
        }
        size_t n = xStreamBufferSend(s_pcm, p, left, pdMS_TO_TICKS(50));
        p += n;
        left -= n;
    }
    return true;
}

static bool open_engine(void)
{
    if (picotts_engine_is_open()) {
        return true;
    }
    log_heap("before engine load");
    int64_t t0 = esp_timer_get_time();
    if (!picotts_engine_open()) {
        return false;
    }
    ESP_LOGI(TAG, "engine loaded in %lld ms", (long long)(esp_timer_get_time() - t0) / 1000);
    log_heap("after engine load");
    return true;
}

static void speak(unsigned r)
{
    if (!open_engine()) {
        atomic_store(&s_failed, r);
        return;
    }
    EXT_RAM_BSS_ATTR static int16_t pcm[STEP_FRAMES];
    size_t pos = 0;
    size_t frames = 0;
    int64_t t0 = esp_timer_get_time();
    bool ok = true;
    while (ok && !stopped(r)) {
        /* Final before length: the length is set first, so it's complete once final is. */
        bool final = atomic_load(&s_final);
        size_t len = atomic_load(&s_len);
        if (pos >= len) {
            if (final) {
                break;
            }
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));   /* the next sentence */
            continue;
        }
        /* Up to a sentence end (muse_tts_more()), then a '\0' so Pico says it
         * now rather than waiting to see what follows. */
        int put = picotts_engine_put(s_text + pos, len - pos);
        if (put < 0) {
            ok = false;
            break;
        }
        pos += (size_t)put;
        if (pos >= len && picotts_engine_put("", 1) < 0) {
            ok = false;
            break;
        }
        picotts_step_t step;
        do {
            size_t n = 0;
            step = picotts_engine_get(pcm, STEP_FRAMES, &n);
            if (n && !emit(r, pcm, n)) {
                break;
            }
            frames += n;
        } while (step == PICOTTS_BUSY && !stopped(r));
        ok = step != PICOTTS_ERROR;
    }
    double secs = (esp_timer_get_time() - t0) / 1e6;
    if (!ok) {
        /* Start afresh next time. */
        picotts_engine_close();
        atomic_store(&s_failed, r);
        log_heap("engine failed and freed");
    } else if (stopped(r)) {
        picotts_engine_reset();
        ESP_LOGI(TAG, "stopped after %.2fs of speech", (double)frames / PICOTTS_SAMPLE_RATE);
    } else {
        atomic_store(&s_done, r);
        /* Under 1.0 keeps ahead of the speaker; the reply buffer absorbs the rest. */
        ESP_LOGI(TAG, "spoke %.2fs in %.2fs (%.2fx real time), stack %u bytes spare", (double)frames / PICOTTS_SAMPLE_RATE,
                 secs, frames ? secs * PICOTTS_SAMPLE_RATE / frames : 0.0, (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
}

static void tts_task(void *arg)
{
    (void)arg;
    unsigned handled = 0;
    open_engine();   /* now, not when the first reply is waiting on it */
    for (;;) {
        /* speak() may have taken the notification for a request that stopped it. */
        if (atomic_load(&s_req) == handled) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
        /* Busy before looking, so muse_tts_say() never rewrites text being read. */
        atomic_store(&s_busy, true);
        unsigned r = atomic_load(&s_req);
        if (r != handled && !stopped(r)) {
            handled = r;
            speak(r);
        }
        atomic_store(&s_busy, false);
    }
}

void muse_tts_init(void)
{
    if (s_task) {
        return;
    }
    if (!picotts_engine_map()) {
        s_no_voice = true;   /* logged by the engine; replies stay silent */
        return;
    }
    s_lock = xSemaphoreCreateMutex();
    s_pcm = xStreamBufferCreateWithCaps(PCM_BYTES, 1, MALLOC_CAP_SPIRAM);
    s_text = heap_caps_calloc(1, MUSE_TTS_TEXT_MAX, MALLOC_CAP_SPIRAM);
    s_last = heap_caps_calloc(1, MUSE_TTS_TEXT_MAX, MALLOC_CAP_SPIRAM);
    s_more = heap_caps_calloc(1, MUSE_TTS_TEXT_MAX, MALLOC_CAP_SPIRAM);
    if (!s_lock || !s_pcm || !s_text || !s_last || !s_more ||
        xTaskCreatePinnedToCoreWithCaps(tts_task, "muse_tts", STACK_BYTES, NULL, PRIORITY, &s_task, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "start failed");
        s_task = NULL;
        s_no_voice = true;
        return;
    }
    ESP_LOGI(TAG, "on-device speech ready (SVOX Pico, en-US)");
}

bool muse_tts_wanted(void)
{
    return s_task && !s_no_voice && muse_settings_speaker_on() && muse_gadget_tts_allowed();
}

bool muse_tts_say(const char *text)
{
    return muse_tts_start(text, true);
}

bool muse_tts_start(const char *text, bool final)
{
    if (!s_task || s_no_voice) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    unsigned prev = atomic_load(&s_req);
    atomic_store(&s_stop, prev);
    int64_t until = esp_timer_get_time() + STOP_WAIT_MS * 1000LL;
    while (atomic_load(&s_busy) && esp_timer_get_time() < until) {
        vTaskDelay(1);
    }
    bool ok = !atomic_load(&s_busy);
    if (!ok) {
        ESP_LOGW(TAG, "still busy with the last reply");
    } else {
        xStreamBufferReset(s_pcm);
        size_t n = muse_tts_clean(text, s_text, MUSE_TTS_TEXT_MAX);
        ok = n > 0;
        atomic_store(&s_len, n);
        atomic_store(&s_final, final);
    }
    if (ok) {
        atomic_store(&s_req, prev + 1);
        xTaskNotifyGive(s_task);
    }
    xSemaphoreGive(s_lock);
    return ok;
}

void muse_tts_more(const char *text, bool final)
{
    if (!s_task || s_no_voice) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t had = atomic_load(&s_len);
    size_t n = muse_tts_clean(text, s_more, MUSE_TTS_TEXT_MAX);
    if (n > had) {
        /* Only past what's there: the task may be reading the rest. */
        memcpy(s_text + had, s_more + had, n - had + 1);
        atomic_store(&s_len, n);
    }
    if (final) {
        atomic_store(&s_final, true);
    }
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
}

size_t muse_tts_read(int16_t *pcm, size_t frames)
{
    if (!s_pcm) {
        return 0;
    }
    return xStreamBufferReceive(s_pcm, pcm, frames * sizeof(int16_t), 0) / sizeof(int16_t);
}

size_t muse_tts_buffered(void)
{
    return s_pcm ? xStreamBufferBytesAvailable(s_pcm) / sizeof(int16_t) : 0;
}

muse_tts_status_t muse_tts_status(void)
{
    unsigned r = atomic_load(&s_req);
    if (atomic_load(&s_failed) == r) {
        return MUSE_TTS_FAILED;
    }
    if (atomic_load(&s_done) == r && xStreamBufferIsEmpty(s_pcm)) {
        return MUSE_TTS_DONE;
    }
    return MUSE_TTS_SPEAKING;
}

void muse_tts_stop(void)
{
    atomic_store(&s_stop, atomic_load(&s_req));
}

void muse_tts_remember(const char *text, bool append)
{
    if (!s_last || !text || !text[0]) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (append && s_last[0]) {
        strlcat(s_last, "\n", MUSE_TTS_TEXT_MAX);
        strlcat(s_last, text, MUSE_TTS_TEXT_MAX);
    } else {
        strlcpy(s_last, text, MUSE_TTS_TEXT_MAX);
    }
    xSemaphoreGive(s_lock);
}

bool muse_tts_last(char *out, size_t cap)
{
    if (!s_last || !cap) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(out, s_last, cap);
    xSemaphoreGive(s_lock);
    return out[0] != '\0';
}

bool muse_tts_caption(const char *text, size_t played, char *out, size_t cap)
{
    size_t len = strlen(text);
    size_t at = (size_t)((uint64_t)played * CHARS_PER_S / PICOTTS_SAMPLE_RATE);
    if (at >= len) {
        at = len ? len - 1 : 0;
    }
    return muse_hatch_caption_at(text, at, out, cap);
}

size_t muse_tts_caption_frames(const char *text)
{
    return (size_t)((uint64_t)strlen(text) * PICOTTS_SAMPLE_RATE / CHARS_PER_S);
}

void muse_tts_replay_last(void)
{
    if (s_task && s_last && s_last[0]) {
        atomic_store(&s_replay, true);
    }
}

bool muse_tts_replay_take(void)
{
    return atomic_exchange(&s_replay, false);
}
