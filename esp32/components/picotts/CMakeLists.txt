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

# SVOX Pico text-to-speech (Apache-2.0), for replies spoken on the device
# (CONFIG_MUSE_TTS_PICO). See README.md. Other builds register an empty
# component. Requirements are resolved before Kconfig is known, so they're
# listed either way.
if(NOT CONFIG_MUSE_TTS_PICO)
    idf_component_register(PRIV_REQUIRES esp_partition)
    return()
endif()

set(pico_srcs
    picoacph.c picoapi.c picobase.c picocep.c picoctrl.c picodata.c picodbg.c
    picoextapi.c picofftsg.c picokdbg.c picokdt.c picokfst.c picoklex.c
    picoknow.c picokpdf.c picokpr.c picoktab.c picoos.c picopal.c picopam.c
    picopr.c picosa.c picosig.c picosig2.c picospho.c picotok.c picotrns.c
    picowa.c)
# picorsrc.c is built inside esp_picorsrc.c, which reads the voice in place.
list(TRANSFORM pico_srcs PREPEND "pico/lib/")

idf_component_register(
    SRCS "picotts_engine.c" "esp_picorsrc.c" ${pico_srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "." "pico/lib"
    PRIV_REQUIRES esp_partition)

# Upstream C (2009), left as it is: build it as C17, which it predates less
# than C23, and don't hold it to today's warnings.
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu17)
set_source_files_properties(${pico_srcs} "esp_picorsrc.c" PROPERTIES COMPILE_OPTIONS "-w")
# Pico's own exp() relies on an IEEE layout Xtensa doesn't use; picotts_engine.c
# supplies picoos_quick_exp in its place.
set_source_files_properties("pico/lib/picoos.c" PROPERTIES
    COMPILE_DEFINITIONS "picoos_quick_exp=picoos_quick_nope")

# The en-US voice goes into its own partitions (partitions_muse_tts.csv), so
# idf.py flash and board.sh flash write it with the app and OTA images stay
# their usual size.
foreach(part ta sg)
    if(part STREQUAL "ta")
        set(bin "${COMPONENT_DIR}/pico/lang/en-US_ta.bin")
    else()
        set(bin "${COMPONENT_DIR}/pico/lang/en-US_lh0_sg.bin")
    endif()
    partition_table_get_partition_info(size "--partition-name tts_${part}" "size")
    if(size)
        file(SIZE "${bin}" bin_size)
        math(EXPR size "${size}" OUTPUT_FORMAT DECIMAL)
        if(bin_size GREATER size)
            message(FATAL_ERROR "${bin} (${bin_size} bytes) doesn't fit partition tts_${part} (${size})")
        endif()
        esptool_py_flash_to_partition(flash "tts_${part}" "${bin}")
    else()
        message(WARNING "CONFIG_MUSE_TTS_PICO: no tts_${part} partition in the partition table; "
                        "replies stay silent. Use partitions_muse_tts.csv.")
    endif()
endforeach()
