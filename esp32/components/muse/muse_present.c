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
#include <strings.h>
#include <ctype.h>

#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "rom/tjpgd.h"

#include "muse_chat.h"
#if CONFIG_MUSE_PRESENT_FORMATS
#include "muse_image.h"
#endif
#include "muse_mem.h"
#include "muse_sd.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_ui.h"

static const char *TAG = "muse_present";

#define TASK_STACK (20 * 1024)          /* in PSRAM: the decoder, asking, and a TLS fetch of a web image */
#define WEB_MAX (768 * 1024)            /* a web image bigger than this goes to Muse to shrink */
#define WEB_TIMEOUT_MS 6000
#define TEST_PX 48                      /* smaller than this each way, a push is Muse testing, not the picture */
#define TEST_BYTES 900                  /* and a JPEG this small too: a 96 px preview is ~1.5 KB */
#define WEB_BUFFER 16384                /* esp_http_client's receive buffer (PSRAM: over the 4 KB internal limit) */
#define TASK_PRIORITY 3                 /* under the UI (5), the chat session (5) and draw_url's (4) */
#define JOBS 6                          /* the one being shown, the next, a nudge to ask, and others' calls */
#define JPEG_POOL_BYTES 3100            /* the ROM decoder's work pool, as its documentation asks */
#define DECODED_MAX (3 * 1024 * 1024)   /* RGB565 out of the decoder, before resizing */
#define LABEL_MAX 48
#define PATH_MAX_LEN 256                /* a workspace file's path, to ask for */
#define ASK_POLL_MS 1000                /* how often a request waiting to go, or for its reply, is looked at */
#define ASK_GIVE_UP_US (120 * 1000000LL)        /* waiting to ask */
#define ASK_ANSWER_US (270 * 1000000LL)         /* waiting for the request's end: past the session's own four minutes */
#define ASKED_AGAIN_US (10 * 60 * 1000000LL)    /* a path asked for isn't asked for again this soon */
#define ASKED_KEPT 4
#define FETCH_STALE_US (20 * 1000000LL)  /* a push's chunks stopped this long: given up on, as far as the face goes */

typedef struct {
    uint8_t *data;
    size_t len;
    char label[LABEL_MAX];
    bool sharper;   /* a bigger copy of the image already shown this turn (after its preview) */
    void (*call)(void *arg);   /* muse_present_call's, instead of an image */
    void *arg;
} job_t;

static QueueHandle_t s_jobs;   /* job_t *, or NULL to look at the asking */
static atomic_int s_started;   /* 0 not yet, 1 starting, 2 running */
static atomic_uint s_seq;      /* muse_present_seq */

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

/*
 * Sizes `img` (iw x ih RGB565, taken whatever happens) to fit the screen and
 * to the size Muse holds it up, and hands both to the face. False if it
 * couldn't be.
 */
static bool present_pixels(const job_t *job, uint16_t *img, int iw, int ih, int sw, int sh, int photo)
{
    int fw, fh, hw = 0, hh = 0;
    fit(iw, ih, sw, sh, &fw, &fh);
    if (photo) {
        fit(iw, ih, photo, photo, &hw, &hh);
    }
    int64_t t0 = esp_timer_get_time();
    uint16_t *full = fw == iw && fh == ih ? img : resize(img, iw, ih, fw, fh);
    uint16_t *held = photo ? resize(img, iw, ih, hw, hh) : NULL;
    if (full != img) {
        heap_caps_free(img);
    }
    if (!full || (photo && !held)) {
        ESP_LOGW(TAG, "\"%s\": out of memory", job->label);
        heap_caps_free(full);
        heap_caps_free(held);
        return false;
    }
    ESP_LOGI(TAG, "\"%s\": sized to %dx%d and %dx%d in %d ms", job->label, fw, fh, hw, hh, (int)ms_since(t0));
    if (!muse_ui_present(full, fw, fh, held, hw, hh, job->sharper)) {
        ESP_LOGW(TAG, "\"%s\": the face didn't take it", job->label);
        heap_caps_free(full);
        heap_caps_free(held);
        return false;
    }
    return true;
}

#if CONFIG_MUSE_PRESENT_FORMATS
/* A PNG or WebP (muse_image.h): decoded to fit the screen, then as a JPEG's. */
static bool show_other(const job_t *job, muse_image_kind_t kind)
{
    int sw, sh, photo;
    if (!muse_ui_present_sizes(&sw, &sh, &photo)) {
        ESP_LOGW(TAG, "\"%s\": no screen to show it on", job->label);
        return false;
    }
    int64_t t0 = esp_timer_get_time();
    muse_image_t img;
    char err[64];
    if (!muse_image_decode(job->data, job->len, sw, sh, &img, err, sizeof(err))) {
        ESP_LOGW(TAG, "\"%s\": %s: %s", job->label, muse_image_kind_name(kind), err);
        return false;
    }
    ESP_LOGI(TAG, "\"%s\": %dx%d %s, decoded to %dx%d in %d ms", job->label, img.src_w, img.src_h,
             muse_image_kind_name(kind), img.w, img.h, (int)ms_since(t0));
    return present_pixels(job, img.px, img.w, img.h, sw, sh, photo);
}
#endif

/* Decodes and hands it to the face; false if it couldn't be. */
static bool show_jpeg(const job_t *job)
{
    int sw, sh, photo;
    if (!muse_ui_present_sizes(&sw, &sh, &photo)) {
        ESP_LOGW(TAG, "\"%s\": no screen to show it on", job->label);
        return false;
    }
    int64_t t0 = esp_timer_get_time();
    void *pool = heap_caps_malloc(JPEG_POOL_BYTES, MUSE_BIG_CAPS);
    if (!pool) {
        return false;
    }
    jpeg_t j = { .data = job->data, .len = job->len };
    JDEC jd;
    JRESULT rc = jd_prepare(&jd, jpeg_in, pool, JPEG_POOL_BYTES, &j);
    int fw = 0, fh = 0;
    uint8_t scale = 0;
    if (rc == JDR_OK && (jd.width < TEST_PX || jd.height < TEST_PX)) {
        /* Muse checking the command works ("test", 16x16): not the picture. */
        ESP_LOGI(TAG, "\"%s\": %ux%u is a test, not the picture: not shown", job->label, (unsigned)jd.width,
                 (unsigned)jd.height);
        heap_caps_free(pool);
        return false;
    }
    if (rc == JDR_OK) {
        fit(jd.width, jd.height, sw, sh, &fw, &fh);
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
        return false;
    }
    ESP_LOGI(TAG, "\"%s\": %ux%u JPEG, decoded at 1/%d to %dx%d in %d ms", job->label, (unsigned)jd.width,
             (unsigned)jd.height, 1 << scale, j.w, j.h, (int)ms_since(t0));
    return present_pixels(job, j.out, j.w, j.h, sw, sh, photo);
}

/* ---- Fetching a web image ourselves --------------------------------------- */

static bool run(job_t *job);

/* Whether the bytes are a format shown here. */
static bool can_show(const uint8_t *d, size_t len)
{
    bool jpeg = len > 3 && d[0] == 0xFF && d[1] == 0xD8;
#if CONFIG_MUSE_PRESENT_FORMATS
    muse_image_kind_t kind = muse_image_kind(d, len);
    return jpeg || kind == MUSE_IMAGE_PNG || kind == MUSE_IMAGE_WEBP;
#else
    return jpeg;
#endif
}

typedef struct {
    uint8_t *buf;
    size_t len, cap;
    bool too_big;
    int64_t connected_us, first_us;   /* for the log: the handshake, then the bytes */
} fetch_t;

static void fetching(bool on, size_t received, size_t size);
static void decoding(int n);

static esp_err_t fetch_event(esp_http_client_event_t *ev)
{
    fetch_t *f = ev->user_data;
    if (ev->event_id == HTTP_EVENT_ON_CONNECTED && !f->connected_us) {
        f->connected_us = esp_timer_get_time();
        fetching(true, 0, 0);   /* connected: the boxes start coming */
    }
    if (ev->event_id != HTTP_EVENT_ON_DATA || f->too_big) {
        return ESP_OK;
    }
    if (!f->first_us) {
        f->first_us = esp_timer_get_time();
    }
    if (esp_http_client_get_status_code(ev->client) != 200) {
        return ESP_OK;   /* a redirect's body, or an error page */
    }
    if (f->len + ev->data_len > f->cap) {
        size_t cap = f->cap ? f->cap * 2 : 64 * 1024;
        while (cap < f->len + ev->data_len) {
            cap *= 2;
        }
        if (cap > WEB_MAX) {
            f->too_big = true;
            return ESP_OK;
        }
        uint8_t *grown = heap_caps_realloc(f->buf, cap, MUSE_BIG_CAPS);
        if (!grown) {
            f->too_big = true;
            return ESP_OK;
        }
        f->buf = grown;
        f->cap = cap;
    }
    memcpy(f->buf + f->len, ev->data, ev->data_len);
    f->len += ev->data_len;
    int64_t size = esp_http_client_get_content_length(ev->client);   /* 0 or less: chunked, not said */
    fetching(true, f->len, size > 0 ? (size_t)size : 0);
    return ESP_OK;
}

/*
 * A public web image (an https URL Muse wrote into a reply) fetched straight
 * here: a second or two, not the ~20 s of Muse pushing a copy. No token or
 * cookie goes with it. True if it was shown; else Muse is asked after all
 * (too big, not a baseline JPEG, refused, out of reach).
 */
static bool show_from_web(const char *url, const char *label)
{
    int64_t t0 = esp_timer_get_time();
    fetch_t f = { 0 };
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = WEB_TIMEOUT_MS,
        .buffer_size = WEB_BUFFER,
        .buffer_size_tx = 1024,
        .max_redirection_count = 4,
        .event_handler = fetch_event,
        .user_data = &f,
        .user_agent = "Mozilla/5.0 (MuseGadget)",
    };
    esp_http_client_handle_t http = esp_http_client_init(&cfg);
    if (!http) {
        return false;
    }
#if CONFIG_MUSE_PRESENT_FORMATS
    esp_http_client_set_header(http, "Accept", "image/jpeg,image/png,image/webp;q=0.9,image/*;q=0.5");
#else
    esp_http_client_set_header(http, "Accept", "image/jpeg,image/*;q=0.8");
#endif
    esp_err_t err = esp_http_client_perform(http);
    int status = esp_http_client_get_status_code(http);
    esp_http_client_cleanup(http);
    bool showable = f.len > 3 && can_show(f.buf, f.len);
    int64_t t1 = esp_timer_get_time();
    /* Connecting (DNS, TCP, TLS) apart from the bytes: which one was slow. */
    int connect_ms = f.connected_us ? (int)((f.connected_us - t0) / 1000) : -1;
    int body_ms = f.first_us ? (int)((t1 - f.first_us) / 1000) : 0;
    int kbps = body_ms > 0 ? (int)((uint64_t)f.len * 8 / body_ms) : 0;
    if (err != ESP_OK || status != 200 || f.too_big || !showable) {
        ESP_LOGW(TAG, "\"%s\": couldn't fetch it here (%s, HTTP %d, %u bytes%s%s) in %d ms (connected in %d, "
                 "%d kbit/s): asking Muse", label, esp_err_to_name(err), status, (unsigned)f.len,
                 f.too_big ? ", too big" : "", f.len && !showable ? ", a format not shown here" : "",
                 (int)ms_since(t0), connect_ms, kbps);
        heap_caps_free(f.buf);
        fetching(false, 0, 0);
        return false;
    }
    decoding(1);
    fetching(false, 0, 0);
    ESP_LOGI(TAG, "\"%s\": fetched here: %u bytes in %d ms (connected in %d ms, then %d ms at %d kbit/s)", label,
             (unsigned)f.len, (int)ms_since(t0), connect_ms, body_ms, kbps);
    job_t job = { .data = f.buf, .len = f.len };
    strlcpy(job.label, label, sizeof(job.label));
    bool shown = run(&job);
    decoding(-1);
    heap_caps_free(f.buf);
    return shown;
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
/* The ended requests' chats, still on the Muse (with s_ask_lock): deleting
 * one ahead of the next request kept that image waiting, so "up next"'s ask,
 * at its own pace, has them deleted. */
#define STALE_KEPT 4
EXT_RAM_BSS_ATTR static char s_stale[STALE_KEPT][MUSE_CHAT_SID_MAX + 1];

static void stale_add(const char *sid)
{
    if (!sid || !sid[0]) {
        return;
    }
    portENTER_CRITICAL(&s_ask_lock);
    int at = 0;
    for (int i = 0; i < STALE_KEPT; i++) {
        if (!s_stale[i][0]) {
            at = i;
            break;
        }
        at = (i + 1) % STALE_KEPT;   /* all full: the oldest goes undeleted */
    }
    strlcpy(s_stale[at], sid, sizeof(s_stale[at]));
    portEXIT_CRITICAL(&s_ask_lock);
}

size_t muse_present_stale_take(char *out, size_t cap)
{
    size_t n = 0;
    if (cap) {
        out[0] = '\0';
    }
    portENTER_CRITICAL(&s_ask_lock);
    for (int i = 0; i < STALE_KEPT && cap; i++) {
        if (s_stale[i][0] && n + strlen(s_stale[i]) + 3 < cap) {
            n += snprintf(out + n, cap - n, "%s%s", n ? ", " : "", s_stale[i]);
            s_stale[i][0] = '\0';
        }
    }
    portEXIT_CRITICAL(&s_ask_lock);
    return n;
}

static void new_sid(char out[MUSE_CHAT_SID_MAX + 1])
{
    uint8_t u[16];
    esp_fill_random(u, sizeof(u));
    u[6] = (u[6] & 0x0f) | 0x40;
    u[8] = (u[8] & 0x3f) | 0x80;
    snprintf(out, MUSE_CHAT_SID_MAX + 1, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
}

/*
 * A request under way, and whether a push came while it was (with
 * s_ask_lock): Muse may push the image by itself in the turn as well as
 * answer the request, and only the first of the two is shown.
 */
EXT_RAM_BSS_ATTR static bool s_guard, s_guard_shown;
EXT_RAM_BSS_ATTR static size_t s_guard_len;   /* the size of the one shown this turn */
/* The last pushed image shown, for a sharper copy of it that comes after its
 * request ended (Muse's answer can be late): with s_ask_lock. */
EXT_RAM_BSS_ATTR static int64_t s_shown_us;
EXT_RAM_BSS_ATTR static size_t s_shown_len;
EXT_RAM_BSS_ATTR static char s_shown_label[LABEL_MAX];
#define LATE_SHARPER_US (300LL * 1000000)
EXT_RAM_BSS_ATTR static bool s_web_tried;   /* the wanted web image was fetched here already (or tried) */

/* Waiting for an image (muse_present_wait), and its bytes so far (with s_ask_lock). */
EXT_RAM_BSS_ATTR static struct {
    int64_t since_us;   /* 0: not waiting */
    uint32_t seq;       /* s_seq then: a move means it's shown */
    size_t received, size;
} s_wait;

/*
 * Where the image's really got (with s_ask_lock), for muse_present_phase: its
 * bytes coming, and the images taken but not handed to the face yet. Plain,
 * not atomics: those don't work in PSRAM.
 */
EXT_RAM_BSS_ATTR static struct {
    bool on;
    int64_t last_us;
    size_t received, size;
} s_dl;
EXT_RAM_BSS_ATTR static int s_decoding;
EXT_RAM_BSS_ATTR static bool s_sharper_got;   /* the request's sharper copy came, and was handed on */
EXT_RAM_BSS_ATTR static uint32_t s_up_seq;    /* muse_present_up_seq */

/* A push of `len` bytes is the sharper copy of the last one shown, come after
 * its request ended: bigger, soon after, and no other image wanted since
 * (with s_ask_lock). It goes quietly in his pocket, not through the boxes. */
static bool late_sharper(size_t len)
{
    return !s_guard && !s_want.want && !s_wait.since_us && s_shown_us
           && esp_timer_get_time() - s_shown_us < LATE_SHARPER_US && len > s_shown_len;
}

static void fetching(bool on, size_t received, size_t size)
{
    portENTER_CRITICAL(&s_ask_lock);
    s_dl.on = on;
    s_dl.last_us = esp_timer_get_time();
    s_dl.received = received;
    s_dl.size = size;
    portEXIT_CRITICAL(&s_ask_lock);
}

static void decoding(int n)
{
    portENTER_CRITICAL(&s_ask_lock);
    s_decoding += n;
    portEXIT_CRITICAL(&s_ask_lock);
}

/* The task's own: the request under way. */
EXT_RAM_BSS_ATTR static bool s_asking;
EXT_RAM_BSS_ATTR static uint32_t s_asking_hash;
EXT_RAM_BSS_ATTR static int64_t s_asking_us;
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
        if (st == MUSE_CHAT_BG_BUSY && esp_timer_get_time() - s_asking_us > ASK_ANSWER_US) {
            /* The session ends a request in four minutes: this one never started, or lost its way. */
            ESP_LOGW(TAG, "\"%s\": the request to push it never ended; given up", s_asking_label);
            muse_chat_bg_abandon(MUSE_CHAT_BG_FOR_IMAGE);
            st = MUSE_CHAT_BG_FAILED;
        }
        if (st == MUSE_CHAT_BG_BUSY) {
            return;   /* the request gives up by itself after four minutes */
        }
        s_asking = false;
        portENTER_CRITICAL(&s_ask_lock);
        s_guard = s_guard_shown = false;
        portEXIT_CRITICAL(&s_ask_lock);
        stale_add(s_ask_sid);   /* deleted later, with "up next"'s ask: not on the way to an image */
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
    bool late = now - since > ASK_GIVE_UP_US;
    if (want) {
        memcpy(path, s_want.path, sizeof(path));
        memcpy(label, s_want.label, sizeof(label));
    }
    if (want && late) {
        s_want.want = false;
    }
    portEXIT_CRITICAL(&s_ask_lock);
    if (!want) {
        return;
    }
    if (late) {
        ESP_LOGW(TAG, "\"%s\": couldn't ask Muse for it in time; given up", label);
        return;
    }
    bool web_image = !strncmp(path, "https://", 8) || !strncmp(path, "http://", 7);
    if (web_image && !s_web_tried && muse_state_mode(NULL) != MUSE_MODE_LISTENING) {
        s_web_tried = true;   /* once a want: a failure goes on to Muse */
        if (show_from_web(path, label)) {
            portENTER_CRITICAL(&s_ask_lock);
            if (s_want.since_us == since) {
                s_want.want = false;
            }
            s_guard_shown = s_guard;
            portEXIT_CRITICAL(&s_ask_lock);
            atomic_fetch_add(&s_seq, 1);   /* the face has it */
            return;
        }
    }
    if (!muse_hatch_ready()) {
        return;   /* out of reach for now */
    }
    char msg[PATH_MAX_LEN + LABEL_MAX + 512];
    uint32_t hash = path_hash(path);
    unquote(path);
    unquote(label);
    /* Small: every byte goes as base64 Muse writes out, about 35 s per
     * 14K characters, so a 200 px JPEG under 12 KB (one chunk) shows in
     * well under a minute. */
    int n = 0;   /* nothing ahead of it: an earlier one's chat goes later (muse_present_stale_take) */
    /* A file in Muse's workspace (generated), or an image on the web it fetches first. */
    bool web = !strncmp(path, "http://", 7) || !strncmp(path, "https://", 8);
    snprintf(msg + n, sizeof(msg) - n,
             "%s the image at \"%s\" (\"%s\") %sto this gadget with display.show_image, twice, each in one "
             "chunk (offset 0, final=true): first a preview scaled to 96x96 (fit inside) as a baseline JPEG "
             "at 50%% quality, about 1.5 KB, then a 240x240 one at 70%% quality, under 14 KB. Make the base64 "
             "with code and paste its output exactly; if the gadget says it arrived damaged, encode it again. "
             "No test images. Don't create links. Reply with just: sent.",
             web ? "Download" : "Send", path, label, web ? "and send it " : "");
    new_sid(s_ask_sid);
    if (!muse_chat_bg_ask_for(MUSE_CHAT_BG_FOR_IMAGE, s_ask_sid, msg)) {
        return;   /* someone else's request is under way, or a turn: next time */
    }
    s_asking = true;
    s_asking_us = esp_timer_get_time();
    s_asking_hash = hash;
    strlcpy(s_asking_label, label, sizeof(s_asking_label));
    portENTER_CRITICAL(&s_ask_lock);
    s_asked[s_asked_next].hash = s_asking_hash;
    s_asked[s_asked_next].us = now;
    s_asked_next = (s_asked_next + 1) % ASKED_KEPT;
    s_guard = true;
    s_guard_shown = false;
    s_sharper_got = false;
    if (s_want.since_us == since) {
        s_want.want = false;   /* unless a newer one came meanwhile */
    }
    portEXIT_CRITICAL(&s_ask_lock);
    ESP_LOGI(TAG, "\"%s\": asking Muse to push %s", label, path);
}

/* ---- Task --------------------------------------------------------------- */

static bool run(job_t *job)
{
    const uint8_t *d = job->data;
    bool jpeg = job->len > 3 && d[0] == 0xFF && d[1] == 0xD8;
#if CONFIG_MUSE_PRESENT_FORMATS
    muse_image_kind_t kind = muse_image_kind(d, job->len);
    if (!jpeg && kind != MUSE_IMAGE_PNG && kind != MUSE_IMAGE_WEBP) {
        ESP_LOGW(TAG, "\"%s\": %s: only JPEG, PNG and WebP are shown", job->label, muse_image_kind_name(kind));
        return false;
    }
    const char *ext = jpeg ? "jpg" : muse_image_kind_ext(kind);
#else
    bool png = job->len > 8 && !memcmp(d, "\x89PNG", 4);
    if (!jpeg) {
        ESP_LOGW(TAG, "\"%s\": %s: only JPEG is shown", job->label, png ? "a PNG" : "not an image");
        return false;
    }
    const char *ext = "jpg";
#endif
    if (muse_sd_ready() && muse_sd_queue_image(job->data, job->len, ext)) {
        ESP_LOGI(TAG, "\"%s\": queued for the microSD card", job->label);
    }
#if CONFIG_MUSE_PRESENT_FORMATS
    if (!jpeg) {
        return show_other(job, kind);
    }
#endif
    return show_jpeg(job);
}

static void present_task(void *arg)
{
    (void)arg;
    for (;;) {
        job_t *job = NULL;
        TickType_t wait = ask_pending() ? pdMS_TO_TICKS(ASK_POLL_MS) : portMAX_DELAY;
        if (xQueueReceive(s_jobs, &job, wait) == pdTRUE && job && job->call) {
            job->call(job->arg);   /* someone else's: a map's tiles, where we are */
            heap_caps_free(job);
        } else if (job) {
            int64_t t0 = esp_timer_get_time();
            if (run(job)) {
                atomic_fetch_add(&s_seq, 1);   /* the face has it */
            }
            portENTER_CRITICAL(&s_ask_lock);
            if (job->sharper) {
                s_sharper_got = true;   /* handed on, or not to be had */
            } else {
                s_decoding--;
            }
            portEXIT_CRITICAL(&s_ask_lock);
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

/* A label's words that name something: lower case, three letters or more, not filler. */
static bool label_word(const char *w, size_t n)
{
    static const char *const FILLER[] = { "the", "and", "with", "image", "photo", "picture", "preview",
                                          "sharper", "copy", "jpeg", "for", "from", "your" };
    if (n < 3) {
        return false;
    }
    for (size_t i = 0; i < sizeof(FILLER) / sizeof(FILLER[0]); i++) {
        if (strlen(FILLER[i]) == n && !strncasecmp(w, FILLER[i], n)) {
            return false;
        }
    }
    return true;
}

/* The next word in *p (letters and digits): false at the end. */
static bool next_word(const char **p, const char **w, size_t *n)
{
    while (**p && !isalnum((unsigned char)**p)) {
        (*p)++;
    }
    *w = *p;
    while (isalnum((unsigned char)**p)) {
        (*p)++;
    }
    *n = (size_t)(*p - *w);
    return *n > 0;
}

/* Whether a label has a word that names something. */
static bool label_says(const char *s)
{
    const char *w;
    size_t n;
    while (next_word(&s, &w, &n)) {
        if (label_word(w, n)) {
            return true;
        }
    }
    return false;
}

/* Whether two labels name the same thing: a word in common ("leopard" and
 * "leopards" count), or one of them says nothing much ("image", "preview"). */
static bool labels_agree(const char *a, const char *b)
{
    if (!label_says(a) || !label_says(b)) {
        return true;
    }
    const char *pa = a, *w;
    size_t n;
    while (next_word(&pa, &w, &n)) {
        if (!label_word(w, n)) {
            continue;
        }
        const char *pb = b, *v;
        size_t m;
        while (next_word(&pb, &v, &m)) {
            size_t k = n < m ? n : m, d = n > m ? n - m : m - n;
            if (label_word(v, m) && d <= 2 && k >= 3 && !strncasecmp(w, v, k)) {
                return true;
            }
        }
    }
    return false;
}

bool muse_present_push_ok(const char *label, char *why, size_t cap)
{
    if (!label || !label[0]) {
        return true;   /* nothing to go by */
    }
    char expect[LABEL_MAX] = "";
    bool any = false;
    portENTER_CRITICAL(&s_ask_lock);
    if (s_asking) {
        memcpy(expect, s_asking_label, sizeof(expect));
    } else if (s_want.want) {
        memcpy(expect, s_want.label, sizeof(expect));
    } else if (s_wait.since_us) {
        any = true;   /* the turn's own, named by nothing yet */
    } else if (s_shown_us && esp_timer_get_time() - s_shown_us < LATE_SHARPER_US) {
        memcpy(expect, s_shown_label, sizeof(expect));   /* its sharper copy, late */
    }
    portEXIT_CRITICAL(&s_ask_lock);
    muse_mode_t mode = muse_state_mode(NULL);
    if (any || (!expect[0] && (mode == MUSE_MODE_THINKING || mode == MUSE_MODE_SPEAKING))) {
        return true;
    }
    if (!expect[0]) {
        snprintf(why, cap, "not shown: no image is wanted now");
        return false;
    }
    if (labels_agree(label, expect)) {
        return true;
    }
    snprintf(why, cap, "not shown: this gadget is waiting for \"%s\", not \"%s\"", expect, label);
    return false;
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
    if (!strcasecmp(job->label, "test") || len < TEST_BYTES) {
        /* Muse trying the command out: not the picture, and not to be taken for it. */
        ESP_LOGI(TAG, "\"%s\" (%u bytes): a test push, not the picture: ignored", job->label, (unsigned)len);
        job_free(job);
        fetching(false, 0, 0);
        return true;
    }
    /* Muse pushed one by itself (the mode's contract asks it to): one still
     * waiting to be asked for needn't be. One asked for already is this, likely. */
    char called_off[LABEL_MAX] = "";
    portENTER_CRITICAL(&s_ask_lock);
    if (s_want.want) {
        s_want.want = false;
        memcpy(called_off, s_want.label, sizeof(called_off));
    }
    /* A second push in the turn is shown only if it's bigger: the sharp copy
     * after the preview. The same again, or a smaller one, isn't. */
    bool twice = s_guard && s_guard_shown && job->len <= s_guard_len;
    job->sharper = (s_guard && s_guard_shown && !twice) || late_sharper(job->len);
    if (s_guard && !twice) {
        s_guard_shown = true;
        s_guard_len = job->len;
    }
    if (!twice) {
        s_shown_us = esp_timer_get_time();
        s_shown_len = job->len;
        if (!job->sharper) {
            strlcpy(s_shown_label, job->label, sizeof(s_shown_label));
        }
    }
    s_dl.on = false;   /* all here */
    if (!twice && !job->sharper) {
        s_decoding++;
    }
    portEXIT_CRITICAL(&s_ask_lock);
    if (called_off[0]) {
        ESP_LOGI(TAG, "\"%s\": pushed by Muse itself; not asking for \"%s\"", job->label, called_off);
    }
    if (twice) {
        /* By itself in the turn, and again for the request (or the other way round). */
        ESP_LOGI(TAG, "\"%s\": pushed again, no bigger than the one shown; that one stays", job->label);
        job_free(job);
        return true;
    }
    if (!start() || xQueueSend(s_jobs, &job, 0) != pdTRUE) {
        ESP_LOGW(TAG, "\"%s\": busy with others, dropped", job->label);
        portENTER_CRITICAL(&s_ask_lock);
        if (job->sharper) {
            s_sharper_got = true;
        } else {
            s_decoding--;
        }
        portEXIT_CRITICAL(&s_ask_lock);
        job_free(job);
        return false;
    }
    return true;
}

bool muse_present_call(void (*fn)(void *arg), void *arg)
{
    job_t *job = heap_caps_calloc(1, sizeof(*job), MUSE_BIG_CAPS);
    if (!job) {
        return false;
    }
    job->call = fn;
    job->arg = arg;
    if (!start() || xQueueSend(s_jobs, &job, 0) != pdTRUE) {
        heap_caps_free(job);
        return false;
    }
    return true;
}

typedef struct {
    uint8_t *buf;
    size_t len, cap, max;
    bool too_big;
} got_t;

static esp_err_t got_event(esp_http_client_event_t *ev)
{
    got_t *g = ev->user_data;
    if (ev->event_id != HTTP_EVENT_ON_DATA || g->too_big) {
        return ESP_OK;
    }
    int status = esp_http_client_get_status_code(ev->client);
    if (status >= 300 && status < 400) {
        return ESP_OK;   /* a redirect's body */
    }
    if (g->len + ev->data_len > g->cap) {
        size_t cap = g->cap ? g->cap * 2 : 16 * 1024;
        while (cap < g->len + ev->data_len) {
            cap *= 2;
        }
        cap = cap > g->max ? g->max : cap;
        uint8_t *grown = g->len + ev->data_len <= cap ? heap_caps_realloc(g->buf, cap + 1, MUSE_BIG_CAPS) : NULL;
        if (!grown) {
            g->too_big = true;
            return ESP_OK;
        }
        g->buf = grown;
        g->cap = cap;
    }
    memcpy(g->buf + g->len, ev->data, ev->data_len);
    g->len += ev->data_len;
    g->buf[g->len] = 0;   /* text ends there */
    return ESP_OK;
}

uint8_t *muse_present_fetch(const char *url, const char *body, size_t max, size_t *len, int *status)
{
    int64_t t0 = esp_timer_get_time();
    got_t g = { .max = max };
    esp_http_client_config_t cfg = {
        .url = url,
        .method = body ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = WEB_TIMEOUT_MS,
        .buffer_size = WEB_BUFFER,
        .buffer_size_tx = 1024,
        .max_redirection_count = 3,
        .event_handler = got_event,
        .user_data = &g,
        .user_agent = "MuseGadget-personal/1.0 (+https://github.com/GEMISIS/muse-gadget-sdk)",
    };
    *len = 0;
    *status = 0;
    esp_http_client_handle_t http = esp_http_client_init(&cfg);
    if (!http) {
        return NULL;
    }
    if (body) {
        esp_http_client_set_header(http, "Content-Type", "application/json");
        esp_http_client_set_post_field(http, body, (int)strlen(body));
    }
    esp_err_t err = esp_http_client_perform(http);
    *status = esp_http_client_get_status_code(http);
    esp_http_client_cleanup(http);
    if (err != ESP_OK || g.too_big || !g.len) {
        ESP_LOGW(TAG, "%.48s...: %s, HTTP %d, %u bytes%s in %d ms", url, esp_err_to_name(err), *status,
                 (unsigned)g.len, g.too_big ? " (too big)" : "", (int)ms_since(t0));
        heap_caps_free(g.buf);
        return NULL;
    }
    *len = g.len;
    return g.buf;
}

bool muse_present_decode(const uint8_t *data, size_t len, int fit_w, int fit_h, uint16_t **px, int *w, int *h)
{
    *px = NULL;
    if (len > 3 && data[0] == 0xFF && data[1] == 0xD8) {
        void *pool = heap_caps_malloc(JPEG_POOL_BYTES, MUSE_BIG_CAPS);
        if (!pool) {
            return false;
        }
        jpeg_t j = { .data = data, .len = len };
        JDEC jd;
        JRESULT rc = jd_prepare(&jd, jpeg_in, pool, JPEG_POOL_BYTES, &j);
        uint8_t scale = 0;
        int fw = 0, fh = 0;
        if (rc == JDR_OK) {
            fit(jd.width, jd.height, fit_w, fit_h, &fw, &fh);
            while (scale < 3 && (int)(jd.width >> (scale + 1)) >= fw && (int)(jd.height >> (scale + 1)) >= fh) {
                scale++;
            }
            j.w = jd.width >> scale;
            j.h = jd.height >> scale;
            j.out = (size_t)j.w * j.h * 2 <= DECODED_MAX ? heap_caps_calloc((size_t)j.w * j.h, 2, MUSE_BIG_CAPS) : NULL;
            rc = j.out ? jd_decomp(&jd, jpeg_out, scale) : JDR_MEM1;
        }
        heap_caps_free(pool);
        if (rc != JDR_OK) {
            heap_caps_free(j.out);
            return false;
        }
        if (j.w > fw || j.h > fh) {
            uint16_t *small = resize(j.out, j.w, j.h, fw, fh);
            heap_caps_free(j.out);
            if (!small) {
                return false;
            }
            j.out = small;
            j.w = fw;
            j.h = fh;
        }
        *px = j.out;
        *w = j.w;
        *h = j.h;
        return true;
    }
#if CONFIG_MUSE_PRESENT_FORMATS
    muse_image_t img;
    char err[48];
    if (!muse_image_decode(data, len, fit_w, fit_h, &img, err, sizeof(err))) {
        ESP_LOGW(TAG, "decoding: %s", err);
        return false;
    }
    *px = img.px;
    *w = img.w;
    *h = img.h;
    return true;
#else
    return false;
#endif
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
        s_web_tried = false;
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

uint32_t muse_present_seq(void)
{
    return atomic_load(&s_seq);
}

void muse_present_wait(bool on)
{
    int64_t now = esp_timer_get_time();
    uint32_t seq = atomic_load(&s_seq);
    portENTER_CRITICAL(&s_ask_lock);
    if (!on) {
        s_wait.since_us = 0;
    } else if (!s_wait.since_us) {
        s_wait.since_us = now;
        s_wait.seq = seq;
        s_wait.received = s_wait.size = 0;
    }
    portEXIT_CRITICAL(&s_ask_lock);
}

void muse_present_chunk(size_t received, size_t size)
{
    portENTER_CRITICAL(&s_ask_lock);
    bool late = late_sharper(size);
    s_wait.received = received;
    s_wait.size = size;
    /* Not the sharper copy after the one shown, nor a test push in one chunk. */
    if (!(s_guard && s_guard_shown) && !late && !(size == received && received < TEST_BYTES)) {
        s_dl.on = true;
        s_dl.last_us = esp_timer_get_time();
        s_dl.received = received;
        s_dl.size = size;
    }
    portEXIT_CRITICAL(&s_ask_lock);
}

int muse_present_progress(void)
{
    int64_t now = esp_timer_get_time();
    uint32_t seq = atomic_load(&s_seq);
    portENTER_CRITICAL(&s_ask_lock);
    int64_t since = s_wait.since_us;
    bool shown = seq != s_wait.seq;
    size_t received = s_wait.received, size = s_wait.size;
    portEXIT_CRITICAL(&s_ask_lock);
    if (!since) {
        return -1;
    }
    return shown ? 100 : muse_present_estimate(now - since, received, size);
}

muse_present_phase_t muse_present_phase(float *progress)
{
    int64_t now = esp_timer_get_time();
    uint32_t seq = atomic_load(&s_seq);
    portENTER_CRITICAL(&s_ask_lock);
    bool waiting = s_wait.since_us && seq == s_wait.seq;
    bool fetch = s_dl.on && now - s_dl.last_us < FETCH_STALE_US;
    size_t received = s_dl.received, size = s_dl.size;
    int decode = s_decoding;
    portEXIT_CRITICAL(&s_ask_lock);
    *progress = -1.0f;
    if (decode > 0) {
        *progress = 1.0f;
        return MUSE_PRESENT_DECODING;
    }
    if (fetch) {
        if (size) {
            float p = (float)received / (float)size;
            *progress = p < 0 ? 0 : p > 1 ? 1 : p;
        }
        return MUSE_PRESENT_FETCHING;
    }
    return waiting ? MUSE_PRESENT_WAITING : MUSE_PRESENT_NONE;
}

/* With s_ask_lock held: the wanted image is a web one, and *here, still to be (or being) fetched here. */
static bool want_web(bool *here)
{
    bool web = !strncmp(s_want.path, "https://", 8) || !strncmp(s_want.path, "http://", 7);
    *here = web && (!s_web_tried || s_dl.on || s_decoding > 0);
    return web;
}

bool muse_present_pushing(void)
{
    portENTER_CRITICAL(&s_ask_lock);
    bool here;
    bool web = want_web(&here);
    /* A web image that couldn't be fetched here is Muse's to push, as a file is. */
    bool pushing = (s_guard && !s_guard_shown) || (s_want.want && (!web || !here));
    portEXIT_CRITICAL(&s_ask_lock);
    return pushing;
}

bool muse_present_found(void)
{
    portENTER_CRITICAL(&s_ask_lock);
    bool here;
    want_web(&here);
    bool found = s_want.want && here;
    portEXIT_CRITICAL(&s_ask_lock);
    return found;
}

bool muse_present_sharper_pending(void)
{
    portENTER_CRITICAL(&s_ask_lock);
    bool pending = s_guard && s_guard_shown && !s_sharper_got;
    portEXIT_CRITICAL(&s_ask_lock);
    return pending;
}

void muse_present_up(void)
{
    portENTER_CRITICAL(&s_ask_lock);
    s_up_seq++;
    portEXIT_CRITICAL(&s_ask_lock);
}

uint32_t muse_present_up_seq(void)
{
    portENTER_CRITICAL(&s_ask_lock);
    uint32_t up = s_up_seq;
    portEXIT_CRITICAL(&s_ask_lock);
    return up;
}
