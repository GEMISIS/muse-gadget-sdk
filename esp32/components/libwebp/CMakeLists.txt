# libwebp 1.5.0 (https://github.com/webmproject/libwebp, BSD-3-Clause: COPYING,
# PATENTS), decoding only: src/dec, and the plain-C parts of src/dsp and
# src/utils that libwebpdecoder builds from (their Makefile.am's
# COMMON_SOURCES), unchanged. No SIMD, no threads. For a reply's WebP image
# (components/muse/muse_image.c), only where CONFIG_MUSE_PRESENT_FORMATS asks.

if(NOT CONFIG_MUSE_PRESENT_FORMATS)
    idf_component_register()
    return()
endif()

file(GLOB webp_srcs
    "${CMAKE_CURRENT_LIST_DIR}/src/dec/*.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/dsp/*.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/utils/*.c")

idf_component_register(
    SRCS ${webp_srcs}
    INCLUDE_DIRS "."
    PRIV_REQUIRES heap
    LDFRAGMENTS "linker.lf")

# RGB565 in LVGL's (little-endian) byte order; speed over size for the decoder.
target_compile_definitions(${COMPONENT_LIB} PRIVATE WEBP_SWAP_16BIT_CSP=1 HAVE_CONFIG_H)   # src/webp/config.h
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wno-unused-function -Wno-unused-but-set-variable)
# Its allocations (src/utils/utils.c) go to PSRAM: the internal RAM can't spare them.
set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/src/utils/utils.c" PROPERTIES
    COMPILE_OPTIONS "-include;${CMAKE_CURRENT_LIST_DIR}/webp_psram.h")
