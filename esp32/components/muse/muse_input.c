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

#include "muse_input.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#if CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif

#include "muse_battery.h"
#include "muse_ble.h"
#include "muse_board.h"
#include "muse_chat.h"
#include "muse_console.h"
#include "muse_gadget_mode.h"
#include "muse_link.h"
#include "muse_mem.h"
#include "muse_menu.h"
#include "muse_power_menu.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_ui.h"
#include "muse_up_next.h"
#include "muse_voice.h"
#include "muse_browse.h"
#include "muse_widget.h"
#include "muse_wifi.h"
#if CONFIG_MUSE_WATCHER_CAMERA
#include "boards/watcher_camera.h"
#endif

static const char *TAG = "muse_input";

#define POLL_MS 10
#define REST_POLL_MS 50        /* screen off and paused: still quick to wake */
#define REST_WAIT_MS 1000      /* paused, buttons that interrupt: the rest waits this long */
#define NAP_WAIT_MS 10000      /* ... and Wi-Fi napping */
#define POWER_MS 2000          /* refresh battery */
#define REST_POWER_MS 10000    /* ... while paused */
#define WIFI_NAP_MS (2 * 60 * 1000)   /* low power this long: Wi-Fi off until it ends */
#define DOUBLE_TICKS 35        /* 350 ms: a second aux press within this toggles phone setup */

#define GOODBYE_MS 1500        /* let the goodbye animation play */
#define HINT_TICKS 60          /* 0.6 s: warn that holding powers off */
#define LONG_TICKS 150         /* 1.5 s: power off */
#define SLEEP_CHECK_MS 100
#define VOLUME_STEP 5          /* per volume key press */
#define MUTE_TAP_US (350 * 1000)   /* volume down twice within this mutes or unmutes */
#define VOLUME_DELAY_TICKS 50  /* 500 ms: holding volume down starts repeating */
#define VOLUME_REPEAT_TICKS 30 /* ... every 300 ms */

#define SERIAL_RX 1024         /* the driver drops what doesn't fit, so a console line must */
#define SERIAL_LINE 1024
#define CHAT_MAX (192 * 1024)  /* a typed message, assembled from "chat+=" lines */

static QueueHandle_t s_queue;
static TaskHandle_t s_input;
static bool s_talk_down;
static bool s_cpu_low;      /* display stopped and the CPU allowed to sleep */
static volatile bool s_power_off_requested;
static volatile bool s_nap_now;   /* ">nap": asleep, as if on battery, nap without waiting WIFI_NAP_MS */

static void post(muse_ptt_t type, bool wake)
{
    muse_input_event_t ev = { .type = type, .wake = wake };
    ESP_LOGI(TAG, "PTT %s%s", type == MUSE_PTT_DOWN ? "down" : "up", wake ? " (waking)" : "");
    xQueueSend(s_queue, &ev, 0);
}

static bool update_power(void);

static void power_off(void)
{
    ESP_LOGI(TAG, "shutting down");
    muse_state_set_asleep(false);
    update_power();
    muse_state_set_progress(0);
    muse_state_set_level(0);
    muse_state_set_mode(MUSE_MODE_OFF);
    muse_state_set_caption("GOODBYE!");
    vTaskDelay(pdMS_TO_TICKS(GOODBYE_MS));
    esp_err_t err = muse_board->power_off();
    /* Only reached if the board couldn't power off. */
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGE(TAG, "power-off failed (%s)", esp_err_to_name(err));
    muse_state_set_mode(MUSE_MODE_IDLE);
    muse_state_set_caption("COULDN'T POWER OFF");
}

static void set_asleep(bool asleep, const char *why)
{
    if (asleep != muse_state_asleep()) {
        ESP_LOGI(TAG, "%s (%s)", asleep ? "sleeping" : "waking", why);
        muse_state_set_asleep(asleep);
        if (s_input) {
            xTaskNotifyGive(s_input);   /* out of wait_buttons() */
        }
        if (!asleep && !muse_wifi_connected()) {
            muse_wifi_apply();   /* the screen says reconnecting: don't sit out the backoff */
        }
    }
}

static void toggle_phone_setup(void)
{
    bool on = !muse_settings_ble_on();
    ESP_LOGI(TAG, "phone setup %s", on ? "on" : "off");
    muse_settings_set_ble_on(on);
    muse_ble_status_t b;
    muse_ble_status(&b);
    if (on && b.name[0]) {
        muse_state_set_caption("PHONE SETUP: %s", b.name);
    } else {
        muse_state_set_caption("PHONE SETUP %s", on ? "ON" : "OFF");
    }
}

/*
 * Aux button: short press sleeps, a 1.5 s hold powers off, any press wakes.
 * Two quick presses toggle BLE phone setup, so the sleep waits a moment to
 * see whether a second press follows.
 */
static void aux_button(bool pressed, bool edge)
{
    static int held;
    static bool swallow;
    static bool hinted;
    static int sleep_in;    /* ticks until a pending single press sleeps */
    static char saved_caption[64];

    if (sleep_in && --sleep_in == 0) {
        set_asleep(true, muse_board->aux_button);
    }
    if (edge && pressed) {
        held = 0;
        hinted = false;
        swallow = muse_state_asleep();
        if (swallow) {
            set_asleep(false, muse_board->aux_button);
        } else if (sleep_in) {
            sleep_in = 0;
            swallow = true;
            toggle_phone_setup();
        }
        return;
    }
    if (pressed && !swallow) {
        held++;
        if (held == HINT_TICKS) {
            uint32_t v = UINT32_MAX;
            muse_state_caption(saved_caption, sizeof(saved_caption), &v);
            muse_state_set_caption("HOLD TO POWER OFF");
            hinted = true;
        } else if (held == LONG_TICKS) {
            swallow = true;
            power_off();
        }
        return;
    }
    if (edge && !pressed && !swallow) {
        if (hinted) {
            muse_state_set_caption("%s", saved_caption);   /* let go early: cancel */
        } else {
            sleep_in = DOUBLE_TICKS;
        }
    }
}

/* No touch: the aux button opens the menu and steps down it; any press wakes. */
static void menu_button(bool pressed, bool edge)
{
    if (!edge || !pressed) {
        return;
    }
    if (muse_state_asleep()) {
        set_asleep(false, muse_board->aux_button);
    } else if (!s_talk_down) {
        muse_state_poke();
        muse_menu_key(MUSE_MENU_DOWN);
    }
}

static void aux_key(bool pressed, bool edge)
{
    if (muse_board->touch) {
        aux_button(pressed, edge);
    } else {
        menu_button(pressed, edge);
    }
}

/* Talk button: push-to-talk, or Select while the menu is open. Asleep, the
 * press wakes and is posted as a waking one: muse_voice records only if it's
 * still held once awake. */
static void talk_button(unsigned ev)
{
    bool talk_down = s_talk_down;
    static bool swallow;
#if CONFIG_MUSE_WATCHER_CAMERA
    static TickType_t last_release;
    if ((ev & MUSE_BTN_TALK_PRESS) && !muse_state_asleep()
        && !muse_menu_is_open() && last_release
        && xTaskGetTickCount() - last_release <= pdMS_TO_TICKS(350)) {
        ESP_LOGI(TAG, "wheel double-click: camera preview/shutter");
        last_release = 0;
        watcher_camera_preview_toggle();
        swallow = true;
        return;
    }
#endif

    /* A quick tap can latch press and release in the same poll, and a release
     * can land just before the next press; keep them ordered. */
    bool released = ev & MUSE_BTN_TALK_RELEASE;
#if CONFIG_MUSE_WATCHER_CAMERA
    bool saw_release = released;
#endif
    if ((talk_down || swallow) && released) {
        if (talk_down) {
            post(MUSE_PTT_UP, false);
        }
        talk_down = swallow = false;
        released = false;
    }
    if (!talk_down && !swallow && (ev & MUSE_BTN_TALK_PRESS)) {
        if (muse_link_talk_press()) {
            /* Confirmed a Muse app pairing (Link's setup button). */
            muse_state_poke();
            swallow = true;
        } else if (muse_state_asleep()) {
            set_asleep(false, muse_board->talk_button);
            post(MUSE_PTT_DOWN, true);
            talk_down = true;
        } else if (muse_menu_is_open()) {
            muse_state_poke();
            if (!muse_board->keyboard) muse_menu_key(MUSE_MENU_SELECT);
            swallow = true;
        } else if (muse_power_menu_is_open()) {
            muse_state_poke();
            muse_power_menu_key(MUSE_POWER_MENU_SELECT);
            swallow = true;
        } else {
            post(MUSE_PTT_DOWN, false);
            talk_down = true;
        }
    }
    if ((talk_down || swallow) && released) {
        if (talk_down) {
            post(MUSE_PTT_UP, false);
        }
        talk_down = swallow = false;
    }
#if CONFIG_MUSE_WATCHER_CAMERA
    if (saw_release && !swallow) {
        last_release = xTaskGetTickCount();
    }
#endif
    s_talk_down = talk_down;
}

/* Keyboard menus don't repurpose Space/GO as Select. Navigation wakes the
 * screen without accidentally changing a setting; Enter can confirm pairing. */
static void keyboard_buttons(unsigned ev)
{
    const unsigned mask = MUSE_BTN_UP | MUSE_BTN_DOWN | MUSE_BTN_LEFT |
                          MUSE_BTN_RIGHT | MUSE_BTN_ENTER | MUSE_BTN_ESCAPE;
    if (!(ev & mask) || s_talk_down) return;
    if ((ev & MUSE_BTN_ENTER) && muse_link_talk_press()) {
        muse_state_poke();
        return;
    }
    if (muse_state_asleep()) {
        set_asleep(false, "keyboard");
        return;
    }
    muse_state_poke();
    if (ev & MUSE_BTN_ESCAPE) muse_menu_key(MUSE_MENU_BACK);
    else if (ev & MUSE_BTN_UP) muse_menu_key(MUSE_MENU_UP);
    else if (ev & MUSE_BTN_DOWN) muse_menu_key(MUSE_MENU_DOWN);
    else if (ev & MUSE_BTN_LEFT) muse_menu_key(MUSE_MENU_LEFT);
    else if (ev & MUSE_BTN_RIGHT) muse_menu_key(MUSE_MENU_RIGHT);
    else if (ev & MUSE_BTN_ENTER) muse_menu_key(MUSE_MENU_SELECT);
}

static void volume_step(int delta)
{
    if (delta > 0 && !muse_settings_speaker_on()) {
        muse_settings_set_speaker_on(true);   /* volume up while muted: sound back on, first */
    }
    int pct = muse_settings_volume() + delta;
    pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    if (pct != muse_settings_volume()) {
        muse_settings_set_volume(pct);   /* saved, and applied by the app's listener */
    }
    muse_power_menu_show_volume(pct);
    muse_voice_earcon(MUSE_EARCON_TICK);   /* at the new volume, so it can be heard */
}

/*
 * Volume and power keys (MUSE_BTN_VOL_*, MUSE_BTN_POWER_MENU), on boards that
 * have them in place of aux. Asleep, a press only wakes. With the power menu
 * open they move through it instead: volume down up, volume up down, and a
 * long press closes it. Otherwise volume down steps down, repeating while
 * held, volume up steps up, and a long press opens the menu.
 */
static void volume_keys(unsigned ev)
{
    static bool down_held, swallow;
    static int held;
    static int64_t tapped_us;   /* the last single press of volume down, for a double tap */
    static int before;          /* the volume before it */

    if (ev & MUSE_BTN_VOL_DOWN_PRESS) {
        down_held = true;
        held = 0;
        swallow = muse_state_asleep() || muse_power_menu_is_open();
        muse_state_poke();
        int64_t now = esp_timer_get_time();
        if (muse_state_asleep()) {
            set_asleep(false, muse_board->aux_button);
        } else if (muse_power_menu_is_open()) {
            muse_power_menu_key(MUSE_POWER_MENU_UP);
        } else if (tapped_us && now - tapped_us < MUTE_TAP_US) {
            /* A double tap mutes (or unmutes): the first tap's step is undone. */
            tapped_us = 0;
            swallow = true;
            if (muse_settings_volume() != before) {
                muse_settings_set_volume(before);
            }
            bool on = !muse_settings_speaker_on();
            muse_settings_set_speaker_on(on);
            ESP_LOGI(TAG, "volume down twice: %s", on ? "sound on" : "muted");
            muse_power_menu_show_volume(before);
            if (on) {
                muse_voice_earcon(MUSE_EARCON_TICK);
            }
        } else {
            before = muse_settings_volume();
            tapped_us = now;
            volume_step(-VOLUME_STEP);
        }
    } else if (ev & MUSE_BTN_VOL_DOWN_RELEASE) {
        down_held = false;
    } else if (down_held && !swallow && ++held >= VOLUME_DELAY_TICKS
               && (tapped_us = 0, true)   /* held: not a tap */
               && (held - VOLUME_DELAY_TICKS) % VOLUME_REPEAT_TICKS == 0) {
        muse_state_poke();
        volume_step(-VOLUME_STEP);
    }

    if (ev & (MUSE_BTN_VOL_UP | MUSE_BTN_POWER_MENU)) {
        muse_state_poke();
        if (muse_state_asleep()) {
            set_asleep(false, muse_board->aux_button);
        } else if (muse_power_menu_is_open()) {
            muse_power_menu_key(ev & MUSE_BTN_POWER_MENU ? MUSE_POWER_MENU_CLOSE : MUSE_POWER_MENU_DOWN);
        } else if (ev & MUSE_BTN_POWER_MENU) {
            if (!s_talk_down) {
                muse_power_menu_key(MUSE_POWER_MENU_OPEN);
            }
        } else {
            volume_step(VOLUME_STEP);
        }
    }
}

/* A pairing prompt wakes the screen and keeps it on; otherwise idle sleeps. */
static void check_sleep(void)
{
    muse_ble_status_t ble;
    muse_ble_status(&ble);
    bool prompt = ble.passkey || muse_link_state() == MUSE_LINK_CONFIRM;
    if (prompt) {
        set_asleep(false, "pairing");
        return;
    }
    int after = muse_settings_sleep_s();
    float mode_t;
    if (after && !muse_state_asleep() && muse_state_mode(&mode_t) == MUSE_MODE_IDLE
        && muse_state_idle_secs() > after) {
        set_asleep(true, "auto-sleep");
    }
}

static void set_cpu_low(bool low)
{
#if CONFIG_PM_ENABLE
    esp_pm_config_t pm = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = low ? CONFIG_XTAL_FREQ : CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .light_sleep_enable = low,
    };
    esp_err_t err = esp_pm_configure(&pm);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "power management: %s", esp_err_to_name(err));
    }
#else
    (void)low;
#endif
}

/*
 * On battery with the screen dark and voice resting: stop the display and let
 * the CPU drop to the crystal clock and light-sleep between polls, or until a
 * button interrupts (wait_buttons). Returns true while the display is stopped.
 * With a USB host attached (">nap" on the bench) the CPU stays at full speed:
 * at the crystal clock a long line of serial output can stall the USB console
 * until the host reopens the port.
 */
static bool update_power(void)
{
    static bool paused;
    bool pause = muse_board->display_pause && muse_state_on_battery() && muse_state_asleep()
                 && muse_ui_dark() && muse_voice_resting();
    bool want_low = pause && !muse_console_host();
    if (pause == paused && want_low == s_cpu_low) {
        return paused;
    }
    if (pause && !paused) {
        muse_board->display_pause(true);
    }
    if (want_low != s_cpu_low) {
        set_cpu_low(want_low);
    }
    if (!pause && paused) {
        muse_board->display_pause(false);
    }
    paused = pause;
    s_cpu_low = want_low;
    ESP_LOGI(TAG, "%s", s_cpu_low ? "low power: display paused"
                        : paused  ? "display paused (USB host: CPU at full speed)"
                                  : "full power");
    return paused;
}

/*
 * Plugged in to charge: Muse cheers, a hop and a lightning bolt on the face
 * (muse_ui_plugged) and a little jingle (MUSE_EARCON_CHARGE), if he's idle
 * with no turn under way. Asleep, the screen wakes for it; but asleep at
 * night he stays asleep, and at night there's no jingle. A plug again within
 * CHEER_GAP_MS (a loose cable) isn't cheered again, unless `forced` (">charge").
 */
#define CHEER_GAP_MS 10000

static void cheer_plugged(const char *why, bool forced)
{
    static TickType_t last;
    static bool cheered;
    TickType_t now = xTaskGetTickCount();
    bool night = muse_gadget_mode() == MUSE_GADGET_NIGHT;
    const char *quiet = muse_state_mode(NULL) != MUSE_MODE_IDLE || muse_hatch_turn_busy() ? "busy"
                        : night && muse_state_asleep()                                   ? "asleep at night"
                        : !forced && cheered && now - last < pdMS_TO_TICKS(CHEER_GAP_MS) ? "just cheered"
                                                                                          : NULL;
    if (quiet) {
        ESP_LOGI(TAG, "%s: no cheer (%s)", why, quiet);
        return;
    }
    cheered = true;
    last = now;
    ESP_LOGI(TAG, "%s: cheering%s", why, night ? " (night: no jingle)" : "");
    set_asleep(false, why);
    muse_ui_plugged();
    if (!night) {
        muse_voice_earcon(MUSE_EARCON_CHARGE);
    }
}

/* USB power or charging arriving, after the first reading (so not at boot): cheer_plugged. */
static void note_plug(const muse_power_t *p)
{
    static int was = -1;   /* powered at the last reading; -1 before the first */
    int powered = p->usb || p->charging;
    if (was == 0 && powered) {
        cheer_plugged("plugged in", false);
    }
    was = powered;
}

/*
 * Low power for WIFI_NAP_MS: Wi-Fi off, since keeping it associated costs
 * more than the rest of the chip. It rejoins when the screen wakes or USB
 * power arrives; meanwhile nothing reaches the device over the network.
 * Not while a voice note recorded offline waits to go (for a while). Returns
 * true while napping.
 */
static bool update_wifi_nap(TickType_t now, bool paused)
{
    static TickType_t low_since;
    static bool napping;
    if (!s_cpu_low) {
        low_since = now;
    }
    if (!muse_state_asleep() && s_nap_now) {
        s_nap_now = false;
        muse_state_set_as_if_battery(false);
    }
    bool nap = paused && !muse_voice_notes_waiting()
               && (s_nap_now || (s_cpu_low && now - low_since >= pdMS_TO_TICKS(WIFI_NAP_MS)));
    if (nap != napping) {
        napping = nap;
        ESP_LOGI(TAG, "Wi-Fi %s", nap ? "napping" : "waking");
        muse_wifi_nap(nap);
    }
    return napping;
}

static void input_task(void *arg)
{
    (void)arg;
    bool aux_down = false;
    bool paused = false;
    TickType_t checked = xTaskGetTickCount() - pdMS_TO_TICKS(SLEEP_CHECK_MS);
    TickType_t powered = xTaskGetTickCount() - pdMS_TO_TICKS(POWER_MS);

    for (;;) {
        unsigned ev = muse_board->poll_buttons();
        if (ev & (MUSE_BTN_TALK_PRESS | MUSE_BTN_TALK_RELEASE)) {
            ESP_LOGI(TAG, "talk key:%s%s", ev & MUSE_BTN_TALK_PRESS ? " press" : "",
                     ev & MUSE_BTN_TALK_RELEASE ? " release" : "");
            talk_button(ev);
        }
        keyboard_buttons(ev);
        volume_keys(ev);
        /* A latched key (the 1.75's PMU) can report press and release in the
         * same poll, and a release can land just before the next press; keep
         * them ordered, as talk_button does. */
        bool aux_press = ev & MUSE_BTN_AUX_PRESS;
        bool aux_release = ev & MUSE_BTN_AUX_RELEASE;
        bool aux_edge = false;
        if (aux_down && aux_release) {
            aux_down = false;
            aux_release = false;
            aux_edge = true;
            aux_key(false, true);
        }
        if (!aux_down && aux_press) {
            aux_down = aux_edge = true;
            aux_key(true, true);
            if (aux_release) {
                aux_down = false;
                aux_key(false, true);
            }
        }
        if (!aux_edge) {
            aux_key(aux_down, false);
        }

        if (s_power_off_requested) {
            s_power_off_requested = false;
            power_off();
        }

        TickType_t now = xTaskGetTickCount();
        if (now - checked >= pdMS_TO_TICKS(SLEEP_CHECK_MS)) {
            checked = now;
            check_sleep();
        }

        if (now - powered >= pdMS_TO_TICKS(paused ? REST_POWER_MS : POWER_MS)) {
            powered = now;
            muse_power_t p = { .battery_pct = -1 };
            if (muse_board->read_power && muse_board->read_power(&p) == ESP_OK) {
                muse_state_set_power(&p);
                muse_battery_note_power(&p, muse_state_on_battery());
                note_plug(&p);
            }
        }

        paused = update_power();
        bool napping = update_wifi_nap(now, paused);
        muse_battery_note_state(muse_state_asleep(), s_cpu_low);
        /* Paused, anything but a button (a pairing prompt, an image) is
         * noticed within REST_WAIT_MS. Napping, nothing comes over the
         * network; USB power arriving is noticed within NAP_WAIT_MS. */
        if (paused && muse_board->wait_buttons) {
            muse_board->wait_buttons(napping ? NAP_WAIT_MS : REST_WAIT_MS);
        } else {
            vTaskDelay(pdMS_TO_TICKS(paused ? REST_POLL_MS : POLL_MS));
        }
    }
}

/* Reads the rest of a line into buf; false if it was too long (the rest is dropped). */
static bool read_line(char *buf, size_t cap)
{
    size_t len = 0;
    bool whole = true;
    uint8_t c;
    while (muse_console_getc(&c) && c != '\n') {
        if (c == '\r') {
            continue;
        }
        if (len < cap - 1) {
            buf[len++] = (char)c;
        } else {
            whole = false;
        }
    }
    buf[len] = '\0';
    return whole;
}

#if CONFIG_MUSE_HATCH
#define CHAT_OVER_SERIAL "true"

/*
 * "chat+=TEXT" adds a piece of a message and "chat=TEXT" adds the last piece
 * and sends it (TEXT escaped: \n \r \t \\). Every line is acknowledged, and the
 * host waits for that before the next: the driver drops what it has no room for.
 */
static char *s_chat;        /* the message so far */
static size_t s_chat_len;

static void chat_line(char *piece, bool last, bool whole)
{
    size_t n = muse_hatch_unescape(piece);
    if (!s_chat) {
        s_chat = heap_caps_malloc(CHAT_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_chat_len = 0;
    }
    const char *err = !whole ? "LINE TOO LONG" : !s_chat ? "OUT OF MEMORY" : s_chat_len + n >= CHAT_MAX ? "TOO LONG" : NULL;
    if (err) {
        free(s_chat);
        s_chat = NULL;
        muse_hatch_console("error", err, NULL);
        return;
    }
    memcpy(s_chat + s_chat_len, piece, n);
    s_chat_len += n;
    s_chat[s_chat_len] = '\0';
    muse_hatch_console("ack", NULL, "\"bytes\":%u", (unsigned)s_chat_len);
    if (last) {
        muse_hatch_text_turn(s_chat);   /* frees it */
        s_chat = NULL;
    }
}

/* Drops a half-sent message and ends a typed turn. */
static void chat_cancel(void)
{
    free(s_chat);
    s_chat = NULL;
    muse_hatch_text_cancel();
}
#else
#define CHAT_OVER_SERIAL "false"   /* no PSRAM: replies come over Link, and only short ones */
#endif

/*
 * Bench: puts the face in a mode ("face=thinking") until the voice path or
 * another "face=" moves it on. "face=happy" goes back to idle with the happy
 * hop, as a finished turn does. The rest are what Muse is up to
 * (muse_ui_bench_pose), held until the next "face=": "phone", "listen_phone",
 * "packages", "unbox", "assemble", "paint" and "toss" (a moment's painting,
 * the canvas tossed up into the cloud, then waiting on it) thinking, "tea",
 * "pajamas" and "brace" idle; "act:NAME" what Muse would be at for a
 * muse_activity.h activity (act:search, act:mail, act:reminder_cancel...).
 * "download" plays a made image's whole way in, made up: painting it, the
 * toss, the cloud, the push all at once, the boxes, unboxing, the picture
 * put together and held (or, with a photo put away lately, that one out of
 * the pocket).
 */
static void set_face(const char *name)
{
    static const char *const modes[MUSE_MODE_COUNT] = {
        [MUSE_MODE_BOOT] = "boot",
        [MUSE_MODE_IDLE] = "idle",
        [MUSE_MODE_LISTENING] = "listening",
        [MUSE_MODE_THINKING] = "thinking",
        [MUSE_MODE_SPEAKING] = "speaking",
        [MUSE_MODE_ERROR] = "error",
        [MUSE_MODE_OFF] = "off",
    };
    static const struct {
        const char *name;
        muse_ui_bench_t what;
        const char *mode;
    } poses[] = {
        { "phone", MUSE_UI_BENCH_PHONE, "thinking" },
        { "listen_phone", MUSE_UI_BENCH_LISTEN_PHONE, "thinking" },
        { "packages", MUSE_UI_BENCH_PACKAGES, "thinking" },
        { "download", MUSE_UI_BENCH_DOWNLOAD, "thinking" },
        { "unbox", MUSE_UI_BENCH_UNBOX, "thinking" },
        { "assemble", MUSE_UI_BENCH_ASSEMBLE, "thinking" },
        { "toss", MUSE_UI_BENCH_TOSS, "thinking" },
        { "tea", MUSE_UI_BENCH_TEA, "idle" },
        { "pajamas", MUSE_UI_BENCH_PAJAMAS, "idle" },
        { "brace", MUSE_UI_BENCH_BRACE, "idle" },
    };
    muse_state_poke();
    if (!strncmp(name, "act:", 4) || !strcmp(name, "paint")) {
        const char *what = !strcmp(name, "paint") ? "image" : name + 4;
        for (int a = 0; a < MUSE_ACTIVITY_COUNT; a++) {
            if (!strcmp(what, muse_activity_name((muse_activity_t)a))) {
                muse_ui_bench_activity((muse_activity_t)a);
                muse_state_set_mode(MUSE_MODE_IDLE);
                muse_state_set_mode(MUSE_MODE_THINKING);
                return;
            }
        }
        printf("@face.error unknown act \"%s\"\n", what);
        fflush(stdout);
        return;
    }
    muse_ui_bench_t pose = MUSE_UI_BENCH_NONE;
    for (size_t i = 0; i < sizeof(poses) / sizeof(poses[0]); i++) {
        if (!strcmp(name, poses[i].name)) {
            pose = poses[i].what;
            name = poses[i].mode;
        }
    }
    bool happy = !strcmp(name, "happy");
    if (happy) {
        name = "idle";   /* where a finished turn hops to */
    }
    for (int m = 0; m < MUSE_MODE_COUNT; m++) {
        if (!strcmp(name, modes[m])) {
            muse_ui_bench_pose(pose);
            muse_state_set_mode(MUSE_MODE_IDLE);   /* only idle leaves "off" */
            muse_state_set_mode((muse_mode_t)m);
            if (m == MUSE_MODE_THINKING && pose == MUSE_UI_BENCH_NONE) {
                muse_state_set_turn(MUSE_TURN_ANSWERED);   /* just thinking: no note on its way */
            }
            if (happy) {
                muse_state_make_happy();
            }
            return;
        }
    }
    printf("@face.error unknown face \"%s\": boot idle listening thinking speaking error off happy"
           " phone listen_phone packages download unbox assemble paint toss act:NAME tea pajamas brace\n", name);
    fflush(stdout);
}

/* "told_mode": the gadget mode a chat last heard ("desk", "night", "on_the_go"), or null. */
static void add_told_mode(cJSON *chat, int mode)
{
    const char *key = mode >= 0 ? muse_gadget_mode_key((muse_gadget_mode_t)mode) : NULL;
    if (key) {
        cJSON_AddStringToObject(chat, "told_mode", key);
    } else {
        cJSON_AddNullToObject(chat, "told_mode");
    }
}

/* {"chat":"main|gadget|named|new|custom","session_id":ID[,"name":NAME],"told_mode":MODE} for the chat picked now. */
static cJSON *chat_json(void)
{
    char sid[MUSE_CHAT_SID_MAX + 1], gadget[MUSE_CHAT_SID_MAX + 1], name[MUSE_CHAT_NAME_MAX + 1];
    muse_settings_chat_sid(sid);
    muse_settings_gadget_chat_sid(gadget);
    bool named = sid[0] && muse_settings_chat_name(sid, name);
    cJSON *chat = cJSON_CreateObject();
    cJSON_AddStringToObject(chat, "chat",
                            !sid[0] ? "main" : !strcmp(sid, gadget) ? "gadget" : named ? "named"
                            : muse_settings_chat_untitled(sid) ? "new" : "custom");
    cJSON_AddStringToObject(chat, "session_id", sid);
    if (named) {
        cJSON_AddStringToObject(chat, "name", name);
    }
    add_told_mode(chat, muse_settings_chat_told(sid));
    return chat;
}

/* Prints "@<tag> <json>" and frees the json. */
static void print_json(const char *tag, cJSON *json)
{
    char *text = json ? cJSON_PrintUnformatted(json) : NULL;
    if (text) {
        printf("@%s %s\n", tag, text);
        cJSON_free(text);
    }
    cJSON_Delete(json);
    fflush(stdout);
}

static void chat_error(const char *what, const char *why)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "input", what);
    cJSON_AddStringToObject(json, "error", why);
    print_json("chat_sid.error", json);
}

/*
 * The Muse chat turns go to:
 *   "chat_sid"            prints it
 *   "chat_sid=ID"         picks a side chat by its UUID ("chat_sid=" or "=main"
 *                         the main one, "=gadget" this gadget's own)
 *   "chat_new="           a new chat the Muse titles by its first message, picked
 *   "chat_new=NAME"       keeps a new named chat and picks it (or picks the
 *                         one with that name)
 *   "chat_forget=ID|NAME" forgets a named chat (the main one is picked if it
 *                         was this one)
 *   "chat_sub=0" or "=1"  whether the reply subscription names it
 *                         (muse_chat_set_subscribe_session)
 * Each answers with
 *   @chat_sid {"chat":"main|gadget|named|new|custom","session_id":ID,"name":NAME,"told_mode":MODE,
 *              "subscribe_session":BOOL}
 * or "@chat_sid.error" {"input":...,"error":...}. "chats" lists the named ones:
 *   @chats {"current":{...},"chats":[{"name":NAME,"session_id":ID,"told_mode":MODE},...]}
 * MODE is the gadget mode that chat last heard ("desk", "night", "on_the_go"), or null.
 */
static void chat_sid_command(const char *line)
{
    if (!strcmp(line, "chats")) {
        muse_chat_entry_t chats[MUSE_CHATS_MAX];
        int n = muse_settings_chats(chats, MUSE_CHATS_MAX);
        cJSON *json = cJSON_CreateObject();
        cJSON_AddItemToObject(json, "current", chat_json());
        cJSON *list = cJSON_AddArrayToObject(json, "chats");
        for (int i = 0; i < n; i++) {
            cJSON *chat = cJSON_CreateObject();
            cJSON_AddStringToObject(chat, "name", chats[i].name);
            cJSON_AddStringToObject(chat, "session_id", chats[i].sid);
            add_told_mode(chat, chats[i].told_mode);
            cJSON_AddItemToArray(list, chat);
        }
        print_json("chats", json);
        return;
    }
    if (!strncmp(line, "chat_sub=", 9)) {
        muse_chat_set_subscribe_session(strcmp(line + 9, "0") != 0);
    } else if (!strcmp(line, "chat_new=")) {
        /* No name: a new chat the Muse titles by its first message, as on the Chats screen. */
        if (muse_settings_chat_pick_new() == ESP_ERR_NO_MEM) {
            chat_error("", "8 named chats are kept already: forget one first");
            return;
        }
    } else if (!strncmp(line, "chat_new=", 9)) {
        esp_err_t err = muse_settings_chat_new(line + 9, NULL);
        if (err == ESP_ERR_NO_MEM) {
            chat_error(line + 9, "8 named chats are kept already: forget one first");
            return;
        }
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            chat_error(line + 9, "a name is 1 to 32 bytes, without control characters");
            return;
        }
    } else if (!strncmp(line, "chat_forget=", 12)) {
        char sid[MUSE_CHAT_SID_MAX + 1];
        const char *want = line + 12;
        if (muse_settings_chat_find(want, sid)) {
            want = sid;
        }
        if (!muse_settings_chat_forget(want)) {
            chat_error(line + 12, "no named chat has that name or id");
            return;
        }
    } else if (line[8] == '=') {
        const char *want = line + 9;
        char gadget[MUSE_CHAT_SID_MAX + 1];
        muse_settings_gadget_chat_sid(gadget);
        if (!strcmp(want, "main")) {
            want = "";
        } else if (!strcmp(want, "gadget")) {
            want = gadget;
        }
        if (!muse_settings_set_chat_sid(want)) {
            chat_error(want, "use main, gadget or a UUID (8-4-4-4-12 hex digits)");
            return;
        }
    }
    cJSON *json = chat_json();
    cJSON_AddBoolToObject(json, "subscribe_session", muse_chat_subscribe_session());
    print_json("chat_sid", json);
}

/*
 * Console-only commands; false for setup commands. Their buffers are taken
 * per command: without PSRAM, static ones would hold internal RAM for good.
 */
static bool console_command(char *line, bool whole)
{
    if (!strcmp(line, "status")) {
        size_t cap = 1024;   /* long SSID, host and VM names escaped: past 512 */
        char *json = heap_caps_malloc(cap, MUSE_BIG_CAPS);
        if (json) {
            muse_ble_status_json(json, cap);
            printf("@status {\"board\":\"%s\",\"chat\":%s,\"device\":%s}\n", muse_board->name,
                   CHAT_OVER_SERIAL, json);
            fflush(stdout);
            free(json);
        }
        return true;
    }
    bool reset = !strcmp(line, "power.reset");
    if (reset || !strcmp(line, "power")) {
        if (reset) {
            muse_battery_reset();
        }
        const char *saved = muse_battery_saved_json();
        if (saved) {
            printf("@power.saved %s\n", saved);
        }
        size_t cap = 2048;   /* two dozen power locks */
        char *json = heap_caps_malloc(cap, MUSE_BIG_CAPS);
        if (json) {
            muse_battery_json(json, cap);
            printf("@power %s\n", json);
            free(json);
        }
        fflush(stdout);
        return true;
    }
    if (!strcmp(line, "nap")) {
        muse_state_set_as_if_battery(true);
        set_asleep(true, "serial");
        s_nap_now = true;
        return true;
    }
    if (!strcmp(line, "quake")) {
        muse_ui_quake();   /* as a shake of the board would */
        return true;
    }
    if (!strcmp(line, "charge")) {
        cheer_plugged("serial", true);   /* as plugging in would */
        return true;
    }
    if (!strncmp(line, "batt=", 5)) {
        /* The face as if the battery were at N% ("batt=15"), charging
         * ("batt=15c"), or as it is again ("batt=off"). */
        char *end;
        long pct = strtol(line + 5, &end, 10);
        bool off = !strcmp(line + 5, "off");
        if (!off && (end == line + 5 || pct < 0 || pct > 100 || (*end && strcmp(end, "c") != 0))) {
            printf("@batt.error \"%s\": batt=0..100, with c for charging, or batt=off\n", line + 5);
        } else {
            muse_ui_fake_battery(off ? -1 : (int)pct, !off && *end == 'c');
            muse_state_poke();
            printf("@batt %s\n", off ? "off" : line + 5);
        }
        fflush(stdout);
        return true;
    }
#if CONFIG_MUSE_GADGET_UP_NEXT
    if (!strcmp(line, "brief")) {
        muse_up_next_refresh();
        printf("@brief {\"asking\":true}\n");
        fflush(stdout);
        return true;
    }
    if (!strcmp(line, "brief?")) {
        muse_up_next_print();
        return true;
    }
#endif
#if CONFIG_MUSE_HATCH
    if (!strncmp(line, "say=", 4) && line[4]) {
        /* Typed words as a voice turn, on the face: a talk press and release, the mic's audio dropped. */
        char *words = strdup(line + 4);
        muse_ui_bench_pose(MUSE_UI_BENCH_NONE);   /* a real turn: no bench face left over */
        if (words) {
            muse_hatch_unescape(words);
            muse_hatch_typed_voice(words);
            muse_state_poke();
            for (int i = 0; i < 40 && muse_state_asleep(); i++) {
                vTaskDelay(pdMS_TO_TICKS(50));   /* awake first: a press that wakes it is only a wake */
            }
            vTaskDelay(pdMS_TO_TICKS(300));
            post(MUSE_PTT_DOWN, false);
            vTaskDelay(pdMS_TO_TICKS(600));   /* past a tap's length */
            post(MUSE_PTT_UP, false);
        }
        return true;
    }
    if (!strncmp(line, "widget=", 7)) {
        /* A sample widget (muse_widget_sample), as if the last reply had brought it; "none" takes it away. */
        const char *json = muse_widget_sample(line + 7);
        bool none = !strcmp(line + 7, "none");
        bool browser = !strcmp(line + 7, "browser");   /* Muse's browser at work for ~8 s (muse_browse_bench) */
        muse_ui_bench_pose(MUSE_UI_BENCH_NONE);
        muse_state_poke();
        if (none) {
            muse_widget_clear();
            muse_browse_turn();
        }
        if (browser) {
            muse_browse_bench();
        }
        if (none || browser || (json && muse_hatch_widget_bench(json))) {
            printf("@widget {\"widget\":\"%s\"}\n", line + 7);
        } else {
            printf("@widget.error \"%s\": option, options, list, map, localmap, shopping, text, multi, card, "
                   "browser or none\n", line + 7);
        }
        fflush(stdout);
        return true;
    }
#endif
    if (!strncmp(line, "face=", 5)) {
        set_face(line + 5);
        return true;
    }
    if (!strncmp(line, "activity=", 9)) {
        /* As if Muse said he's at this (agent.status's activity_text): what it's taken for, and shown. */
        muse_activity_t a = muse_activity_of(NULL, line + 9);
        printf("@activity {\"activity\":\"%s\"}\n", muse_activity_name(a));
        fflush(stdout);
        muse_state_poke();
        muse_ui_bench_activity(a);
        muse_state_set_mode(MUSE_MODE_IDLE);
        muse_state_set_mode(MUSE_MODE_THINKING);
        return true;
    }
    if (!strcmp(line, "chat_sid") || !strcmp(line, "chats") || !strncmp(line, "chat_sid=", 9)
        || !strncmp(line, "chat_sub=", 9) || !strncmp(line, "chat_new=", 9) || !strncmp(line, "chat_forget=", 12)) {
        chat_sid_command(line);
        return true;
    }
    if (strncmp(line, "chat", 4) != 0) {
        return false;
    }
#if CONFIG_MUSE_HATCH
    if (!strcmp(line, "chat.cancel")) {
        chat_cancel();
        return true;
    }
    bool last = !strncmp(line, "chat=", 5);
    if (last || !strncmp(line, "chat+=", 6)) {
        chat_line(line + (last ? 5 : 6), last, whole);
        return true;
    }
    return false;
#else
    (void)whole;
    muse_hatch_console("error", "THIS BOARD CAN'T CHAT OVER SERIAL", NULL);
    return true;
#endif
}

/*
 * Bench testing over the USB cable: 'd' / 'u' act as the talk button going
 * down / up, so the voice path can be driven without a finger on the button;
 * 'm' plays a built-in MP3 through the reply decoder; 'a' / 's' press the
 * menu's Down / Select; 'p' sends a screenshot; 'z' / 'w' sleep and wake.
 * A line starting with '>' is a setup command, the same "key=value" text as
 * the BLE CMD characteristic, or one of the console's own: "status" prints
 * the device's state, "power" the battery meter (muse_battery.h) and
 * "power.reset" starts it over, "nap" sleeps and leaves Wi-Fi at once (as
 * two minutes asleep on battery would; 'w' rejoins), "quake" shakes Muse
 * up as a shake of the board does, "charge" cheers as plugging in does
 * (cheer_plugged), "brief" asks the Muse for the face's
 * "up next" line now and "brief?" prints it (muse_up_next.h), "face=" shows a face
 * (see set_face), "activity=TEXT" shows Muse at what he'd be at if he said
 * TEXT (muse_activity_of) and prints what it's taken for, "widget=NAME" shows
 * a sample widget (option, options, list, map, localmap, shopping, text,
 * multi, card; none takes it away) as if a reply had brought it, or Muse's
 * browser at work ("browser"), "chat=" sends a typed message to Hatch (see chat_line
 * and tools/muse/chat.py), and "chat_sid=", "chat_new=" and "chats" pick
 * the chat it goes to and list the named ones (see chat_sid_command).
 */
static void serial_task(void *arg)
{
    if (muse_console_install(SERIAL_RX) != ESP_OK) {
        vTaskDelete(NULL);
    }
    for (;;) {
        uint8_t c;
        if (!muse_console_getc(&c)) {
            continue;
        }
        if (c == 'm') {
            muse_voice_request_mp3test();
        } else if (c == 'a' || c == 's') {
            muse_state_poke();
            muse_menu_key(c == 'a' ? MUSE_MENU_DOWN : MUSE_MENU_SELECT);
        } else if (c == 'p') {
            muse_ui_request_snapshot();
        } else if (c == 'z' || c == 'w') {
            set_asleep(c == 'z', "serial");
        } else if (c == '>') {
            char *line = heap_caps_malloc(SERIAL_LINE, MUSE_BIG_CAPS);
            char none[1];   /* no room: the line is read and dropped */
            bool whole = read_line(line ? line : none, line ? SERIAL_LINE : sizeof(none));
            if (line && !console_command(line, whole)) {
                muse_ble_command(line);
            }
            free(line);
        } else if (c == 'd' || c == 'u') {
            muse_ui_bench_pose(MUSE_UI_BENCH_NONE);
            muse_state_poke();
            post(c == 'd' ? MUSE_PTT_DOWN : MUSE_PTT_UP, false);
        }
    }
}

esp_err_t muse_input_start(QueueHandle_t queue)
{
    s_queue = queue;
    if (xTaskCreate(input_task, "muse_input", 4096, NULL, 6, &s_input) != pdPASS) {
        return ESP_FAIL;
    }
    /* Bench-test and setup console; the input still works if it can't start. */
    xTaskCreate(serial_task, "muse_serial", 3584, NULL, 5, NULL);
    return ESP_OK;
}

void muse_input_request_power_off(void)
{
    s_power_off_requested = true;
}
