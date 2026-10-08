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
 * A chat reply's image (muse_present.h): fetched over HTTPS if the chat
 * session couldn't get it on its own connection, then decoded with the ROM's
 * JPEG decoder (as display.draw_url's are, main/image_fetch.c) into two
 * RGB565 copies in PSRAM: one fitting the screen, for a tap to show full
 * size, and one at the size Muse holds it up (muse_ui_present). It all runs
 * on one task with its stack in PSRAM, which never touches flash or NVS: the
 * microSD copy is saved by the extras task (muse_sd_queue_image).
 */
#include "muse_present.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "rom/tjpgd.h"

#include "muse_mem.h"
#include "muse_sd.h"
#include "muse_ui.h"

static const char *TAG = "muse_present";

#define TASK_STACK (20 * 1024)          /* in PSRAM: TLS for the HTTPS fetch, then the decoder */
#define TASK_PRIORITY 3                 /* under the UI (5), the chat session (5) and draw_url's (4) */
#define JOBS 2                          /* the one being shown, and the next */
#define HTTP_TIMEOUT_MS 10000           /* per socket operation */
#define HTTP_DEADLINE_US (30 * 1000000LL)
#define HTTP_MAX_REDIRECTS 3
#define FIRST_CAP (64 * 1024)           /* the buffer's start without a length to go on */
#define JPEG_POOL_BYTES 3100            /* the ROM decoder's work pool, as its documentation asks */
#define DECODED_MAX (3 * 1024 * 1024)   /* RGB565 out of the decoder, before resizing */
#define LABEL_MAX 48

typedef struct {
    uint8_t *data;          /* the image, or NULL to fetch it from url */
    size_t len;
    char *url;
    char *token;
    size_t byte_len;
    char label[LABEL_MAX];
} job_t;

static QueueHandle_t s_jobs;   /* job_t * */
static atomic_int s_started;   /* 0 not yet, 1 starting, 2 running */

static void job_free(job_t *job)
{
    if (!job) {
        return;
    }
    heap_caps_free(job->data);
    heap_caps_free(job->url);
    if (job->token) {
        memset(job->token, 0, strlen(job->token));
    }
    heap_caps_free(job->token);
    heap_caps_free(job);
}

/* In PSRAM: internal RAM is short, and these only wait on the queue. */
static char *big_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *copy = heap_caps_malloc(n, MUSE_BIG_CAPS);
    if (copy) {
        memcpy(copy, s, n);
    }
    return copy;
}

static int64_t ms_since(int64_t t0)
{
    return (esp_timer_get_time() - t0) / 1000;
}

/* ---- HTTPS -------------------------------------------------------------- */

static bool redirected(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

/* GETs url whole into PSRAM. NULL on failure, with *status the HTTP status (0 if none). */
static uint8_t *https_get(const job_t *job, size_t *out_len, int *status)
{
    *status = 0;
    size_t token_len = job->token ? strlen(job->token) : 0;
    esp_http_client_config_t cfg = {
        .url = job->url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 4096,
        .buffer_size_tx = 1024 + (int)token_len,   /* the bearer header goes in it whole */
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t http = esp_http_client_init(&cfg);
    if (!http) {
        return NULL;
    }
    char *bearer = NULL;
    if (token_len) {
        bearer = heap_caps_malloc(token_len + 8, MUSE_BIG_CAPS);
        if (bearer) {
            snprintf(bearer, token_len + 8, "Bearer %s", job->token);
            esp_http_client_set_header(http, "Authorization", bearer);
        }
    }
    esp_http_client_set_header(http, "Accept", "image/jpeg,image/*");
    int64_t deadline = esp_timer_get_time() + HTTP_DEADLINE_US;
    uint8_t *buf = NULL;
    size_t len = 0, cap = 0;
    bool ok = false;
    int64_t content_len = 0;
    for (int i = 0;; i++) {
        if (esp_http_client_open(http, 0) != ESP_OK) {
            break;
        }
        content_len = esp_http_client_fetch_headers(http);
        *status = esp_http_client_get_status_code(http);
        if (!redirected(*status) || i == HTTP_MAX_REDIRECTS) {
            break;
        }
        /* Wherever it leads, the VM's token stays with the VM. */
        esp_http_client_delete_header(http, "Authorization");
        bool moved = esp_http_client_set_redirection(http) == ESP_OK;
        esp_http_client_close(http);
        if (!moved) {
            *status = 0;
            break;
        }
    }
    if (*status == 200) {
        cap = content_len > 0 ? (size_t)content_len : job->byte_len ? job->byte_len : FIRST_CAP;
        cap = cap < MUSE_PRESENT_MAX ? cap : MUSE_PRESENT_MAX;
        buf = heap_caps_malloc(cap, MUSE_BIG_CAPS);
        while (buf && esp_timer_get_time() < deadline) {
            if (len == cap) {
                if (cap == MUSE_PRESENT_MAX) {
                    ESP_LOGW(TAG, "image over %u KB: dropped", MUSE_PRESENT_MAX / 1024);
                    break;
                }
                size_t grown_cap = cap * 2 < MUSE_PRESENT_MAX ? cap * 2 : MUSE_PRESENT_MAX;
                uint8_t *grown = heap_caps_realloc(buf, grown_cap, MUSE_BIG_CAPS);
                if (!grown) {
                    break;
                }
                buf = grown;
                cap = grown_cap;
            }
            int n = esp_http_client_read(http, (char *)buf + len, (int)(cap - len));
            if (n < 0) {
                break;
            }
            if (n == 0) {
                ok = esp_http_client_is_complete_data_received(http) && len > 0;
                break;
            }
            len += (size_t)n;
        }
    }
    esp_http_client_cleanup(http);
    if (bearer) {
        memset(bearer, 0, token_len + 8);
        heap_caps_free(bearer);
    }
    if (!ok) {
        heap_caps_free(buf);
        return NULL;
    }
    *out_len = len;
    return buf;
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

/* ---- Task --------------------------------------------------------------- */

static void run(job_t *job)
{
    if (!job->data) {
        int64_t t0 = esp_timer_get_time();
        int status;
        job->data = https_get(job, &job->len, &status);
        if (!job->data) {
            ESP_LOGW(TAG, "\"%s\": HTTPS fetch failed (HTTP %d) after %d ms", job->label, status,
                     (int)ms_since(t0));
            return;
        }
        ESP_LOGI(TAG, "\"%s\": fetched over HTTPS (the fallback): %u bytes in %d ms", job->label,
                 (unsigned)job->len, (int)ms_since(t0));
    }
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
        job_t *job;
        if (xQueueReceive(s_jobs, &job, portMAX_DELAY) == pdTRUE) {
            run(job);
            job_free(job);
            ESP_LOGI(TAG, "stack %u free", (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
    }
}

static bool start(void)
{
    int expected = 0;
    if (atomic_compare_exchange_strong(&s_started, &expected, 1)) {
        s_jobs = xQueueCreateWithCaps(JOBS, sizeof(job_t *), MALLOC_CAP_SPIRAM);
        if (!s_jobs || xTaskCreatePinnedToCoreWithCaps(present_task, "muse_present", TASK_STACK, NULL,
                                                       TASK_PRIORITY, NULL, 1, MUSE_BIG_CAPS) != pdPASS) {
            ESP_LOGE(TAG, "start failed: images in replies won't show");
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

static void submit(job_t *job, const char *label)
{
    strlcpy(job->label, label && label[0] ? label : "image", sizeof(job->label));
    if (!start() || xQueueSend(s_jobs, &job, 0) != pdTRUE) {
        ESP_LOGW(TAG, "\"%s\": busy with others, dropped", job->label);
        job_free(job);
    }
}

void muse_present_bytes(uint8_t *data, size_t len, const char *label)
{
    job_t *job = heap_caps_calloc(1, sizeof(*job), MUSE_BIG_CAPS);
    if (!job) {
        heap_caps_free(data);
        return;
    }
    job->data = data;
    job->len = len;
    submit(job, label);
}

void muse_present_fetch(const char *url, const char *token, const char *label, size_t byte_len)
{
    job_t *job = heap_caps_calloc(1, sizeof(*job), MUSE_BIG_CAPS);
    if (!job) {
        return;
    }
    job->url = big_strdup(url);
    job->token = token && token[0] ? big_strdup(token) : NULL;
    job->byte_len = byte_len < MUSE_PRESENT_MAX ? byte_len : MUSE_PRESENT_MAX;
    if (!job->url || (token && token[0] && !job->token)) {
        job_free(job);
        return;
    }
    submit(job, label);
}
