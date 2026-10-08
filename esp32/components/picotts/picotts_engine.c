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
 * The engine's setup and teardown, after DiUS's esp_picotts.c (Apache-2.0),
 * without its polling task: muse_tts.c drives the engine from its own.
 */
#include "picotts_engine.h"

#include <math.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"

/* In this order: esp_picorsrc.h needs picoapi.h's types. */
#include "picoapi.h"
#include "picoapid.h"
#include "picoos.h"
#include "esp_picorsrc.h"

static const char *TAG = "picotts";
static const pico_Char VOICE[] = "PicoVoice";

static const void *s_ta, *s_sg;
static void *s_mem;
static pico_System s_sys;
static pico_Resource s_ta_res, s_sg_res;
static pico_Engine s_engine;
static bool s_voice;

/* Pico's own exp() trick assumes an IEEE layout that doesn't hold on Xtensa;
 * picoos.c is built with its version renamed, so this one is used. */
picoos_double picoos_quick_exp(const picoos_double y)
{
    return exp(y);
}

static void log_status(const char *what, pico_Status st)
{
    pico_Retstring msg = "";
    if (s_sys) {
        pico_getSystemStatusMessage(s_sys, st, msg);
    }
    ESP_LOGE(TAG, "%s (%d): %s", what, (int)st, msg);
}

static const void *map(const char *name)
{
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, name);
    if (!part) {
        ESP_LOGW(TAG, "no %s partition: flash a build with partitions_muse_tts.csv", name);
        return NULL;
    }
    const void *ptr = NULL;
    esp_partition_mmap_handle_t handle;
    if (esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &ptr, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "can't map %s", name);
        return NULL;
    }
    return ptr;   /* mapped for good: the voice is read in place */
}

bool picotts_engine_map(void)
{
    if (!s_ta) {
        s_ta = map("tts_ta");
    }
    if (!s_sg) {
        s_sg = map("tts_sg");
    }
    return s_ta && s_sg;
}

void picotts_engine_close(void)
{
    if (s_engine) {
        pico_disposeEngine(s_sys, &s_engine);
        s_engine = NULL;
    }
    if (s_voice) {
        pico_releaseVoiceDefinition(s_sys, VOICE);
        s_voice = false;
    }
    if (s_sg_res) {
        esp_pico_unloadResource(s_sys, &s_sg_res);
        s_sg_res = NULL;
    }
    if (s_ta_res) {
        esp_pico_unloadResource(s_sys, &s_ta_res);
        s_ta_res = NULL;
    }
    if (s_sys) {
        pico_terminate(&s_sys);
        s_sys = NULL;
    }
    heap_caps_free(s_mem);
    s_mem = NULL;
}

static bool add_to_voice(pico_Resource res)
{
    pico_Retstring name;
    pico_Status st = pico_getResourceName(s_sys, res, name);
    if (st == PICO_OK) {
        st = pico_addResourceToVoiceDefinition(s_sys, VOICE, (const pico_Char *)name);
    }
    if (st != PICO_OK) {
        log_status("voice setup failed", st);
    }
    return st == PICO_OK;
}

bool picotts_engine_open(void)
{
    if (s_engine) {
        return true;
    }
    if (!picotts_engine_map()) {
        return false;
    }
    s_mem = heap_caps_malloc(PICOTTS_WORK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_mem) {
        ESP_LOGE(TAG, "no memory for the engine (%u bytes)", (unsigned)PICOTTS_WORK_BYTES);
        return false;
    }
    pico_Status st = pico_initialize(s_mem, PICOTTS_WORK_BYTES, &s_sys);
    if (st != PICO_OK) {
        log_status("init failed", st);
        goto fail;
    }
    /* An erased or foreign partition fails the SVOX header check here. */
    if ((st = esp_pico_loadResource(s_sys, s_ta, &s_ta_res)) != PICO_OK) {
        log_status("text analysis voice data failed to load", st);
        goto fail;
    }
    if ((st = esp_pico_loadResource(s_sys, s_sg, &s_sg_res)) != PICO_OK) {
        log_status("signal generation voice data failed to load", st);
        goto fail;
    }
    if ((st = pico_createVoiceDefinition(s_sys, VOICE)) != PICO_OK) {
        log_status("voice creation failed", st);
        goto fail;
    }
    s_voice = true;
    if (!add_to_voice(s_ta_res) || !add_to_voice(s_sg_res)) {
        goto fail;
    }
    if ((st = pico_newEngine(s_sys, VOICE, &s_engine)) != PICO_OK) {
        s_engine = NULL;
        log_status("engine creation failed", st);
        goto fail;
    }
    return true;
fail:
    picotts_engine_close();
    return false;
}

bool picotts_engine_is_open(void)
{
    return s_engine != NULL;
}

int picotts_engine_put(const char *utf8, size_t len)
{
    pico_Int16 put = 0;
    pico_Status st = pico_putTextUtf8(s_engine, (const pico_Char *)utf8, (pico_Int16)(len > 32767 ? 32767 : len), &put);
    if (st != PICO_OK) {
        log_status("text rejected", st);
        return -1;
    }
    return put;
}

picotts_step_t picotts_engine_get(int16_t *pcm, size_t cap, size_t *frames)
{
    pico_Int16 bytes = 0, type = 0;
    /* The engine fails a step that would hand over more than 255 bytes. */
    size_t max = cap * sizeof(int16_t);
    pico_Status st = pico_getData(s_engine, pcm, (pico_Int16)(max > 254 ? 254 : max), &bytes, &type);
    *frames = bytes > 0 ? (size_t)bytes / sizeof(int16_t) : 0;
    if (st == PICO_STEP_BUSY) {
        return PICOTTS_BUSY;
    }
    if (st == PICO_STEP_IDLE) {
        return PICOTTS_IDLE;
    }
    log_status("synthesis failed", st);
    return PICOTTS_ERROR;
}

void picotts_engine_reset(void)
{
    if (s_engine) {
        pico_resetEngine(s_engine, PICO_RESET_SOFT);
    }
}
