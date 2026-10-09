<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# picotts

SVOX Pico, the text-to-speech engine from Android, for replies spoken on the
device (`CONFIG_MUSE_TTS_PICO`, see `components/muse/muse_tts.h`). Built only
when that option is on; other builds register an empty component.

| | |
|---|---|
| Upstream | DiUS's ESP-IDF port, https://github.com/DiUS/esp-picotts, published as `jmattsson/picotts` 1.1.3 in the Espressif component registry (2024-11-14). Its `pico/` is SVOX's Pico as Android released it |
| License | Apache-2.0. SVOX's text is in [`pico/lib/NOTICE`](pico/lib/NOTICE) |

## What's upstream and what's ours

| File | From | License |
|---|---|---|
| `pico/lib/*` | upstream (SVOX via DiUS), unmodified | Apache-2.0 |
| `pico/lang/en-US_ta.bin`, `pico/lang/en-US_lh0_sg.bin` | upstream, unmodified: the en-US voice | Apache-2.0 |
| `esp_picorsrc.c`, `esp_picorsrc.h` | DiUS, unmodified: loads the voice in place from flash | Apache-2.0 |
| `picotts_engine.c`, `include/picotts_engine.h` | Meta, after DiUS's `esp_picotts.c`: setup and teardown, no task of its own | Apache-2.0 |
| `CMakeLists.txt`, `README.md` | Meta | Apache-2.0 |

Don't edit or restyle the upstream files or give them a Meta header. Their
build options are set in `CMakeLists.txt`: C17, warnings off, and Pico's
`picoos_quick_exp` swapped for `exp()` (its trick doesn't hold on Xtensa).

## The voice

The two `.bin` files (1.4 MB) aren't in the app image. They go into the
`tts_ta` and `tts_sg` partitions of `partitions_muse_tts.csv`, which
`idf.py flash` and `tools/muse/board.sh flash` write with the app, and are
read in place through the flash cache. OTA updates never touch them. A board
without those partitions keeps showing replies silently, as before.

The engine itself needs 1.1 MB of working memory, taken from PSRAM while a
reply is spoken and freed after five idle minutes.

## Updating, or another language

1. Copy `pico/lib/` and `esp_picorsrc.*` from the new upstream release,
   unchanged, and check its license is still Apache-2.0.
2. For another language, copy its `<lang>_ta.bin` and `<lang>_<voice>_sg.bin`
   from upstream's `pico/lang/` and change the two names in `CMakeLists.txt`.
   The partitions fit any of upstream's languages.
3. Update the version and date above.
