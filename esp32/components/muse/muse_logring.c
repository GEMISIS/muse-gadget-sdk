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

/* The log kept in PSRAM (muse_logring.h). */
#include "muse_logring.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define RING_BYTES (96 * 1024)
#define RING_LINE_MAX 320

EXT_RAM_BSS_ATTR static char *s_ring;
EXT_RAM_BSS_ATTR static size_t s_head;     /* where the next byte goes */
EXT_RAM_BSS_ATTR static bool s_wrapped;
EXT_RAM_BSS_ATTR static char s_line[RING_LINE_MAX];   /* with s_lock */
EXT_RAM_BSS_ATTR static SemaphoreHandle_t s_lock;
static vprintf_like_t s_prev;

static void put(const char *p, size_t n)
{
    while (n) {
        size_t room = RING_BYTES - s_head, k = n < room ? n : room;
        memcpy(s_ring + s_head, p, k);
        s_head = (s_head + k) % RING_BYTES;
        s_wrapped |= s_head == 0;
        p += k;
        n -= k;
    }
}

static int keep(const char *fmt, va_list ap)
{
    va_list copy;
    va_copy(copy, ap);
    int r = s_prev ? s_prev(fmt, ap) : vprintf(fmt, ap);
    /* Never waits: a line logged while another's being kept (or from an ISR) is only printed. */
    if (!xPortInIsrContext() && xSemaphoreTake(s_lock, 0) == pdTRUE) {
        int n = vsnprintf(s_line, sizeof(s_line), fmt, copy);
        if (n > 0) {
            put(s_line, (size_t)n < sizeof(s_line) ? (size_t)n : sizeof(s_line) - 1);
        }
        xSemaphoreGive(s_lock);
    }
    va_end(copy);
    return r;
}

void muse_logring_start(void)
{
    if (s_ring) {
        return;
    }
    s_ring = heap_caps_malloc(RING_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_lock = s_ring ? xSemaphoreCreateMutexWithCaps(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (!s_lock) {
        return;
    }
    s_prev = esp_log_set_vprintf(keep);
}

void muse_logring_print(void)
{
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }
    /* Copied out first, so logging goes on while it prints (slowly, over USB). */
    size_t n = s_wrapped ? RING_BYTES : s_head;
    char *copy = heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (copy) {
        size_t start = s_wrapped ? s_head : 0;
        for (size_t i = 0; i < n; i++) {
            copy[i] = s_ring[(start + i) % RING_BYTES];
        }
        copy[n] = '\0';
    }
    xSemaphoreGive(s_lock);
    if (!copy) {
        return;
    }
    printf("\n@log begin %u\n", (unsigned)n);
    for (size_t i = 0; i < n; i++) {
        copy[i] = copy[i] ? copy[i] : ' ';   /* a stray NUL would end it early */
    }
    const char *p = s_wrapped ? strchr(copy, '\n') : copy;   /* from a whole line */
    for (p = p ? p + (s_wrapped ? 1 : 0) : copy; *p;) {
        size_t k = strnlen(p, 128);
        fwrite(p, 1, k, stdout);
        fflush(stdout);
        p += k;
        vTaskDelay(pdMS_TO_TICKS(4));   /* the USB console drops what comes faster than it goes */
    }
    printf("\n@log end\n");
    fflush(stdout);
    heap_caps_free(copy);
}
