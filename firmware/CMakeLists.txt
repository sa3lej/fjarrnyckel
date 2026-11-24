cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# TinyUSB's audio class (the box as a sound card for the radio audio). Espressif's esp_tinyusb has no switch for
# it, so it is turned on here, for every component, before TinyUSB reads its configuration.
# 48 kHz, 16-bit, mono, with a feedback endpoint so the computer follows the box's clock.
idf_build_set_property(COMPILE_DEFINITIONS "CFG_TUD_AUDIO=1" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "CFG_TUD_AUDIO_ENABLE_EP_OUT=1" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP=1" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX=98" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ=1568" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "CFG_TUD_AUDIO_CTRL_BUF_SZ=64" APPEND)
set(PROJECT_VER "0.11.0")
project(fjarrnyckel)
