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
 * microSD on the Waveshare 2.16 (muse_sd.h): SDMMC, 1-bit, CLK 2, CMD 1,
 * D0 3, with D3/CS on 41 and no card detect (the schematic's SD-CARD block;
 * the BSP's BSP_SD_* pins). The card shares the 3.3 V rail (DCDC1) with the
 * chip, so it's powered whenever the board is.
 */
#include "muse_sd.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdmmc_cmd.h"

#include "muse_chat.h"
#include "muse_extras.h"
#include "muse_mem.h"
#include "muse_state.h"

static const char *TAG = "sd";

#define SD_CLK GPIO_NUM_2
#define SD_CMD GPIO_NUM_1
#define SD_D0 GPIO_NUM_3
#define SD_D3 GPIO_NUM_41       /* held high: a card that sees it low at reset goes into SPI mode */

#define IMAGES "images"
#define TMP_IMAGE MUSE_SD_DIR "/" IMAGES "/partial.tmp"
#define TEE_BUF 4096            /* batches the download's small reads into card writes */

static bool s_ready;
static sdmmc_card_t *s_card;

#if CONFIG_MUSE_GADGET_CAPTION_LOG
static void caption_init(void);
#endif

void muse_sd_mount(void)
{
    gpio_config_t d3 = {
        .pin_bit_mask = BIT64(SD_D3),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&d3);

    const esp_vfs_fat_mount_config_t mount = {
        .format_if_mount_failed = false,   /* never wipe a card that didn't mount */
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = {
        .clk = SD_CLK,
        .cmd = SD_CMD,
        .d0 = SD_D0,
        .d1 = GPIO_NUM_NC,
        .d2 = GPIO_NUM_NC,
        .d3 = GPIO_NUM_NC,
        .d4 = GPIO_NUM_NC,
        .d5 = GPIO_NUM_NC,
        .d6 = GPIO_NUM_NC,
        .d7 = GPIO_NUM_NC,
        .cd = SDMMC_SLOT_NO_CD,
        .wp = SDMMC_SLOT_NO_WP,
        .width = 1,
        .flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP,   /* besides the board's 10k */
    };
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_vfs_fat_sdmmc_mount(MUSE_SD_ROOT, &host, &slot, &mount, &s_card);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no microSD card (%s, %d ms): not saving images or captions",
                 esp_err_to_name(err), (int)((esp_timer_get_time() - t0) / 1000));
        return;
    }
    s_ready = true;
    ESP_LOGI(TAG, "microSD %s: %llu MB at " MUSE_SD_ROOT ", saving images %s", s_card->cid.name,
             (unsigned long long)s_card->csd.capacity * s_card->csd.sector_size / (1024 * 1024),
             muse_sd_save_images() ? "on" : "off");
    muse_sd_mkdirs(IMAGES);
    unlink(TMP_IMAGE);   /* left by a download cut short by a reset */
#if CONFIG_MUSE_GADGET_CAPTION_LOG
    caption_init();
#endif
}

bool muse_sd_ready(void)
{
    return s_ready;
}

bool muse_sd_mkdirs(const char *rel)
{
    if (!s_ready) {
        return false;
    }
    char path[128];
    int n = snprintf(path, sizeof(path), MUSE_SD_DIR "/%s", rel);
    if (n <= 0 || n >= (int)sizeof(path)) {
        return false;
    }
    for (char *p = path + strlen(MUSE_SD_ROOT) + 1;; p++) {
        if (*p == '/' || *p == '\0') {
            char c = *p;
            *p = '\0';
            if (mkdir(path, 0775) != 0 && errno != EEXIST) {
                ESP_LOGW(TAG, "mkdir %s: %s", path, strerror(errno));
                return false;
            }
            if (!c) {
                return true;
            }
            *p = c;
        }
    }
}

FILE *muse_sd_fopen(const char *rel, const char *mode)
{
    if (!s_ready) {
        return NULL;
    }
    const char *slash = strrchr(rel, '/');
    if (slash) {
        char dir[96];
        if (slash - rel >= (int)sizeof(dir)) {
            return NULL;
        }
        memcpy(dir, rel, slash - rel);
        dir[slash - rel] = '\0';
        if (!muse_sd_mkdirs(dir)) {
            return NULL;
        }
    } else if (!muse_sd_mkdirs("")) {
        return NULL;
    }
    char path[128];
    snprintf(path, sizeof(path), MUSE_SD_DIR "/%s", rel);
    return fopen(path, mode);
}

bool muse_sd_save_images(void)
{
    return muse_extras_get_i32("sd_save_images", CONFIG_MUSE_GADGET_SD_SAVE_IMAGES) != 0;
}

void muse_sd_set_save_images(bool on)
{
    muse_extras_set_i32("sd_save_images", on);
}

/* ---- Saving downloads as they stream ------------------------------------- */

struct muse_sd_tee {
    int fd;
    bool failed;
    size_t used, total;
    uint8_t buf[TEE_BUF];
};

muse_sd_tee_t *muse_sd_tee_begin(void)
{
    if (!s_ready || !muse_sd_save_images() || !muse_sd_mkdirs(IMAGES)) {
        return NULL;
    }
    muse_sd_tee_t *t = heap_caps_malloc(sizeof(*t), MUSE_BIG_CAPS);
    if (!t) {
        return NULL;
    }
    t->fd = open(TMP_IMAGE, O_WRONLY | O_CREAT | O_TRUNC, 0664);
    if (t->fd < 0) {
        ESP_LOGW(TAG, "can't save the image: %s", strerror(errno));
        heap_caps_free(t);
        return NULL;
    }
    t->failed = false;
    t->used = t->total = 0;
    return t;
}

static void tee_flush(muse_sd_tee_t *t)
{
    if (t->used && !t->failed && write(t->fd, t->buf, t->used) != (ssize_t)t->used) {
        ESP_LOGW(TAG, "image write failed (%s): not saving it", strerror(errno));
        t->failed = true;
    }
    t->used = 0;
}

void muse_sd_tee_write(muse_sd_tee_t *t, const void *data, size_t len)
{
    if (!t || t->failed) {
        return;
    }
    const uint8_t *p = data;
    t->total += len;
    while (len) {
        size_t n = TEE_BUF - t->used < len ? TEE_BUF - t->used : len;
        memcpy(t->buf + t->used, p, n);
        t->used += n;
        p += n;
        len -= n;
        if (t->used == TEE_BUF) {
            tee_flush(t);
        }
    }
}

/* images/<YYYYMMDD-HHMMSS>[-WxH][-N].<ext>, or img-NNNNN when the clock isn't set. */
static bool image_name(char *out, size_t cap, const char *ext, int w, int h)
{
    char stem[48], size[24] = "";
    if (w > 0 && h > 0) {
        snprintf(size, sizeof(size), "-%dx%d", w, h);
    }
    struct stat st;
    if (muse_time_format(stem, sizeof(stem), "%Y%m%d-%H%M%S")) {
        for (int n = 1; n < 100; n++) {
            char dup[8] = "";
            if (n > 1) {
                snprintf(dup, sizeof(dup), "-%d", n);
            }
            snprintf(out, cap, MUSE_SD_DIR "/" IMAGES "/%s%s%s.%s", stem, size, dup, ext);
            if (stat(out, &st) != 0) {
                return true;
            }
        }
        return false;
    }
    for (int n = 1; n < 100000; n++) {
        snprintf(out, cap, MUSE_SD_DIR "/" IMAGES "/img-%05d%s.%s", n, size, ext);
        if (stat(out, &st) != 0) {
            return true;
        }
    }
    return false;
}

void muse_sd_tee_end(muse_sd_tee_t *t, bool ok, const char *ext, int w, int h)
{
    if (!t) {
        return;
    }
    tee_flush(t);
    bool synced = fsync(t->fd) == 0;
    close(t->fd);
    char path[128];
    if (ok && synced && !t->failed && t->total
        && image_name(path, sizeof(path), ext, w, h) && rename(TMP_IMAGE, path) == 0) {
        ESP_LOGI(TAG, "saved %s (%u bytes)", path + strlen(MUSE_SD_ROOT) + 1, (unsigned)t->total);
    } else {
        unlink(TMP_IMAGE);
    }
    heap_caps_free(t);
}

/* ---- Whole images, saved by the extras task -------------------------------- */

static portMUX_TYPE s_queued_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t *s_queued;
static size_t s_queued_len;
static char s_queued_ext[4];

bool muse_sd_queue_image(const void *data, size_t len, const char *ext)
{
    if (!s_ready || !data || !len) {
        return false;
    }
    uint8_t *copy = heap_caps_malloc(len, MUSE_BIG_CAPS);
    if (!copy) {
        return false;
    }
    memcpy(copy, data, len);
    bool taken = false;
    portENTER_CRITICAL(&s_queued_lock);
    if (!s_queued) {
        s_queued = copy;
        s_queued_len = len;
        strlcpy(s_queued_ext, ext, sizeof(s_queued_ext));
        taken = true;
    }
    portEXIT_CRITICAL(&s_queued_lock);
    if (!taken) {
        heap_caps_free(copy);
    }
    return taken;
}

void muse_sd_image_write(void)
{
    portENTER_CRITICAL(&s_queued_lock);
    uint8_t *data = s_queued;
    size_t len = s_queued_len;
    char ext[sizeof(s_queued_ext)];
    strlcpy(ext, s_queued_ext, sizeof(ext));
    s_queued = NULL;
    portEXIT_CRITICAL(&s_queued_lock);
    if (!data) {
        return;
    }
    /* The tee does the rest: the setting, the temporary file, the name. */
    muse_sd_tee_t *t = muse_sd_tee_begin();
    muse_sd_tee_write(t, data, len);
    muse_sd_tee_end(t, true, ext, 0, 0);
    heap_caps_free(data);
}

/* ---- Caption log ---------------------------------------------------------- */

#if CONFIG_MUSE_GADGET_CAPTION_LOG

#define CAPTIONS MUSE_SD_DIR "/captions.log"
#define HEARD_MAX 512
#define REPLY_MAX 4096
#define LINE_MAX_ 192           /* one caption line */
#define QUEUE_MAX 8192          /* lines waiting for the card */

typedef struct {
    char heard[HEARD_MAX];
    char error[HEARD_MAX];
    char reply[REPLY_MAX];
    size_t page_start;          /* where the current page's lines begin in reply */
    bool page_skip_first;       /* its first line repeats the page before's last */
    char page_first[LINE_MAX_];
    char page_last[LINE_MAX_];
    char page[MUSE_CAPTION_MAX];
    char queue[2][QUEUE_MAX];   /* filled by the voice task, written by the extras task */
    size_t queued[2];
    int filling;
} captions_t;

static captions_t *s_cap;
static SemaphoreHandle_t s_cap_lock;

static void caption_init(void)
{
    s_cap = heap_caps_calloc(1, sizeof(*s_cap), MUSE_BIG_CAPS);
    s_cap_lock = xSemaphoreCreateMutex();
    if (!s_cap || !s_cap_lock) {
        ESP_LOGW(TAG, "no memory for the caption log");
        heap_caps_free(s_cap);
        s_cap = NULL;
    }
}

static bool cap_lock(void)
{
    return s_cap && xSemaphoreTake(s_cap_lock, portMAX_DELAY) == pdTRUE;
}

static void cap_unlock(void)
{
    xSemaphoreGive(s_cap_lock);
}

/* Appends `text` to the reply, a space between it and what's there. */
static void reply_append(const char *text, size_t len)
{
    size_t have = strlen(s_cap->reply);
    if (have && have + 1 < REPLY_MAX && s_cap->reply[have - 1] != ' ') {
        s_cap->reply[have++] = ' ';
        s_cap->reply[have] = '\0';
    }
    if (have + len >= REPLY_MAX) {
        len = REPLY_MAX - 1 - have;
    }
    memcpy(s_cap->reply + have, text, len);
    s_cap->reply[have + len] = '\0';
}

/* One "<time>  <who>: <text>" line onto the queue, newlines flattened. */
static void queue_line(const char *stamp, const char *who, const char *text)
{
    char *q = s_cap->queue[s_cap->filling];
    size_t *n = &s_cap->queued[s_cap->filling];
    int len = snprintf(q + *n, QUEUE_MAX - *n, "%s  %s: %s\n", stamp, who, text);
    if (len <= 0 || *n + len >= QUEUE_MAX) {
        q[*n] = '\0';   /* the card's fallen behind: drop the line */
        return;
    }
    for (char *c = q + *n; c < q + *n + len - 1; c++) {
        if (*c == '\n' || *c == '\r') {
            *c = ' ';
        }
    }
    *n += len;
}

static void flush_locked(void)
{
    if (!s_cap->heard[0] && !s_cap->reply[0] && !s_cap->error[0]) {
        return;
    }
    char stamp[32];
    if (!muse_time_format(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S")) {
        snprintf(stamp, sizeof(stamp), "+%llds", (long long)(esp_timer_get_time() / 1000000));
    }
    if (s_cap->heard[0]) {
        queue_line(stamp, "heard", s_cap->heard);
    }
    if (s_cap->reply[0]) {
        queue_line(stamp, "muse", s_cap->reply);
    }
    if (s_cap->error[0]) {
        queue_line(stamp, "error", s_cap->error);
    }
    s_cap->heard[0] = s_cap->reply[0] = s_cap->error[0] = '\0';
    s_cap->page[0] = s_cap->page_first[0] = s_cap->page_last[0] = '\0';
    s_cap->page_start = 0;
}

void muse_sd_caption_event(int ev, const char *text)
{
    if (!s_ready || !cap_lock()) {
        return;
    }
    switch (ev) {
    case MUSE_HATCH_EV_HEARD:
        /* A transcript after a reply starts the next turn. */
        if (s_cap->reply[0] || s_cap->error[0]) {
            flush_locked();
        }
        strlcpy(s_cap->heard, text, sizeof(s_cap->heard));
        break;
    case MUSE_HATCH_EV_ERROR:
        strlcpy(s_cap->error, text, sizeof(s_cap->error));
        flush_locked();
        break;
    default:
        break;
    }
    cap_unlock();
}

/* Splits off the line at *p (up to '\n'); false at the end. */
static bool next_line(const char **p, const char **start, size_t *len)
{
    if (!**p) {
        return false;
    }
    *start = *p;
    const char *nl = strchr(*p, '\n');
    *len = nl ? (size_t)(nl - *p) : strlen(*p);
    *p = nl ? nl + 1 : *p + *len;
    return true;
}

static bool line_is(const char *line, size_t len, const char *s)
{
    return strlen(s) == len && memcmp(line, s, len) == 0;
}

void muse_sd_caption_page(const char *page)
{
    if (!s_ready || !page[0] || !cap_lock()) {
        return;
    }
    if (strcmp(page, s_cap->page) == 0) {
        cap_unlock();
        return;
    }
    const char *p = page, *line = page;
    size_t len = 0;
    next_line(&p, &line, &len);
    /* The same page again, grown as the reply streamed in (or re-wrapped): put
     * it back in place of what it said before. Otherwise a new page, whose
     * first line repeats the last page's last if it follows on from it. */
    size_t prev_len = strlen(s_cap->page);
    bool same = s_cap->page[0] && (line_is(line, len, s_cap->page_first)
                                   || strncmp(page, s_cap->page, prev_len) == 0);
    if (same) {
        s_cap->reply[s_cap->page_start] = '\0';
    } else {
        s_cap->page_skip_first = s_cap->page[0] && line_is(line, len, s_cap->page_last);
        s_cap->page_start = strlen(s_cap->reply);
    }
    strlcpy(s_cap->page, page, sizeof(s_cap->page));
    snprintf(s_cap->page_first, sizeof(s_cap->page_first), "%.*s", (int)len, line);
    p = page;
    for (bool first = true; next_line(&p, &line, &len); first = false) {
        if (!(first && s_cap->page_skip_first)) {
            reply_append(line, len);
        }
        snprintf(s_cap->page_last, sizeof(s_cap->page_last), "%.*s", (int)len, line);
    }
    cap_unlock();
}

void muse_sd_caption_flush(void)
{
    if (!s_ready || !cap_lock()) {
        return;
    }
    flush_locked();
    cap_unlock();
}

void muse_sd_caption_write(void)
{
    if (!s_ready || !cap_lock()) {
        return;
    }
    /* Swap queues, so the voice task never waits on the card. */
    int full = s_cap->filling;
    size_t n = s_cap->queued[full];
    if (n) {
        s_cap->filling ^= 1;
        s_cap->queued[s_cap->filling] = 0;
    }
    cap_unlock();
    if (!n) {
        return;
    }
    int fd = open(CAPTIONS, O_WRONLY | O_CREAT | O_APPEND, 0664);
    if (fd < 0 || write(fd, s_cap->queue[full], n) != (ssize_t)n) {
        ESP_LOGW(TAG, "captions.log: %s", strerror(errno));
    }
    if (fd >= 0) {
        close(fd);
    }
}

#endif /* CONFIG_MUSE_GADGET_CAPTION_LOG */
