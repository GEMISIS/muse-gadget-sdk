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
 * An image Muse shows the user (muse_present.h): decoded with the ROM's JPEG
 * decoder (as display.draw_url's are, main/image_fetch.c) into two RGB565
 * copies in PSRAM: one fitting the screen, for a tap to show full size, and
 * one at the size Muse holds it up (muse_ui_present). The same task asks Muse
 * to push a reply's image (muse_present_ask). It has its stack in PSRAM, so
 * it never touches flash or NVS: the microSD copy is saved by the extras
 * task (muse_sd_queue_image).
 */
#include "muse_present.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "rom/tjpgd.h"

#include "muse_chat.h"
#include "muse_mem.h"
#include "muse_sd.h"
#include "muse_settings.h"
#include "muse_ui.h"

static const char *TAG = "muse_present";

#define TASK_STACK (12 * 1024)          /* in PSRAM: the decoder, and asking */
#define TASK_PRIORITY 3                 /* under the UI (5), the chat session (5) and draw_url's (4) */
#define JOBS 3                          /* the one being shown, the next, and a nudge to ask */
#define JPEG_POOL_BYTES 3100            /* the ROM decoder's work pool, as its documentation asks */
#define DECODED_MAX (3 * 1024 * 1024)   /* RGB565 out of the decoder, before resizing */
#define LABEL_MAX 48
#define PATH_MAX_LEN 256                /* a workspace file's path, to ask for */
#define ASK_POLL_MS 1000                /* how often a request waiting to go, or for its reply, is looked at */
#define ASK_GIVE_UP_US (120 * 1000000LL)        /* waiting to ask */
#define ASKED_AGAIN_US (10 * 60 * 1000000LL)    /* a path asked for isn't asked for again this soon */
#define ASKED_KEPT 4

typedef struct {
    uint8_t *data;
    size_t len;
    char label[LABEL_MAX];
} job_t;

static QueueHandle_t s_jobs;   /* job_t *, or NULL to look at the asking */
static atomic_int s_started;   /* 0 not yet, 1 starting, 2 running */

static void job_free(job_t *job)
{
    if (!job) {
        return;
    }
    heap_caps_free(job->data);
    heap_caps_free(job);
}

static int64_t ms_since(int64_t t0)
{
    return (esp_timer_get_time() - t0) / 1000;
}

/* ---- JPEG --------------------------------------------------------------- */

typedef struct {
    const uint8_t *data;
    size_t len, pos;
    uint16_t *out;          /* w x h RGB565, as LVGL takes it */
    int w, h;
} jpeg_t;

static UINT jpeg_in(JDEC *jd, BYTE *buf, UINT len)
{
    jpeg_t *j = jd->device;
    size_t left = j->len - j->pos;
    len = len < left ? len : (UINT)left;
    if (buf) {
        memcpy(buf, j->data + j->pos, len);
    }
    j->pos += len;
    return len;
}

static UINT jpeg_out(JDEC *jd, void *bitmap, JRECT *rect)
{
    jpeg_t *j = jd->device;
    const uint8_t *rgb = bitmap;
    for (int y = rect->top; y <= rect->bottom; y++) {
        for (int x = rect->left; x <= rect->right; x++, rgb += 3) {
            if (x < j->w && y < j->h) {
                j->out[(size_t)y * j->w + x] = (uint16_t)((rgb[0] & 0xF8) << 8 | (rgb[1] & 0xFC) << 3 | rgb[2] >> 3);
            }
        }
    }
    return 1;
}

/* Fits w x h inside bw x bh, keeping its shape. */
static void fit(int w, int h, int bw, int bh, int *ow, int *oh)
{
    if ((int64_t)w * bh > (int64_t)h * bw) {
        *ow = bw;
        *oh = (int)((int64_t)h * bw / w);
    } else {
        *oh = bh;
        *ow = (int)((int64_t)w * bh / h);
    }
    *ow = *ow > 0 ? *ow : 1;
    *oh = *oh > 0 ? *oh : 1;
}

/* Bilinear, in 16.16; four samples a pixel when shrinking by half or more. */
static uint16_t *resize(const uint16_t *src, int sw, int sh, int dw, int dh)
{
    uint16_t *dst = heap_caps_malloc((size_t)dw * dh * sizeof(uint16_t), MUSE_BIG_CAPS);
    if (!dst) {
        return NULL;
    }
    int32_t sx = (int32_t)(((int64_t)sw << 16) / dw), sy = (int32_t)(((int64_t)sh << 16) / dh);
    bool super = sx >= (3 << 15) || sy >= (3 << 15);
    static const int32_t OFF[4][2] = { { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } };
    int taps = super ? 4 : 1;
    for (int y = 0; y < dh; y++) {
        for (int x = 0; x < dw; x++) {
            int r = 0, g = 0, b = 0;
            for (int k = 0; k < taps; k++) {
                /* The sample's centre, less half a source pixel. */
                int32_t fx = x * sx + sx / 2 - (1 << 15) + (super ? OFF[k][0] * sx / 4 : 0);
                int32_t fy = y * sy + sy / 2 - (1 << 15) + (super ? OFF[k][1] * sy / 4 : 0);
                fx = fx < 0 ? 0 : fx > ((sw - 1) << 16) ? ((sw - 1) << 16) : fx;
                fy = fy < 0 ? 0 : fy > ((sh - 1) << 16) ? ((sh - 1) << 16) : fy;
                int x0 = fx >> 16, y0 = fy >> 16;
                int x1 = x0 + 1 < sw ? x0 + 1 : x0, y1 = y0 + 1 < sh ? y0 + 1 : y0;
                int ax = (fx >> 8) & 0xff, ay = (fy >> 8) & 0xff;
                uint16_t p[4] = { src[(size_t)y0 * sw + x0], src[(size_t)y0 * sw + x1],
                                  src[(size_t)y1 * sw + x0], src[(size_t)y1 * sw + x1] };
                int wt[4] = { (256 - ax) * (256 - ay), ax * (256 - ay), (256 - ax) * ay, ax * ay };
                for (int i = 0; i < 4; i++) {
                    r += (p[i] >> 11) * wt[i];
                    g += ((p[i] >> 5) & 63) * wt[i];
                    b += (p[i] & 31) * wt[i];
                }
            }
            int div = taps << 16;
            dst[(size_t)y * dw + x] = (uint16_t)((r + div / 2) / div << 11 | (g + div / 2) / div << 5 | (b + div / 2) / div);
        }
    }
    return dst;
}

static void show_jpeg(const job_t *job)
{
    int sw, sh, photo;
    if (!muse_ui_present_sizes(&sw, &sh, &photo)) {
        ESP_LOGW(TAG, "\"%s\": no screen to show it on", job->label);
        return;
    }
    int64_t t0 = esp_timer_get_time();
    void *pool = heap_caps_malloc(JPEG_POOL_BYTES, MUSE_BIG_CAPS);
    if (!pool) {
        return;
    }
    jpeg_t j = { .data = job->data, .len = job->len };
    JDEC jd;
    JRESULT rc = jd_prepare(&jd, jpeg_in, pool, JPEG_POOL_BYTES, &j);
    uint16_t *full = NULL, *held = NULL;
    int fw = 0, fh = 0, hw = 0, hh = 0;
    uint8_t scale = 0;
    if (rc == JDR_OK) {
        fit(jd.width, jd.height, sw, sh, &fw, &fh);
        if (photo) {
            fit(jd.width, jd.height, photo, photo, &hw, &hh);
        }
        /* The most the decoder can shrink it and still leave as much as the screen shows. */
        while (scale < 3 && (int)(jd.width >> (scale + 1)) >= fw && (int)(jd.height >> (scale + 1)) >= fh) {
            scale++;
        }
        j.w = jd.width >> scale;
        j.h = jd.height >> scale;
        if ((size_t)j.w * j.h * sizeof(uint16_t) > DECODED_MAX) {
            ESP_LOGW(TAG, "\"%s\": %ux%u is too big to decode", job->label, (unsigned)jd.width, (unsigned)jd.height);
            rc = JDR_MEM1;
        } else {
            j.out = heap_caps_calloc((size_t)j.w * j.h, sizeof(uint16_t), MUSE_BIG_CAPS);
            rc = j.out ? jd_decomp(&jd, jpeg_out, scale) : JDR_MEM1;
        }
    }
    heap_caps_free(pool);
    if (rc != JDR_OK) {
        ESP_LOGW(TAG, "\"%s\": %s (%d)", job->label,
                 rc == JDR_FMT3 ? "unsupported JPEG: progressive, not baseline"
                 : rc == JDR_MEM1 ? "out of memory" : "not a valid JPEG", (int)rc);
        heap_caps_free(j.out);
        return;
    }
    int64_t t1 = esp_timer_get_time();
    full = fw == j.w && fh == j.h ? j.out : resize(j.out, j.w, j.h, fw, fh);
    held = photo ? resize(j.out, j.w, j.h, hw, hh) : NULL;
    if (full != j.out) {
        heap_caps_free(j.out);
    }
    if (!full || (photo && !held)) {
        ESP_LOGW(TAG, "\"%s\": out of memory", job->label);
        heap_caps_free(full);
        heap_caps_free(held);
        return;
    }
    ESP_LOGI(TAG, "\"%s\": %ux%u JPEG, decoded at 1/%d in %d ms, sized to %dx%d and %dx%d in %d ms", job->label,
             (unsigned)jd.width, (unsigned)jd.height, 1 << scale, (int)((t1 - t0) / 1000), fw, fh, hw, hh,
             (int)ms_since(t1));
    if (!muse_ui_present(full, fw, fh, held, hw, hh)) {
        ESP_LOGW(TAG, "\"%s\": the face didn't take it", job->label);
        heap_caps_free(full);
        heap_caps_free(held);
    }
}

/* ---- Asking for a reply's image ----------------------------------------- */

/* The image to ask for next (with s_ask_lock); a newer one replaces it. */
static portMUX_TYPE s_ask_lock = portMUX_INITIALIZER_UNLOCKED;
EXT_RAM_BSS_ATTR static struct {
    bool want;
    int64_t since_us;
    char path[PATH_MAX_LEN];
    char label[LABEL_MAX];
} s_want;

/* The paths asked for lately (with s_ask_lock). In PSRAM, as everything
 * here but the lock: internal RAM is short. */
EXT_RAM_BSS_ATTR static struct {
    uint32_t hash;
    int64_t us;
} s_asked[ASKED_KEPT];
EXT_RAM_BSS_ATTR static int s_asked_next;

/*
 * The task's own: each ask goes to a fresh chat, never listed, and has the
 * Muse delete the one before (the last answered ask's), so they don't pile
 * up in the Muse app. Kept in RAM: a restart leaves at most one behind.
 */
EXT_RAM_BSS_ATTR static char s_ask_sid[MUSE_CHAT_SID_MAX + 1];
EXT_RAM_BSS_ATTR static char s_ask_prev[MUSE_CHAT_SID_MAX + 1];

static void new_sid(char out[MUSE_CHAT_SID_MAX + 1])
{
    uint8_t u[16];
    esp_fill_random(u, sizeof(u));
    u[6] = (u[6] & 0x0f) | 0x40;
    u[8] = (u[8] & 0x3f) | 0x80;
    snprintf(out, MUSE_CHAT_SID_MAX + 1, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
}

/* The task's own: the request under way. */
EXT_RAM_BSS_ATTR static bool s_asking;
EXT_RAM_BSS_ATTR static uint32_t s_asking_hash;
EXT_RAM_BSS_ATTR static char s_asking_label[LABEL_MAX];

static uint32_t path_hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) {
        h = (h ^ (uint8_t)*s) * 16777619u;
    }
    return h;
}

/* With s_ask_lock held. */
static bool asked_lately(uint32_t hash, int64_t now)
{
    for (int i = 0; i < ASKED_KEPT; i++) {
        if (s_asked[i].us && s_asked[i].hash == hash && now - s_asked[i].us < ASKED_AGAIN_US) {
            return true;
        }
    }
    return false;
}

static bool ask_pending(void)
{
    portENTER_CRITICAL(&s_ask_lock);
    bool want = s_want.want;
    portEXIT_CRITICAL(&s_ask_lock);
    return want || s_asking;
}

/* Quotes would end the path or the caption early in the message. */
static void unquote(char *s)
{
    for (; *s; s++) {
        if (*s == '"' || *s == '\n' || *s == '\r') {
            *s = '\'';
        }
    }
}

/* Takes the reply to the request under way, then asks for the next image once it can. */
static void ask_tick(void)
{
    if (s_asking) {
        char reply[64];
        muse_chat_bg_state_t st = muse_chat_bg_result_for(MUSE_CHAT_BG_FOR_IMAGE, reply, sizeof(reply));
        if (st == MUSE_CHAT_BG_BUSY) {
            return;   /* the request gives up by itself after two minutes */
        }
        s_asking = false;
        strlcpy(s_ask_prev, s_ask_sid, sizeof(s_ask_prev));   /* deleted with the next ask */
        if (st == MUSE_CHAT_BG_DONE) {
            ESP_LOGI(TAG, "\"%s\": Muse answered \"%s\"", s_asking_label, reply);
        } else {
            ESP_LOGW(TAG, "\"%s\": Muse didn't answer the request to push it", s_asking_label);
            portENTER_CRITICAL(&s_ask_lock);
            for (int i = 0; i < ASKED_KEPT; i++) {
                if (s_asked[i].hash == s_asking_hash) {
                    s_asked[i].us = 0;   /* it can be asked for again */
                }
            }
            portEXIT_CRITICAL(&s_ask_lock);
        }
    }
    char path[PATH_MAX_LEN], label[LABEL_MAX];
    int64_t now = esp_timer_get_time(), since;
    portENTER_CRITICAL(&s_ask_lock);
    bool want = s_want.want;
    since = s_want.since_us;
    if (want) {
        memcpy(path, s_want.path, sizeof(path));
        memcpy(label, s_want.label, sizeof(label));
    }
    if (want && now - since > ASK_GIVE_UP_US) {
        s_want.want = false;
    }
    portEXIT_CRITICAL(&s_ask_lock);
    if (!want) {
        return;
    }
    if (now - since > ASK_GIVE_UP_US) {
        ESP_LOGW(TAG, "\"%s\": couldn't ask Muse for it in time; given up", label);
        return;
    }
    if (!muse_hatch_ready()) {
        return;   /* out of reach for now */
    }
    char msg[PATH_MAX_LEN + LABEL_MAX + 512];
    uint32_t hash = path_hash(path);
    unquote(path);
    unquote(label);
    /* Small: every byte goes as base64 Muse writes out, about 45 s per
     * 16 KiB chunk, so a 240 px JPEG (one chunk) shows in under a minute. */
    int n = 0;
    if (s_ask_prev[0]) {
        n = snprintf(msg, sizeof(msg),
                     "First, quietly delete the chat with session id %s (an earlier one of these); don't "
                     "mention it. Then: ", s_ask_prev);
    }
    /* A file in Muse's workspace (generated), or an image on the web it fetches first. */
    bool web = !strncmp(path, "http://", 7) || !strncmp(path, "https://", 8);
    snprintf(msg + n, sizeof(msg) - n,
             "%s the image at \"%s\" (\"%s\") %sto this gadget with display.show_image: first make a copy "
             "scaled to 240x240 (keep the aspect, fit inside), saved as a baseline JPEG at about 70%% quality, "
             "under 16 KiB, then send that copy in one chunk (offset 0, final=true), or in chunks of up to 16 KiB if it "
             "won't fit. Don't create links. Reply with just: sent.",
             web ? "Download" : "Send", path, label, web ? "and send it " : "");
    new_sid(s_ask_sid);
    if (!muse_chat_bg_ask_for(MUSE_CHAT_BG_FOR_IMAGE, s_ask_sid, msg)) {
        return;   /* someone else's request is under way, or a turn: next time */
    }
    s_asking = true;
    s_asking_hash = hash;
    strlcpy(s_asking_label, label, sizeof(s_asking_label));
    portENTER_CRITICAL(&s_ask_lock);
    s_asked[s_asked_next].hash = s_asking_hash;
    s_asked[s_asked_next].us = now;
    s_asked_next = (s_asked_next + 1) % ASKED_KEPT;
    if (s_want.since_us == since) {
        s_want.want = false;   /* unless a newer one came meanwhile */
    }
    portEXIT_CRITICAL(&s_ask_lock);
    ESP_LOGI(TAG, "\"%s\": asking Muse to push %s", label, path);
}

/* ---- Task --------------------------------------------------------------- */

static void run(job_t *job)
{
    const uint8_t *d = job->data;
    bool jpeg = job->len > 3 && d[0] == 0xFF && d[1] == 0xD8;
    bool png = job->len > 8 && !memcmp(d, "\x89PNG", 4);
    if (!jpeg) {
        ESP_LOGW(TAG, "\"%s\": %s: only JPEG is shown", job->label, png ? "a PNG" : "not an image");
        return;
    }
    if (muse_sd_ready() && muse_sd_queue_image(job->data, job->len, "jpg")) {
        ESP_LOGI(TAG, "\"%s\": queued for the microSD card", job->label);
    }
    show_jpeg(job);
}

static void present_task(void *arg)
{
    (void)arg;
    for (;;) {
        job_t *job = NULL;
        TickType_t wait = ask_pending() ? pdMS_TO_TICKS(ASK_POLL_MS) : portMAX_DELAY;
        if (xQueueReceive(s_jobs, &job, wait) == pdTRUE && job) {
            int64_t t0 = esp_timer_get_time();
            run(job);
            job_free(job);
            ESP_LOGI(TAG, "shown in %d ms; stack %u free", (int)ms_since(t0),
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
        ask_tick();
    }
}

static bool start(void)
{
    int expected = 0;
    if (atomic_compare_exchange_strong(&s_started, &expected, 1)) {
        s_jobs = xQueueCreateWithCaps(JOBS, sizeof(job_t *), MALLOC_CAP_SPIRAM);
        if (!s_jobs || xTaskCreatePinnedToCoreWithCaps(present_task, "muse_present", TASK_STACK, NULL,
                                                       TASK_PRIORITY, NULL, 1, MUSE_BIG_CAPS) != pdPASS) {
            ESP_LOGE(TAG, "start failed: Muse's images won't show");
            if (s_jobs) {
                vQueueDeleteWithCaps(s_jobs);
                s_jobs = NULL;
            }
            atomic_store(&s_started, 0);
            return false;
        }
        atomic_store(&s_started, 2);
    }
    return atomic_load(&s_started) == 2;
}

bool muse_present_bytes(uint8_t *data, size_t len, const char *label)
{
    job_t *job = heap_caps_calloc(1, sizeof(*job), MUSE_BIG_CAPS);
    if (!job) {
        heap_caps_free(data);
        return false;
    }
    job->data = data;
    job->len = len;
    strlcpy(job->label, label && label[0] ? label : "image", sizeof(job->label));
    if (!start() || xQueueSend(s_jobs, &job, 0) != pdTRUE) {
        ESP_LOGW(TAG, "\"%s\": busy with others, dropped", job->label);
        job_free(job);
        return false;
    }
    return true;
}

void muse_present_ask(const char *path, const char *label)
{
    if (!path || !path[0] || strlen(path) >= PATH_MAX_LEN) {
        ESP_LOGW(TAG, "no path to ask Muse for the image by");
        return;
    }
    int64_t now = esp_timer_get_time();
    uint32_t hash = path_hash(path);
    portENTER_CRITICAL(&s_ask_lock);
    bool asked = asked_lately(hash, now);
    if (!asked) {
        s_want.want = true;
        s_want.since_us = now;
        strlcpy(s_want.path, path, sizeof(s_want.path));
        strlcpy(s_want.label, label && label[0] ? label : "image", sizeof(s_want.label));
    }
    portEXIT_CRITICAL(&s_ask_lock);
    if (asked) {
        ESP_LOGI(TAG, "%s: asked for already", path);
        return;
    }
    job_t *wake = NULL;
    if (start()) {
        xQueueSend(s_jobs, &wake, 0);   /* if the queue's full, the task is busy and looks after it */
    }
}
