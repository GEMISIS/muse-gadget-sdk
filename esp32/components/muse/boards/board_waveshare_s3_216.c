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
 * Waveshare ESP32-S3-Touch-AMOLED-2.16: square 480x480 CO5300 AMOLED over
 * QSPI with CST9220 touch, ES8311 speaker + ES7210 dual mic, AXP2101 PMU.
 *
 * Sources:
 * - Pins (QSPI CS 12, SCLK 38, D0-D3 4-7, panel reset 39; touch reset 40,
 *   INT 11; I2C SDA 15, SCL 14; I2S MCLK 42, BCLK 9, WS 45, DOUT 8, DIN 10;
 *   amp enable 46), the CO5300 init sequence and the CST9217-family touch
 *   driver: the waveshare/esp32_s3_touch_amoled_2_16 BSP
 *   (include/bsp/esp32_s3_touch_amoled_2_16.h), which matches
 *   examples/arduino/libraries/Mylibrary/pin_config.h in
 *   waveshareteam/ESP32-S3-Touch-AMOLED-2.16 and the wiki's GPIO table.
 * - Buttons: the product page and wiki list BOOT (GPIO0), PWR and a
 *   programmable key on GPIO18 (pulled up, active low). The schematic takes
 *   PWR only to the AXP2101's PWRON pin, so it is read from the PMU's key
 *   latch.
 * - AXP2101 at 0x34: examples/esp-idf/01_AXP2101 in the vendor repo. The
 *   schematic feeds VCC3V3 from DCDC1 and A3V3 (the codecs) from ALDO1.
 *
 * GPIO18 talks. BOOT and PWR set the volume in place of an aux button:
 * BOOT turns it down (repeating while held), a PWR click turns it up, and
 * holding PWR 1.5 s opens the power menu (muse_power_menu.h), where BOOT,
 * PWR and GPIO18 are up, down and select. Sleep is in that menu.
 */
#include "esp_err.h"         /* the BSP's display.h uses esp_err_t without it */
#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_board.h"
#include "muse_lcd_bands.h"
#include "muse_mem.h"
#include "muse_pmu.h"

static const char *TAG = "board";

#define DRAW_BUF_LINES 120      /* four bands to the screen (muse_lcd_bands.h) */
#define LCD_CHUNK_BYTES (BSP_LCD_H_RES * 8 * 2)
#define TALK_GPIO GPIO_NUM_18
#define PMU_KEY_EVERY 2         /* poll the PMU over I2C every 20 ms */
#define PWR_LONG_MS 1500        /* the power menu; the hardware cuts power at 10 s */

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_touch_handle_t s_tp;
static muse_gpio_button_t s_talk, s_boot;

static esp_err_t init(void)
{
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "i2c init");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_talk, TALK_GPIO), TAG, "talk button");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_boot, GPIO_NUM_0), TAG, "boot button");
    /* Only the PMU sees PWR: latch its edges for poll_buttons(). */
    esp_err_t err = muse_pmu_init(bsp_i2c_get_handle(), true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PMU unavailable (%s): battery status disabled", esp_err_to_name(err));
        return ESP_OK;
    }
    /* DCDC1 (VCC3V3) and ALDO1 (A3V3, for the codecs) are the rails the 1.75
     * boards keep; the 2.16's schematic draws the same two for the ESP32 and
     * the codecs. */
    err = muse_pmu_keep_rails(BIT(0), BIT(0));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "unused rails left on (%s)", esp_err_to_name(err));
    }
    err = muse_pmu_set_long_press_ms(PWR_LONG_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PWR long press left at the PMU's default (%s)", esp_err_to_name(err));
    }
    return ESP_OK;
}

/* The CO5300 needs even-aligned update windows. */
static void round_area(lv_event_t *e)
{
    lv_area_t *a = lv_event_get_param(e);
    a->x1 &= ~1;
    a->y1 &= ~1;
    a->x2 |= 1;
    a->y2 |= 1;
}

/*
 * bsp_display_start(), less its draw buffers, as on the 1.75C: the bands go
 * out through two fixed internal buffers instead of PSRAM ones that each need
 * an internal DMA bounce buffer (board_waveshare_s3_175c.c).
 */
static lv_display_t *display_start(lv_indev_t **touch)
{
    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_core_id = MUSE_UI_CORE;
    adapter_cfg.task_priority = MUSE_UI_PRIORITY;
    if (esp_lv_adapter_init(&adapter_cfg) != ESP_OK) {
        return NULL;
    }

    const bsp_display_config_t panel_cfg = {
        .max_transfer_sz = LCD_CHUNK_BYTES,
    };
    if (bsp_display_new(&panel_cfg, &s_panel, &s_io) != ESP_OK) {
        return NULL;
    }
    const esp_lv_adapter_display_config_t disp_cfg = {
        .panel = s_panel,
        .panel_io = s_io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = BSP_LCD_H_RES,
            .ver_res = BSP_LCD_V_RES,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = muse_lcd_bands_register(disp_cfg, DRAW_BUF_LINES, LCD_CHUNK_BYTES);
    if (!disp) {
        return NULL;
    }
    lv_display_add_event_cb(disp, round_area, LV_EVENT_INVALIDATE_AREA, NULL);

    /* The BSP's own bsp_display_start() orientation. */
    const bsp_display_cfg_t touch_cfg = {
        .touch_flags = { .swap_xy = 1, .mirror_y = 1 },
    };
    if (bsp_touch_new(&touch_cfg, &s_tp) != ESP_OK) {
        return NULL;
    }
    const esp_lv_adapter_touch_config_t tp_cfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, s_tp);
    *touch = esp_lv_adapter_register_touch(&tp_cfg);
    if (!*touch || esp_lv_adapter_start() != ESP_OK) {
        return NULL;
    }
    return disp;
}

static bool display_lock(int timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

static void send_brightness(void *level)
{
    /* CO5300 "write display brightness" (0x51), as the BSP sends it. */
    esp_lcd_panel_io_tx_param(s_io, (0x02 << 24) | (0x51 << 8), level, 1);
}

static void set_brightness(int pct)
{
    uint8_t level = (uint8_t)(pct * 255 / 100);
    muse_lcd_bands_run(send_brightness, &level);
}

static void send_sleep(void *sleep)
{
    esp_lcd_panel_io_tx_param(s_io, (0x02 << 24) | ((*(bool *)sleep ? 0x10 : 0x11) << 8), NULL, 0);
}

static void send_flip(void *flipped)
{
    /* The BSP's init leaves MADCTL at 0xA0 (MY | MV), which the driver keeps;
     * MX in place of MY is the same picture turned 180 degrees (0x60). */
    bool f = *(bool *)flipped;
    esp_lcd_panel_mirror(s_panel, f, !f);
}

/*
 * Touch is mirrored in software (esp_lcd_touch.c: mirror x, then y, then
 * swap). Upright that's mirror y and swap: the screen's (x, y) is
 * (H - ty, tx). Turned it's (W - x, H - y) = (ty, W - tx), which is mirror x
 * and swap, the panel being square.
 */
static void set_flip(bool flipped)
{
    muse_lcd_bands_run(send_flip, &flipped);
    esp_lcd_touch_set_mirror_x(s_tp, flipped);
    esp_lcd_touch_set_mirror_y(s_tp, !flipped);
}

/* Plain SLPIN/SLPOUT over the QSPI command path, as on the 1.75C, rather than
 * the driver's deep standby, whose wake needs a reset. */
static void panel_sleep(bool sleep)
{
    muse_lcd_bands_run(send_sleep, &sleep);
    vTaskDelay(pdMS_TO_TICKS(120));   /* settle before the next command */
}

/*
 * Screen off: LVGL stops, and the CST9220 goes from scanning to deep sleep
 * (command 0xD105, as the CST9217), where it only answers its reset line,
 * GPIO 40; the panel's is GPIO 39.
 */
static void display_pause(bool pause)
{
    if (pause) {
        esp_lv_adapter_pause(-1);
        esp_lcd_panel_io_tx_param(s_tp->io, 0xD1, (uint8_t[]){ 0x05 }, 1);
    } else {
        gpio_set_level(BSP_LCD_TOUCH_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(BSP_LCD_TOUCH_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(50));   /* as the driver waits after its reset */
        esp_lv_adapter_resume();
    }
}

static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    *spk = bsp_audio_codec_speaker_init();
    *mic = bsp_audio_codec_microphone_init();
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

static void set_mic_gain(esp_codec_dev_handle_t mic, int db)
{
    /* ES7210 PGA steps are 3 dB; snap so the UI shows what's applied. */
    db = (db / 3) * 3;
    /* esp_codec_dev rounds 33 dB down to 30; the next real step up is 34.5. */
    esp_codec_dev_set_in_gain(mic, db == 33 ? 34.5f : (float)db);
}

/*
 * BOOT's edges are volume down's (muse_gpio_button_poll reports them as the
 * talk bits). PWR is the PMU's own click and long press: a long press once
 * it's held past PWR_LONG_MS, a click on the release of a shorter one. Should
 * both come in for one press, the long press wins.
 */
static unsigned poll_buttons(void)
{
    static unsigned tick;
    static bool long_pressed;
    unsigned boot = muse_gpio_button_poll(&s_boot);
    unsigned ev = muse_gpio_button_poll(&s_talk) |
                  (boot & MUSE_BTN_TALK_PRESS ? MUSE_BTN_VOL_DOWN_PRESS : 0) |
                  (boot & MUSE_BTN_TALK_RELEASE ? MUSE_BTN_VOL_DOWN_RELEASE : 0);
    if (tick++ % PMU_KEY_EVERY == 0) {
        unsigned key = muse_pmu_poll_key();
        if (key & MUSE_PMU_KEY_PRESS) {
            long_pressed = false;
        }
        if (key & MUSE_PMU_KEY_LONG) {
            long_pressed = true;
            ev |= MUSE_BTN_POWER_MENU;
        } else if ((key & MUSE_PMU_KEY_CLICK) && !long_pressed) {
            ev |= MUSE_BTN_VOL_UP;
        }
    }
    return ev;
}

static const muse_board_t s_board = {
    .name = "Waveshare ESP32-S3-Touch-AMOLED-2.16",
    .width = BSP_LCD_H_RES,
    .height = BSP_LCD_V_RES,
    .round = false,
    .touch = true,
    .diagonal_in = 2.16f,
    .talk_button = "top right",
    .aux_button = "top left",   /* BOOT and PWR, in captions and wake logs */
    /* Keys along the top edge (USB-C below): BOOT, PWR, then GPIO18. No
     * aux_hint: the volume keys don't sleep or power off, so there's no
     * power icon to show. */
    .talk_hint = { LV_ALIGN_TOP_MID, 150, 16 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .display_pause = display_pause,
    .set_flip = set_flip,
    .audio_init = audio_init,
    .mic_slot = -1,
    .set_mic_gain = set_mic_gain,
    .poll_buttons = poll_buttons,   /* PWR is on the PMU, so it's polled */
    .read_power = muse_pmu_read_power,
    .power_off = muse_pmu_power_off,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
