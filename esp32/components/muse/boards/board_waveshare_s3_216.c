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
 * Waveshare ESP32-S3-Touch-AMOLED-2.16: 480x480 CO5300 AMOLED with CST9220
 * touch, ES8311 speaker + ES7210 dual mic, AXP2101 PMU, PCF85063 RTC.
 * Schematic-checked: the PMU IRQ line is not wired to the ESP32 (I2C polling
 * still works), DCDC1=VCC3V3, ALDO1=A3V3 (codecs).
 *
 * Keys along the top edge, left to right: BOOT (GPIO0), PWR, KEY3 (GPIO18,
 * active low, board pull-up). KEY3 talks. BOOT and PWR set the volume in
 * place of an aux button: BOOT turns it down (repeating while held), a PWR
 * click turns it up, and holding PWR 1.5 s opens the power menu
 * (muse_power_menu.h), where BOOT, PWR and KEY3 are up, down and select.
 * Sleep is in that menu. PWR reaches only the PMU, so its click and long
 * press are latched there and read over I2C.
 */

#include "esp_err.h" /* this BSP's display.h uses esp_err_t without including it */

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

#define DRAW_BUF_LINES 118 /* four bands to the screen (muse_lcd_bands.h) */
#define LCD_CHUNK_BYTES (BSP_LCD_H_RES * 8 * 2)

#define KEY_GPIO GPIO_NUM_18 /* KEY3, active low with the board's pull-up */
#define PMU_KEY_EVERY 2      /* poll the PMU over I2C every 20 ms */
#define PWR_LONG_MS 1500     /* the power menu; the hardware cuts power at 10 s */

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_touch_handle_t s_tp;
static muse_gpio_button_t s_key, s_boot;

static esp_err_t init(void)
{
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "i2c init");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_key, KEY_GPIO), TAG, "key button");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_boot, GPIO_NUM_0), TAG, "boot button");

    /* PWR reaches only the PMU: latch its key IRQs and poll them over I2C.
     * Drain the latch; the press that turned the board on may still be held. */
    esp_err_t err = muse_pmu_init(bsp_i2c_get_handle(), true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PMU unavailable (%s): battery status disabled", esp_err_to_name(err));
        return ESP_OK;
    }
    (void)muse_pmu_poll_key();

    /* Only DCDC1 (VCC3V3) and ALDO1 (A3V3, for the codecs) feed anything; the
     * schematic leaves the rest unconnected. Waveshare's AXP2101 example and
     * xiaozhi's board turn them off too. */
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
 * bsp_display_start(), less its draw buffers: the BSP's are PSRAM, and every
 * flush from them needs a fresh 46 KB internal DMA bounce buffer, which can't
 * be had once Wi-Fi and BLE are up. The bands go out through two fixed 7 KB
 * internal buffers instead; 9 KB ones left 1 KB free while Wi-Fi joined.
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

    /* As the BSP's own bsp_display_start() maps touch at ROTATE_0. */
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

/*
 * MADCTL for each quarter turn from upright. The BSP's init leaves 0xA0
 * (MY | MV); MX | MV (0x60) is that turned 180 degrees, and the two without
 * MV (0x00, MX | MY) the sideways ones, the panel being square. Which of
 * those is which way round is SIDEWAYS_SWAPPED's to say (muse_orient.c
 * names a turn by the way the board's top edge points).
 */
#define MADCTL_MX 0x40
#define MADCTL_MY 0x80
#define MADCTL_MV 0x20
#define SIDEWAYS_SWAPPED 0
static const uint8_t MADCTL_TURN[4] = {
    MADCTL_MY | MADCTL_MV,
    SIDEWAYS_SWAPPED ? MADCTL_MX | MADCTL_MY : 0x00,
    MADCTL_MX | MADCTL_MV,
    SIDEWAYS_SWAPPED ? 0x00 : MADCTL_MX | MADCTL_MY,
};

static void send_turn(void *madctl)
{
    esp_lcd_panel_io_tx_param(s_io, (0x02 << 24) | (0x36 << 8), madctl, 1);
}

/*
 * Touch follows the panel bit for bit: esp_lcd_touch mirrors x, then y, then
 * swaps (esp_lcd_touch.c), as MX, MY and MV turn the picture. Upright that's
 * mirror y and swap: the screen's (x, y) is (H - ty, tx).
 */
static void set_turn(int quarters)
{
    uint8_t madctl = MADCTL_TURN[quarters & 3];
    muse_lcd_bands_run(send_turn, &madctl);
    esp_lcd_touch_set_mirror_x(s_tp, madctl & MADCTL_MX);
    esp_lcd_touch_set_mirror_y(s_tp, madctl & MADCTL_MY);
    esp_lcd_touch_set_swap_xy(s_tp, madctl & MADCTL_MV);
}

/* Plain SLPIN/SLPOUT over the QSPI command path. The driver's own sleep also
 * enters deep standby, whose wake pulses the reset line; panel (GPIO39) and
 * touch (GPIO40) resets are separate lines here. */
static void panel_sleep(bool sleep)
{
    muse_lcd_bands_run(send_sleep, &sleep);
    vTaskDelay(pdMS_TO_TICKS(120)); /* settle before the next command */
}

/*
 * Screen off: LVGL stops, and the touch controller is put to sleep with the
 * CST9217's deep-sleep command (0xD1 0x05, as on the 1.75C), where it only
 * answers its reset line (GPIO40).
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
        vTaskDelay(pdMS_TO_TICKS(50)); /* as the driver waits after its reset */
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
 * KEY3 talks. BOOT's edges are volume down's (muse_gpio_button_poll reports
 * them as the talk bits; muse_input repeats while it's held). PWR is the
 * PMU's own click and long press: a long press once it's held past
 * PWR_LONG_MS, a click on the release of a shorter one. Should both come in
 * for one press, the long press wins. A click is a whole press by itself, so
 * one that went by unpolled while wait_buttons slept still counts; nothing
 * here acts on PWR's bare press and release edges.
 */
static unsigned poll_buttons(void)
{
    static unsigned tick;
    static bool long_pressed;
    unsigned boot = muse_gpio_button_poll(&s_boot);
    unsigned ev = muse_gpio_button_poll(&s_key) |
                  (boot & MUSE_BTN_TALK_PRESS ? MUSE_BTN_VOL_DOWN_PRESS : 0) |
                  (boot & MUSE_BTN_TALK_RELEASE ? MUSE_BTN_VOL_DOWN_RELEASE : 0);
    if (tick++ % PMU_KEY_EVERY == 0) {
        unsigned pmu = muse_pmu_poll_key();
        if (pmu & MUSE_PMU_KEY_PRESS) {
            long_pressed = false;
        }
        if (pmu & MUSE_PMU_KEY_LONG) {
            long_pressed = true;
            ev |= MUSE_BTN_POWER_MENU;
        } else if ((pmu & MUSE_PMU_KEY_CLICK) && !long_pressed) {
            ev |= MUSE_BTN_VOL_UP;
        }
    }
    return ev;
}

static void wait_buttons(int timeout_ms)
{
    /* The PMU's IRQ line isn't wired to the ESP32, so PWR can't wake the chip
     * from light sleep; its latched click or long press is read on the next
     * poll, within wait_buttons' timeout. The PMU handles power off at 10 s. */
    muse_gpio_buttons_wait((muse_gpio_button_t *const[]){ &s_key, &s_boot }, 2, timeout_ms);
}

static const muse_board_t s_board = {
    .name = "Waveshare ESP32-S3-Touch-AMOLED-2.16",
    .width = BSP_LCD_H_RES, .height = BSP_LCD_V_RES, .round = false, .touch = true, .diagonal_in = 2.16f,
    .talk_button = "key", .aux_button = "boot",
    /* The buttons are on the top edge: BOOT, PWR, KEY3 from the left, seen from
     * the front. No talk_hint or aux_hint, so no icons beside them: a mic
     * drawn by the top edge read as a status, not a key, and the volume keys
     * don't sleep or power off, so there's no power icon to show. */
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .display_pause = display_pause,
    .set_turn = set_turn,
    .audio_init = audio_init,
    .mic_slot = -1,
    .set_mic_gain = set_mic_gain,
    .poll_buttons = poll_buttons,
    .wait_buttons = wait_buttons,
    .read_power = muse_pmu_read_power,
    .power_off = muse_pmu_power_off,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
