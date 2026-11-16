# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

# The camera module in the CSI connector: ST B-CAMS-IMX (MB1854, IMX335).
set(SHIELD st_b_cams_imx_mb1854)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(n6_demo)

file(GLOB_RECURSE app_sources CONFIGURE_DEPENDS src/*.c)
target_sources(app PRIVATE ${app_sources})
target_include_directories(app PRIVATE src)

# LVGL's Helium (MVE) blend routines. The Zephyr module build lists the C
# sources but not this assembly file, so without it CONFIG_LV_DRAW_SW_ASM_HELIUM
# fails at link time.
if(CONFIG_LV_DRAW_SW_ASM_HELIUM)
  set(lv_helium_asm ${ZEPHYR_LVGL_MODULE_DIR}/src/draw/sw/blend/helium/lv_blend_helium.S)
  target_sources(app PRIVATE ${lv_helium_asm})
  # Zephyr's lv_conf.h includes <string.h>, which is not valid in assembly.
  # Pre-defining its include guard skips it; the assembly only needs the
  # LV_* values that lv_conf_kconfig.h derives from Kconfig.
  set_source_files_properties(${lv_helium_asm} PROPERTIES
    COMPILE_DEFINITIONS ZEPHYR_MODULES_LVGL_LV_CONF_H_)
endif()
