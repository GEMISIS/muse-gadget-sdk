# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Muse: the voice companion UI that runs beside Home Link on boards with a
# display (CONFIG_MUSE_BOARD_*). Link-only builds register an empty component,
# except the Voice PE, which borrows the Hatch voice session.
#
# Requirements are resolved before Kconfig is known, so everything any board
# needs is listed unconditionally. Managed components (LVGL, codecs, BSPs) come
# from idf_component.yml, where rules keep them out of Link-only builds.
set(srcs)
set(embed)
if(CONFIG_MUSE_ENABLED)
    list(APPEND srcs
        "muse_app.c"
        "muse_board.c"
        "muse_console.c"
        "muse_state.c"
        "muse_battery.c"
        "muse_ui.c"
        "muse_audio.c"
        "muse_pmu.c"
        "muse_input.c"
        "muse_menu.c"
        "muse_power_menu.c"
        "muse_gadget_mode.c"
        "muse_voice.c"
        "muse_adpcm.c"
        "muse_settings.c"
        "muse_settings_ui.c"
        "muse_dialog.c"
        "muse_style.c"
        "muse_text.c"
        "muse_keypad.c"
        "muse_lv_mem.c"
        "muse_link.c"
        "muse_ble.c")
    if(CONFIG_MUSE_HATCH)
        list(APPEND srcs "muse_chat.c" "muse_account_api.c" "muse_chat_session.cpp")
        list(APPEND srcs "muse_present.c")   # images in replies, held up on the face
        list(APPEND srcs "muse_widget.c")   # widgets in replies (options, lists...)
        if(CONFIG_MUSE_PRESENT_FORMATS)
            list(APPEND srcs "muse_image.c")   # their PNG and WebP
        endif()
        list(APPEND embed "test_reply.mp3")   # muse_hatch_mp3_selftest
        if(CONFIG_MUSE_TTS_PICO)
            list(APPEND srcs "muse_tts.c" "muse_tts_text.c")   # replies spoken on the device
        endif()
    else()
        list(APPEND srcs "muse_chat_link.c")   # text replies over Link's session
    endif()
    list(APPEND srcs "muse_chat_text.c")
    if(CONFIG_MUSE_CJK_FONT)
        list(APPEND srcs "fonts/muse_font_cjk_16.c")   # captions' CJK fallback
    endif()
    # Your own character (tools/muse/AVATAR_RECIPE.md) stands in for the default
    # avatar when avatar/muse_pixel.c exists here; this avatar/ is gitignored. The
    # default is in avatar/ at the top of the project.
    file(GLOB custom_avatar CONFIGURE_DEPENDS "${COMPONENT_DIR}/avatar/muse_pixel.c")
    if(custom_avatar)
        message(STATUS "Custom avatar: components/muse/avatar/muse_pixel.c")
        list(APPEND srcs "avatar/muse_pixel.c")
    else()
        list(APPEND srcs "../../avatar/muse_pixel.c")
    endif()
    if(CONFIG_MUSE_BOARD_WAVESHARE_S3_175C OR CONFIG_MUSE_BOARD_WAVESHARE_S3_175)
        # One driver: the 1.75 differs only in pins (its BSP's) and its PWR key.
        list(APPEND srcs "boards/board_waveshare_s3_175c.c" "boards/muse_lcd_bands.c")
    elseif(CONFIG_MUSE_BOARD_WAVESHARE_S3_216)
        list(APPEND srcs "boards/board_waveshare_s3_216.c" "boards/muse_lcd_bands.c")
        if(CONFIG_MUSE_HATCH)
            list(APPEND srcs "muse_widget_ui.c")   # their sheet on the face
        endif()
        if(CONFIG_MUSE_GADGET_EXTRAS)
            # microSD, RTC, IMU and the face's corner readouts (Kconfig.gadget)
            list(APPEND srcs "muse_extras.c" "muse_sd.c" "muse_rtc.c" "muse_imu.c"
                             "muse_home_extras.c" "fonts/muse_font_clock_72.c")
            if(CONFIG_MUSE_GADGET_AUTO_FLIP)
                list(APPEND srcs "muse_orient.c")   # upside down at night
            endif()
            if(CONFIG_MUSE_GADGET_UP_NEXT)
                list(APPEND srcs "muse_up_next.c")   # the face's "up next" line
            endif()
        endif()
        if(CONFIG_MUSE_GADGET_CHATS)
            list(APPEND srcs "muse_chats_ui.c")   # the Chats screen, left of Muse
            if(CONFIG_MUSE_HATCH)
                list(APPEND srcs "muse_chat_delete.c")   # its "Forget and delete from Muse"
            endif()
        endif()
    elseif(CONFIG_MUSE_BOARD_AIPI)
        list(APPEND srcs "boards/board_aipi.c")
    elseif(CONFIG_MUSE_BOARD_ESPRESSIF_BOX_3)
        list(APPEND srcs "boards/board_espressif_box_3.c" "boards/box_3_microphone.c")
    elseif(CONFIG_MUSE_BOARD_WAVESHARE_C6_18)
        list(APPEND srcs "boards/board_waveshare_c6_18.c")
    elseif(CONFIG_MUSE_BOARD_WAVESHARE_C6_206)
        list(APPEND srcs "boards/board_waveshare_c6_206.c")
    elseif(CONFIG_MUSE_BOARD_SENSECAP_WATCHER)
        list(APPEND srcs "boards/board_sensecap_watcher.c" "boards/muse_lcd_bands.c")
        if(CONFIG_MUSE_WATCHER_CAMERA)
            list(APPEND srcs "boards/watcher_camera.c")
        endif()
    elseif(CONFIG_MUSE_BOARD_M5STACK_CARDPUTER_ADV)
        list(APPEND srcs "boards/board_m5stack_cardputer_adv.c")
    elseif(CONFIG_MUSE_BOARD_M5STACK_STICKS3)
        list(APPEND srcs "boards/board_m5stack_sticks3.c")
    elseif(CONFIG_MUSE_BOARD_M5STACK_STICKC_PLUS2)
        list(APPEND srcs "boards/board_m5stack_stickc_plus2.c")
    elseif(CONFIG_MUSE_BOARD_M5STACK_STOPWATCH)
        list(APPEND srcs "boards/board_m5stack_stopwatch.c" "boards/muse_lcd_bands.c")
    elseif(CONFIG_MUSE_BOARD_M5STACK_CORES3)
        list(APPEND srcs "boards/board_m5stack_cores3.c")
    elseif(CONFIG_MUSE_BOARD_M5STACK_CORE2)
        list(APPEND srcs "boards/board_m5stack_core2.c")
    elseif(CONFIG_MUSE_BOARD_FNK0104B)
        list(APPEND srcs "boards/board_fnk0104b.c")
    elseif(CONFIG_MUSE_BOARD_GUITION_JC3248W535)
        list(APPEND srcs "boards/board_guition_jc3248w535.c")
    elseif(CONFIG_MUSE_BOARD_WAVESHARE_S3_LCD7)
        list(APPEND srcs "boards/board_waveshare_s3_lcd7.c")
    endif()
elseif(CONFIG_HOMEHUB_VOICE)
    # The Voice PE board uses just the Hatch voice session; the project's main/
    # voice_muse_chat.c stands in for the Muse settings, Wi-Fi and Link hooks.
    list(APPEND srcs "muse_chat.c" "muse_account_api.c" "muse_chat_session.cpp" "muse_chat_text.c" "muse_text.c"
                     "muse_widget.c")
    list(APPEND embed "test_reply.mp3")
endif()

set(muse_priv_requires
    esp_timer esp_driver_usb_serial_jtag esp_psram nvs_flash
    bt esp-tls mbedtls esp_app_format esp_http_client
    noise_core minimp3 picotts
    esp_lcd esp_adc esp_driver_i2s esp_driver_ledc esp_driver_spi esp_driver_uart esp_wifi
    esp_driver_pcnt
    fatfs sdmmc esp_driver_sdmmc esp_netif lwip)   # the 2.16's extras: microSD, SNTP
list(APPEND muse_priv_requires camera)
# A reply's WebP images (muse_image.c): libwebp builds nothing unless
# CONFIG_MUSE_PRESENT_FORMATS, and requirements can't wait on the config.
# libpng comes in through idf_component.yml, on the boards that have it.
list(APPEND muse_priv_requires libwebp)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    EMBED_FILES ${embed}
    WHOLE_ARCHIVE   # muse_lv_mem.c provides symbols only LVGL references
    REQUIRES esp_driver_gpio esp_driver_i2c
    PRIV_REQUIRES ${muse_priv_requires}
)

if(CONFIG_MUSE_ENABLED)
    # LVGL spins forever on a failed assert unless LV_ASSERT_HANDLER is set,
    # which its Kconfig can't do. Abort, so the panic handler prints a backtrace
    # and reboots. PUBLIC reaches LVGL and every file that includes lvgl.h.
    idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
    target_compile_definitions(${lvgl_lib} PUBLIC
        "LV_ASSERT_HANDLER_INCLUDE=<stdlib.h>"
        "LV_ASSERT_HANDLER=abort()\;")
endif()
