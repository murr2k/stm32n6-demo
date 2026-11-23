# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

# The camera module in the CSI connector: ST B-CAMS-IMX (MB1854, IMX335).
set(SHIELD st_b_cams_imx_mb1854)

# zprof (https://github.com/murr2k/zprof), the tracer, is part of the build: its Zephyr
# module and the demo's settings for it (zprof.conf). West fetches it into zprof/; its
# module sits in firmware/zprof, where module discovery does not look, so it is added
# here. -DZPROF=OFF (n6.cmd --no-zprof) builds without it: the SystemView build, and the
# observer A/B. -DZPROF_DIR=... (n6.cmd --zprof DIR) takes another checkout.
set(zprof_default_dir ${CMAKE_CURRENT_LIST_DIR}/../zprof)
option(ZPROF "zprof, the tracer: its module from ZPROF_DIR and zprof.conf" ON)
set(ZPROF_DIR ${zprof_default_dir} CACHE PATH "zprof checkout (west update fetches it)")
# Under sysbuild, -Dapp_ZPROF and -Dapp_ZPROF_DIR reach this image only in its
# sysbuild cache, which Zephyr's zephyr_get() reads only after find_package: read
# them here. Not given means the default, so a build's choice never sticks.
if(SYSBUILD AND EXISTS "${SYSBUILD_CACHE}")
  get_filename_component(zprof_img "${SYSBUILD_CACHE}" NAME)
  string(REGEX REPLACE "_sysbuild_cache\\.txt$" "" zprof_img "${zprof_img}")
  file(STRINGS "${SYSBUILD_CACHE}" zprof_sb REGEX "^${zprof_img}_ZPROF(_DIR)?:" ENCODING UTF-8)
  set(ZPROF ON)
  set(ZPROF_DIR ${zprof_default_dir})
  foreach(zprof_line ${zprof_sb})
    if(zprof_line MATCHES "^${zprof_img}_ZPROF:[^=]*=(.*)$")
      set(ZPROF ${CMAKE_MATCH_1})
    elseif(zprof_line MATCHES "^${zprof_img}_ZPROF_DIR:[^=]*=(.*)$")
      set(ZPROF_DIR ${CMAKE_MATCH_1})
    endif()
  endforeach()
endif()
if(ZPROF)
  if(NOT EXISTS ${ZPROF_DIR}/firmware/zprof/zephyr/module.yml)
    message(FATAL_ERROR "no zprof at ${ZPROF_DIR}: run west update (or build with -DZPROF=OFF)")
  endif()
  list(APPEND ZEPHYR_EXTRA_MODULES ${ZPROF_DIR}/firmware/zprof)
  list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/zprof.conf)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(n6_demo)

file(GLOB_RECURSE app_sources CONFIGURE_DEPENDS src/*.c)
target_sources(app PRIVATE ${app_sources})
target_include_directories(app PRIVATE src)

# Zephyr's symbol table (CONFIG_SYMTAB) is generated from the first link and
# filled in only in the final one, and its section lies before rodata: in the
# final image everything after it moves up by the table's size (27.8 KB
# here). The interrupt table (isr_tables.c) is generated from the same first
# link with each handler's argument as a literal address, so an argument that
# points into rodata points at something else in the final image: the board
# hung at boot in the EXTI3 interrupt, whose line range (line_range_3, from
# Zephyr's GPIO interrupt driver) was read from a table of doubles
# (2026-11-20). Such arguments go to rom_start, ahead of the table;
# tools/n6.py checks every build for arguments that moved.
if(CONFIG_SYMTAB)
  zephyr_linker_sources(ROM_START SORT_KEY zz_isr_args linker/isr_args.ld)
endif()

# The recorder (src/media/recorder.c) puts its own code in the path between
# ST's encoder library and the Zephyr VENC driver's EWL layer, at link time,
# so the Zephyr tree stays unmodified: the cache is cleaned as the hardware
# starts (EWLEnableHW), the wait for the hardware is longer (EWLWaitHwRdy),
# and the linear memory bypasses the driver's chunk table, which overflows
# after a few encoder starts (EWLMallocLinear and friends).
if(CONFIG_VIDEO_STM32_VENC)
  zephyr_ld_options(
    -Wl,--wrap=EWLEnableHW -Wl,--wrap=EWLWaitHwRdy
    -Wl,--wrap=EWLMallocLinear -Wl,--wrap=EWLFreeLinear
    -Wl,--wrap=EWLMallocRefFrm -Wl,--wrap=EWLFreeRefFrm)
endif()

# The microSD card (src/media/sdcard.c): switched to High Speed (CMD6)
# between Zephyr's driver's HAL_SD_Init() and its 4-bit setup, so that its bus
# may run at 48 MHz; a data timeout of 1 s instead of 89, and no endless wait
# for the card's state, so that a card taken out mid-transfer fails at once.
zephyr_ld_options(-Wl,--wrap=HAL_SD_Init -Wl,--wrap=SDMMC_ConfigData
                  -Wl,--wrap=HAL_SD_GetCardState)

# The web remote's page, gzipped into the image; src/platform/http.c serves
# it at / with Content-Encoding: gzip.
generate_inc_file_for_target(app ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/remote.html
  ${ZEPHYR_BINARY_DIR}/include/generated/remote.html.gz.inc --gzip)

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

# The NPU: ST's runtime (ll_aton, from the ST Edge AI install) and the
# networks tools/nn.py converts into build-nn/. Both are under ST's licence
# (SLA0044), which forbids open-source terms, so neither is in this
# repository; without them the app builds with the NPU features left out
# (APP_HAS_NPU undefined).
set(NN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../build-nn)
if(DEFINED ENV{STEDGEAI_CORE_DIR})
  set(STEDGEAI_DIR $ENV{STEDGEAI_CORE_DIR})
else()
  set(STEDGEAI_DIR C:/ST/STEdgeAI/4.0)
endif()
set(ATON ${STEDGEAI_DIR}/Middlewares/ST/AI)
set(NN_NETS palm_detector hand_landmark)
if(CONFIG_APP_NPU AND EXISTS ${ATON}/Npu/ll_aton/ll_aton.c AND EXISTS ${NN_DIR}/nn_weights.h)
  message(STATUS "NPU: networks from ${NN_DIR}, runtime from ${ATON}")
  set(nn_defs
    LL_ATON_PLATFORM=LL_ATON_PLAT_STM32N6
    LL_ATON_OSAL=LL_ATON_OSAL_ZEPHYR
    LL_ATON_RT_MODE=LL_ATON_RT_ASYNC
    LL_ATON_SW_FALLBACK
    LL_ATON_DBG_BUFFER_INFO_EXCLUDED=1
    LL_ATON_EB_DBG_INFO
    APP_HAS_PARALLEL_NETWORKS=0)
  set(nn_incs ${ATON}/Inc ${ATON}/Npu/ll_aton ${ATON}/Npu/Devices/STM32N6xx ${NN_DIR})
  foreach(net ${NN_NETS})
    list(APPEND nn_incs ${NN_DIR}/${net})
  endforeach()

  set(nn_sources)
  set(nn_net_sources)
  foreach(f ll_aton.c ll_aton_osal_zephyr.c ll_aton_debug.c ll_aton_lib.c ll_aton_lib_sw_operators.c
            ll_aton_rt_main.c ll_aton_runtime.c ll_aton_cipher.c ll_aton_stai_internal.c ll_aton_util.c
            ll_sw_float.c ll_sw_integer.c ecloader.c)
    list(APPEND nn_sources ${ATON}/Npu/ll_aton/${f})
  endforeach()
  list(APPEND nn_sources ${ATON}/Npu/Devices/STM32N6xx/npu_cache.c ${ATON}/Npu/Devices/STM32N6xx/mcu_cache.c)
  foreach(net ${NN_NETS})
    list(APPEND nn_net_sources ${NN_DIR}/${net}/${net}.c ${NN_DIR}/${net}/stai_${net}.c)
  endforeach()
  target_sources(app PRIVATE ${nn_sources} ${nn_net_sources})
  # vendor and generated code: not ours to fix
  set_source_files_properties(${nn_sources} PROPERTIES COMPILE_OPTIONS -w)
  set_source_files_properties(${nn_net_sources} PROPERTIES COMPILE_OPTIONS
    "-w;-include;${CMAKE_CURRENT_SOURCE_DIR}/src/nn/st_sections.h")
  target_link_libraries(app PRIVATE ${ATON}/Lib/GCC/ARMCortexM55/NetworkRuntime1201_CM55_GCC.a)
  target_include_directories(app PRIVATE ${nn_incs})
  target_compile_definitions(app PRIVATE APP_HAS_NPU=1 ${nn_defs})
  # the test inputs (tools/nn.py testimg) are in the flash beside the weights
  # (tools/nn.py flash), not in the image: nn_weights.h says where
endif()
