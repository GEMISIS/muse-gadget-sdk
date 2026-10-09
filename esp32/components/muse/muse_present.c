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
 * decoder (as display.draw_url's are, main/image_fetch.c), or muse_jpeg.h's
 * for a progressive one, into two RGB565 copies in PSRAM: one fitting the
 * screen, for a tap to show full size, and one at the size Muse holds it
 * up (muse_ui_present). The same task asks Muse
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
#include <unistd.h>

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

#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "muse_chat.h"
#include "muse_img_url.h"
#include "muse_jpeg.h"
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
#define JPEG_KEEP_FREE (160 * 1024)     /* PSRAM a progressive JPEG's decoding leaves everyone else */
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

/*
 * What a JPEG the ROM's decoder refuses may take to decode: the largest free
 * block of PSRAM, less what's left to everyone else. Nothing without PSRAM:
 * no internal RAM goes on it.
 */
static size_t jpeg_budget(void)
{
#if CONFIG_SPIRAM
    size_t all = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t big = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    size_t room = all > JPEG_KEEP_FREE ? all - JPEG_KEEP_FREE : 0;
    return big < room ? big : room;
#else
    return 0;
#endif
}

/*
 * A progressive JPEG (or one otherwise not baseline) by muse_jpeg.h, fitting
 * w x h: as sharp as the screen shows if there's the memory for it, softer
 * if not, down to its DC coefficients alone (1/8).
 */
static bool own_jpeg(const char *label, const uint8_t *data, size_t len, int w, int h, muse_jpeg_t *img)
{
    int64_t t0 = esp_timer_get_time();
    size_t budget = jpeg_budget();
    char err[64];
    if (!muse_jpeg_decode(data, len, w, h, budget, 8, img, err, sizeof(err))) {
        ESP_LOGW(TAG, "\"%s\": JPEG: %s (%u KB of PSRAM to decode it in)", label, err, (unsigned)(budget / 1024));
        return false;
    }
    char how[8];
    snprintf(how, sizeof(how), "%d/8", img->eighths);
    ESP_LOGI(TAG, "\"%s\": %dx%d JPEG, decoded %s %s at %dx%d in %d ms (%u KB of %u KB)", label, img->src_w,
             img->src_h, img->progressive ? "progressive" : "sequential",
             img->eighths == 8 ? "full" : img->eighths == 1 ? "dc" : how, img->w, img->h, (int)ms_since(t0),
             (unsigned)(img->need / 1024), (unsigned)(budget / 1024));
    return true;
}

static bool show_own_jpeg(const job_t *job, int sw, int sh, int photo)
{
    muse_jpeg_info_t info;
    if (muse_jpeg_info(job->data, job->len, &info) && (info.w < TEST_PX || info.h < TEST_PX)) {
        ESP_LOGI(TAG, "\"%s\": %dx%d is a test, not the picture: not shown", job->label, info.w, info.h);
        return false;
    }
    muse_jpeg_t img;
    if (!own_jpeg(job->label, job->data, job->len, sw, sh, &img)) {
        return false;
    }
    return present_pixels(job, img.px, img.w, img.h, sw, sh, photo);
}

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
    if (rc == JDR_FMT3) {
        /* Progressive, as the web's often are, or not baseline some other way: ours. */
        heap_caps_free(j.out);
        return show_own_jpeg(job, sw, sh, photo);
    }
    if (rc != JDR_OK) {
        ESP_LOGW(TAG, "\"%s\": %s (%d)", job->label, rc == JDR_MEM1 ? "out of memory" : "not a valid JPEG", (int)rc);
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

static void fetching(bool on, size_t received, size_t size);
static void decoding(int n);

/*
 * Fast as it'll go within a 16 KB TCP window (~1 Mbit/s at ~125 ms, the
 * DMA heap's limit: cmake/validate_config.cmake):
 * - a CDN's ~640 px copy first (muse_img_url.h), the original if that fails;
 * - JPEG asked for over WebP (the ROM decodes it, at 1/2, 1/4 or 1/8);
 * - the last host's TLS session kept a minute (s_kept), so the next fetch
 *   from it, a redirect's or the original after its smaller copy, resumes
 *   it in a round trip instead of a full handshake; a redirect to the same
 *   host keeps its connection;
 * - a big one (PARALLEL_MIN, its server taking ranges) in up to three
 *   ranges at once, each its own connection and window, while the DMA heap
 *   has a window's room for each over the Noise channel's floor; the ranges
 *   go on short-lived tasks with PSRAM stacks (no flash, no NVS).
 * DNS, connecting (TCP and TLS), waiting and the body are logged apart.
 */
#define WEB_ACCEPT "image/jpeg,image/png;q=0.9,image/webp;q=0.5,*/*;q=0.1"
#define WEB_AGENT "Mozilla/5.0 (MuseGadget)"
#define WEB_TX 1536                     /* request buffer: over 1 KB, so in PSRAM too */
#define WEB_HOST_MAX 96
#define WEB_URL_MAX 640
#define WEB_REDIRECTS 4
#define WEB_KEEP_US (60 * 1000000LL)    /* a host's TLS session kept this long, for the next fetch */
#define PARALLEL_MIN (128 * 1024)       /* smaller, one connection is as quick: the others' handshakes cost more */
#define PARALLEL_MAX 3                  /* connections, the first among them */
#define PART_MIN (48 * 1024)
#define DMA_FLOOR (12 * 1024)           /* the Noise control channel's (noise_tx_has_control_headroom) */
#define DMA_PER_CONN (16 * 1024)        /* a connection's window, in flight in Wi-Fi's buffers */
#define PART_STACK (12 * 1024)          /* a range's task: TLS, in PSRAM */
#define PART_WAIT_MS 45000

typedef struct {
    bool ranges;             /* Accept-Ranges: bytes */
    int64_t connected_us;    /* HTTP_EVENT_ON_CONNECTED: TCP and TLS done */
} web_ev_t;

typedef struct {
    int dns_ms, tcp_ms, connect_ms, wait_ms, body_ms;   /* -1: not measured */
    int status, parts;
    bool resumed;            /* the kept session's host */
} web_times_t;

/* The last host fetched from, its client (and so its TLS session) kept a
 * while: the present task's own, so no lock. */
EXT_RAM_BSS_ATTR static struct {
    char host[WEB_HOST_MAX];
    esp_http_client_handle_t http;
    int64_t used_us;
} s_kept;

static esp_err_t web_event(esp_http_client_event_t *ev)
{
    web_ev_t *w = ev->user_data;
    if (!w) {
        return ESP_OK;
    }
    if (ev->event_id == HTTP_EVENT_ON_CONNECTED && !w->connected_us) {
        w->connected_us = esp_timer_get_time();
    } else if (ev->event_id == HTTP_EVENT_ON_HEADER && ev->header_key && ev->header_value
               && !strcasecmp(ev->header_key, "Accept-Ranges") && !strncasecmp(ev->header_value, "bytes", 5)) {
        w->ranges = true;
    }
    return ESP_OK;
}

static esp_http_client_handle_t web_client(const char *url)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = WEB_TIMEOUT_MS,
        .buffer_size = WEB_BUFFER,
        .buffer_size_tx = WEB_TX,
        .event_handler = web_event,
        .user_agent = WEB_AGENT,
#if CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS
        .save_client_session = true,   /* kept with the client, for its next connection */
#endif
    };
    esp_http_client_handle_t http = esp_http_client_init(&cfg);
    if (http) {
        esp_http_client_set_header(http, "Accept", WEB_ACCEPT);
    }
    return http;
}

/* The URL's host, lower case; false if there's none or it's too long. */
static bool host_of(const char *url, char *out, size_t cap)
{
    const char *p = strstr(url, "://");
    if (!p) {
        return false;
    }
    p += 3;
    size_t n = strcspn(p, "/?#:");
    if (!n || n >= cap) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        out[i] = (char)tolower((unsigned char)p[i]);
    }
    out[n] = '\0';
    return true;
}

/* The kept client, if it's for `host` and still fresh; else a new one. */
static esp_http_client_handle_t web_take(const char *url, const char *host, bool *resumed)
{
    *resumed = false;
    if (s_kept.http && !strcmp(s_kept.host, host) && esp_timer_get_time() - s_kept.used_us < WEB_KEEP_US) {
        esp_http_client_handle_t http = s_kept.http;
        s_kept.http = NULL;
        if (esp_http_client_set_url(http, url) == ESP_OK) {
            *resumed = true;
            return http;
        }
        esp_http_client_cleanup(http);
    }
    return web_client(url);
}

/* Done with it: closed, and kept for its host's next fetch in place of the last. */
static void web_give(esp_http_client_handle_t http, const char *host)
{
    esp_http_client_close(http);
    esp_http_client_set_user_data(http, NULL);
    if (s_kept.http) {
        esp_http_client_cleanup(s_kept.http);
    }
    s_kept.http = http;
    strlcpy(s_kept.host, host, sizeof(s_kept.host));
    s_kept.used_us = esp_timer_get_time();
}

/* The kept client let go once it's stale: no RAM held for a host not coming back. */
static void web_expire(void)
{
    if (s_kept.http && esp_timer_get_time() - s_kept.used_us >= WEB_KEEP_US) {
        esp_http_client_cleanup(s_kept.http);
        s_kept.http = NULL;
    }
}

/* Opens the request and reads its headers, following redirects (a same-host
 * one on the same connection). Its length in *len (-1: not said). */
static esp_err_t web_open(esp_http_client_handle_t http, int64_t *len, int *status)
{
    for (int i = 0;; i++) {
        esp_err_t err = esp_http_client_open(http, 0);
        if (err != ESP_OK && i > 0) {
            esp_http_client_close(http);   /* the server closed the kept connection: a fresh one */
            err = esp_http_client_open(http, 0);
        }
        if (err != ESP_OK) {
            return err;
        }
        int64_t got = esp_http_client_fetch_headers(http);
        *status = esp_http_client_get_status_code(http);
        bool chunked = esp_http_client_is_chunked_response(http);
        *len = chunked || got <= 0 ? -1 : got;
        bool redirect = *status == 301 || *status == 302 || *status == 303 || *status == 307 || *status == 308;
        if (got < 0 && !chunked) {
            return ESP_FAIL;
        }
        if (!redirect || i == WEB_REDIRECTS) {
            return ESP_OK;
        }
        esp_http_client_flush_response(http, NULL);
        if (esp_http_client_set_redirection(http) != ESP_OK) {
            return ESP_FAIL;
        }
    }
}

/* Reads n bytes into dst (*got counting them); true if all came. */
static bool web_body(esp_http_client_handle_t http, uint8_t *dst, size_t n, volatile size_t *got)
{
    while (*got < n) {
        size_t want = n - *got < WEB_BUFFER ? n - *got : WEB_BUFFER;
        int r = esp_http_client_read(http, (char *)dst + *got, (int)want);
        if (r <= 0) {
            return false;
        }
        *got += (size_t)r;
    }
    return true;
}

/* GETs bytes [from, to) of the URL the client's at into buf + from. */
static bool web_range(esp_http_client_handle_t http, uint8_t *buf, size_t from, size_t to, volatile size_t *got)
{
    char range[48];
    snprintf(range, sizeof(range), "bytes=%u-%u", (unsigned)from, (unsigned)(to - 1));
    esp_http_client_set_header(http, "Range", range);
    int64_t len;
    int status;
    *got = 0;
    bool ok = web_open(http, &len, &status) == ESP_OK && status == 206 && len == (int64_t)(to - from)
              && web_body(http, buf + from, to - from, got);
    esp_http_client_delete_header(http, "Range");
    esp_http_client_close(http);
    return ok;
}

typedef struct {
    char url[WEB_URL_MAX];
    uint8_t *buf;
    size_t from, to;
    volatile size_t got;
    volatile bool done, ok;
    int ms;
    TaskHandle_t waiter, task;
} part_t;

/* A range on a connection of its own; then it waits to be deleted. */
static void part_task(void *arg)
{
    part_t *p = arg;
    int64_t t0 = esp_timer_get_time();
    web_ev_t ev = { 0 };
    esp_http_client_handle_t http = web_client(p->url);
    if (http) {
        esp_http_client_set_user_data(http, &ev);
        p->ok = web_range(http, p->buf, p->from, p->to, &p->got);
        esp_http_client_cleanup(http);
    }
    p->ms = (int)ms_since(t0);
    p->done = true;
    xTaskNotifyGive(p->waiter);
    vTaskSuspend(NULL);
}

/* How many connections for a body of `len`: more only while the DMA heap
 * has a window's room for each over the control channel's floor. */
static int parts_for(int64_t len, bool ranges)
{
    if (!ranges || len < PARALLEL_MIN) {
        return 1;
    }
    size_t dma = heap_caps_get_free_size(MALLOC_CAP_DMA);
    int n = 1 + (dma > DMA_FLOOR ? (int)((dma - DMA_FLOOR) / DMA_PER_CONN) : 0);
    n = n < PARALLEL_MAX ? n : PARALLEL_MAX;
    while (n > 1 && len / n < PART_MIN) {
        n--;
    }
    return n;
}

static size_t parts_got(part_t *const *parts, int n)
{
    size_t sum = 0;
    for (int i = 0; i < n; i++) {
        sum += parts[i] ? parts[i]->got : 0;
    }
    return sum;
}

/*
 * The body, `len` bytes, into buf: the first part on `http` (open, its
 * headers read), the rest in ranges on their own tasks; a range that
 * fails is fetched again here. True if all of it came.
 */
static bool web_parallel(esp_http_client_handle_t http, uint8_t *buf, size_t len, int n, web_times_t *t)
{
    part_t *parts[PARALLEL_MAX] = { 0 };
    char *url = n > 1 ? heap_caps_malloc(WEB_URL_MAX, MUSE_BIG_CAPS) : NULL;
    if (!url || esp_http_client_get_url(http, url, WEB_URL_MAX) != ESP_OK) {
        n = 1;   /* the URL the redirects led to, for the ranges */
    }
    size_t cut = len / n;
    int started = 1;
    for (int i = 1; i < n; i++) {
        part_t *p = heap_caps_calloc(1, sizeof(*p), MUSE_BIG_CAPS);
        if (!p) {
            break;
        }
        strlcpy(p->url, url, sizeof(p->url));
        p->buf = buf;
        p->from = cut * i;
        p->to = i == n - 1 ? len : cut * (i + 1);
        p->waiter = xTaskGetCurrentTaskHandle();
        if (xTaskCreatePinnedToCoreWithCaps(part_task, "muse_part", PART_STACK, p, TASK_PRIORITY, &p->task, 0,
                                            MUSE_BIG_CAPS) != pdPASS) {
            heap_caps_free(p);
            break;
        }
        parts[i] = p;
        started++;
    }
    heap_caps_free(url);
    /* Ours: up to the first range taken, or the whole if none was. */
    size_t ours = started > 1 ? parts[1]->from : len;
    volatile size_t got = 0;
    bool ok = true;
    while (ok && got < ours) {
        size_t want = ours - got < WEB_BUFFER ? ours - got : WEB_BUFFER;
        int r = esp_http_client_read(http, (char *)buf + got, (int)want);
        ok = r > 0;
        got += r > 0 ? (size_t)r : 0;
        fetching(true, got + parts_got(parts, PARALLEL_MAX), len);
    }
    esp_http_client_close(http);   /* the rest is the ranges' */
    /* Then the ranges', and any that failed again on ours. */
    int64_t until = esp_timer_get_time() + PART_WAIT_MS * 1000LL;
    for (int i = 1; i < PARALLEL_MAX; i++) {
        part_t *p = parts[i];
        while (p && !p->done && esp_timer_get_time() < until) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
            fetching(true, got + parts_got(parts, PARALLEL_MAX), len);
        }
    }
    for (int i = 1; i < PARALLEL_MAX; i++) {
        part_t *p = parts[i];
        if (!p) {
            continue;
        }
        if (!p->done) {
            /* Stuck past all its timeouts: left (and its memory) rather than freed under it. */
            ESP_LOGW(TAG, "range %u-%u never finished: left", (unsigned)p->from, (unsigned)p->to);
            ok = false;
            continue;
        }
        while (eTaskGetState(p->task) != eSuspended) {
            vTaskDelay(1);
        }
        vTaskDeleteWithCaps(p->task);
        ESP_LOGI(TAG, "range %u-%u: %s in %d ms", (unsigned)p->from, (unsigned)p->to, p->ok ? "fetched" : "failed",
                 p->ms);
        if (!p->ok && ok) {
            volatile size_t again = 0;
            ok = web_range(http, buf, p->from, p->to, &again);
        }
        heap_caps_free(p);
    }
    t->parts = started;
    return ok;
}

/*
 * GETs a web image into *data (PSRAM), timing each phase. False with
 * *data NULL if it didn't come whole as a 200 (or is over WEB_MAX: *too_big).
 */
static bool web_get(const char *url, uint8_t **data, size_t *size, bool *too_big, web_times_t *t)
{
    *data = NULL;
    *size = 0;
    *too_big = false;
    memset(t, 0, sizeof(*t));
    t->dns_ms = t->connect_ms = t->wait_ms = t->body_ms = -1;
    char host[WEB_HOST_MAX];
    if (!host_of(url, host, sizeof(host))) {
        return false;
    }
    /* DNS apart: lwIP keeps the answer, so the client's own lookup is instant. */
    int64_t t0 = esp_timer_get_time();
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) == 0) {
        t->dns_ms = (int)ms_since(t0);
        freeaddrinfo(res);
    }
    web_ev_t ev = { 0 };
    esp_http_client_handle_t http = web_take(url, host, &t->resumed);
    if (!http) {
        return false;
    }
    esp_http_client_set_user_data(http, &ev);
    int64_t t1 = esp_timer_get_time(), len = -1;
    esp_err_t err = web_open(http, &len, &t->status);
    int64_t t2 = esp_timer_get_time();
    t->connect_ms = ev.connected_us ? (int)((ev.connected_us - t1) / 1000) : 0;
    t->wait_ms = (int)((t2 - (ev.connected_us ? ev.connected_us : t1)) / 1000);
    bool ok = err == ESP_OK && t->status == 200;
    if (ok && len > WEB_MAX) {
        *too_big = true;
        ok = false;
    }
    uint8_t *buf = NULL;
    size_t got = 0;
    if (ok && len > 0) {
        fetching(true, 0, (size_t)len);   /* connected: the boxes start coming */
        buf = heap_caps_malloc((size_t)len, MUSE_BIG_CAPS);
        int n = parts_for(len, ev.ranges);
        ok = buf && web_parallel(http, buf, (size_t)len, n, t);
        got = ok ? (size_t)len : 0;
    } else if (ok) {
        /* Its length not said (chunked): grown as it comes. */
        fetching(true, 0, 0);
        size_t cap = 0;
        t->parts = 1;
        for (;;) {
            if (got == cap) {
                size_t grown_cap = cap ? cap * 2 : 64 * 1024;
                uint8_t *grown = grown_cap <= WEB_MAX ? heap_caps_realloc(buf, grown_cap, MUSE_BIG_CAPS) : NULL;
                if (!grown) {
                    *too_big = grown_cap > WEB_MAX;
                    ok = false;
                    break;
                }
                buf = grown;
                cap = grown_cap;
            }
            int r = esp_http_client_read(http, (char *)buf + got, (int)(cap - got));
            if (r < 0) {
                ok = false;
            }
            if (r <= 0) {
                break;
            }
            got += (size_t)r;
            fetching(true, got, 0);
        }
        ok = ok && got > 0 && esp_http_client_is_complete_data_received(http);
    }
    t->body_ms = (int)ms_since(t2);
    web_give(http, host);
    if (!ok) {
        heap_caps_free(buf);
        return false;
    }
    *data = buf;
    *size = got;
    return true;
}

static void web_log(const char *label, const char *what, const web_times_t *t, size_t len, int total_ms)
{
    int kbps = t->body_ms > 0 ? (int)((uint64_t)len * 8 / (unsigned)t->body_ms) : 0;
    ESP_LOGI(TAG, "\"%s\": %s: HTTP %d, %u bytes in %d ms: dns %d, connect %d (tcp+tls%s), wait %d, body %d ms "
             "at %d kbit/s over %d connection%s", label, what, t->status, (unsigned)len, total_ms, t->dns_ms,
             t->connect_ms, t->resumed ? ", session kept" : "", t->wait_ms, t->body_ms, kbps, t->parts,
             t->parts == 1 ? "" : "s");
}

/*
 * A public web image (an https URL Muse wrote into a reply) fetched straight
 * here: a second or two, not the ~20 s of Muse pushing a copy. No token or
 * cookie goes with it. The CDN's smaller copy first, the URL as given if
 * that can't be fetched or decoded. True if it was shown; else Muse is asked
 * after all (too big, a format not shown here, refused, out of reach).
 */
static bool show_from_web(const char *url, const char *label)
{
    int64_t t0 = esp_timer_get_time();
    char *small = heap_caps_malloc(WEB_URL_MAX, MUSE_BIG_CAPS);
    bool smaller = small && muse_img_url_smaller(url, MUSE_IMG_URL_PX, small, WEB_URL_MAX);
    bool shown = false;
    for (int i = smaller ? 0 : 1; i < 2 && !shown; i++) {
        const char *u = i == 0 ? small : url;
        uint8_t *data = NULL;
        size_t len = 0;
        bool too_big = false;
        web_times_t t;
        int64_t ti = esp_timer_get_time();
        bool got = web_get(u, &data, &len, &too_big, &t) && can_show(data, len);
        web_log(label, i == 0 ? "its smaller copy" : "fetched here", &t, len, (int)ms_since(ti));
        fetching(false, 0, 0);
        if (!got) {
            ESP_LOGW(TAG, "\"%s\": %.80s: %s", label, u,
                     too_big ? "too big" : data ? "a format not shown here" : "couldn't fetch it");
            heap_caps_free(data);
            continue;
        }
        decoding(1);
        job_t job = { .data = data, .len = len };
        strlcpy(job.label, label, sizeof(job.label));
        shown = run(&job);
        decoding(-1);
        heap_caps_free(data);
        if (!shown && i == 0) {
            ESP_LOGW(TAG, "\"%s\": its smaller copy wasn't shown: the original, then", label);
        }
    }
    heap_caps_free(small);
    if (!shown) {
        ESP_LOGW(TAG, "\"%s\": not shown from here in %d ms: asking Muse", label, (int)ms_since(t0));
        return false;
    }
    ESP_LOGI(TAG, "\"%s\": shown %d ms after asking", label, (int)ms_since(t0));
    return true;
}

/* ---- >fetch= (muse_present_bench_fetch) ---------------------------------- */

/* TCP alone, to the URL's host on 443 (or 80): the connect's round trip, for the bench. */
static int tcp_ms(const char *url)
{
    char host[WEB_HOST_MAX];
    if (!host_of(url, host, sizeof(host))) {
        return -1;
    }
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res = NULL;
    if (getaddrinfo(host, strncmp(url, "https", 5) ? "80" : "443", &hints, &res) != 0 || !res) {
        return -1;
    }
    int ms = -1, fd = socket(res->ai_family, res->ai_socktype, 0);
    if (fd >= 0) {
        struct timeval tv = { .tv_sec = 5 };
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        int64_t t0 = esp_timer_get_time();
        if (connect(fd, res->ai_addr, res->ai_addrlen) == 0) {
            ms = (int)ms_since(t0);
        }
        close(fd);
    }
    freeaddrinfo(res);
    return ms;
}

static void bench_fetch(void *arg)
{
    char *url = arg;
    int64_t t0 = esp_timer_get_time();
    char *small = heap_caps_malloc(WEB_URL_MAX, MUSE_BIG_CAPS);
    bool smaller = small && muse_img_url_smaller(url, MUSE_IMG_URL_PX, small, WEB_URL_MAX);
    const char *u = smaller ? small : url;
    int tcp = tcp_ms(u);
    uint8_t *data = NULL;
    size_t len = 0;
    bool too_big = false, got = false, shown = false;
    web_times_t t;
    const char *kind = "none";
    int fetch_ms = 0, show_ms = 0;
    /* As show_from_web: the original after its smaller copy, if that can't be fetched or shown. */
    for (int i = smaller ? 0 : 1; i < 2 && !shown; i++) {
        u = i == 0 ? small : url;
        heap_caps_free(data);
        data = NULL;
        int64_t tf = esp_timer_get_time();
        got = web_get(u, &data, &len, &too_big, &t);
        fetch_ms += (int)ms_since(tf);
        fetching(false, 0, 0);
        kind = !got ? "none" : len > 3 && data[0] == 0xFF && data[1] == 0xD8 ? "JPEG"
#if CONFIG_MUSE_PRESENT_FORMATS
               : muse_image_kind_name(muse_image_kind(data, len));
#else
               : "other";
#endif
        if (got && can_show(data, len)) {
            int64_t t1 = esp_timer_get_time();
            job_t job = { .data = data, .len = len };
            strlcpy(job.label, "fetch", sizeof(job.label));
            decoding(1);
            shown = run(&job);
            decoding(-1);
            show_ms += (int)ms_since(t1);
            if (shown) {
                atomic_fetch_add(&s_seq, 1);   /* the face has it */
            }
        }
    }
    printf("@fetch {\"ok\":%s,\"shown\":%s,\"smaller\":%s,\"status\":%d,\"bytes\":%u,\"kind\":\"%s\",\"dns_ms\":%d,"
           "\"tcp_ms\":%d,\"connect_ms\":%d,\"tls_ms\":%d,\"resumed\":%s,\"wait_ms\":%d,\"body_ms\":%d,"
           "\"parts\":%d,\"fetch_ms\":%d,\"decode_show_ms\":%d,\"total_ms\":%d,\"dma_free\":%u}\n",
           got ? "true" : "false", shown ? "true" : "false", u != url ? "true" : "false", t.status, (unsigned)len,
           kind, t.dns_ms, tcp, t.connect_ms, tcp >= 0 && t.connect_ms > tcp ? t.connect_ms - tcp : -1,
           t.resumed ? "true" : "false", t.wait_ms, t.body_ms, t.parts, fetch_ms, show_ms, (int)ms_since(t0),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));
    fflush(stdout);
    heap_caps_free(data);
    heap_caps_free(small);
    heap_caps_free(url);
}

bool muse_present_bench_fetch(const char *url)
{
    if (!url || (strncmp(url, "https://", 8) && strncmp(url, "http://", 7)) || strlen(url) >= WEB_URL_MAX) {
        return false;
    }
    char *copy = heap_caps_malloc(strlen(url) + 1, MUSE_BIG_CAPS);
    if (!copy) {
        return false;
    }
    strcpy(copy, url);
    if (!muse_present_call(bench_fetch, copy)) {
        heap_caps_free(copy);
        return false;
    }
    return true;
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
    char msg[2 * PATH_MAX_LEN + LABEL_MAX + 1024];
    uint32_t hash = path_hash(path);
    unquote(path);
    unquote(label);
    /* Small: every byte goes as base64 Muse writes out, about 35 s per
     * 14K characters, so a 200 px JPEG under 12 KB (one chunk) shows in
     * well under a minute. */
    int n = 0;   /* nothing ahead of it: an earlier one's chat goes later (muse_present_stale_take) */
    /* A file in Muse's workspace (generated), or an image on the web it fetches first. */
    bool web = !strncmp(path, "http://", 7) || !strncmp(path, "https://", 8);
    /* The command itself, ready to run: working out how to scale and encode it
     * took Muse minutes, the push then timing out on the gadget's side. A web
     * image (one the gadget couldn't fetch itself) is downloaded first. */
#define ENCODE_PY "python3 -c \"import base64,io,sys;from PIL import Image;" \
                  "im=Image.open(sys.argv[1]).convert('RGB');im.thumbnail((240,240));b=io.BytesIO();" \
                  "im.save(b,'JPEG',quality=70);print(base64.b64encode(b.getvalue()).decode())\""
    char cmd[2 * PATH_MAX_LEN + 400];
    if (web) {
        snprintf(cmd, sizeof(cmd), "curl -sL -o /tmp/gadget_img \"%s\" && " ENCODE_PY " /tmp/gadget_img", path);
    } else {
        snprintf(cmd, sizeof(cmd), ENCODE_PY " \"%s\"", path);
    }
    snprintf(msg + n, sizeof(msg) - n,
             "Send the image \"%s\" to this gadget, quickly: run exactly this, then pass its whole output "
             "(a 240 px JPEG, under 14 KB) as data_b64 in one display.show_image call (offset 0, final=true, "
             "label \"%s\"):\n%s\nNothing else first. If it arrives damaged, run it again. No test images. "
             "Don't create links. Reply with just: sent.",
             label, label, cmd);
#undef ENCODE_PY
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
        TickType_t wait = ask_pending() ? pdMS_TO_TICKS(ASK_POLL_MS)
                          : s_kept.http ? pdMS_TO_TICKS(WEB_KEEP_US / 4000) : portMAX_DELAY;
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
        web_expire();
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
        if (rc == JDR_FMT3) {
            heap_caps_free(j.out);
            muse_jpeg_t img;
            if (!own_jpeg("decoding", data, len, fit_w, fit_h, &img)) {
                return false;
            }
            *px = img.px;
            *w = img.w;
            *h = img.h;
            return true;
        }
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
    /* One push is asked for now, sharp straight away (a preview first saved
     * little): none to hold the photo up waiting for. A bigger one that comes
     * all the same still takes its place (late_sharper, job->sharper). */
    return false;
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
