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
  # test inputs made by tools/nn.py testimg, for tools/nn.py check
  if(EXISTS ${NN_DIR}/test/palm_in.bin AND EXISTS ${NN_DIR}/test/hand_in.bin)
    generate_inc_file_for_target(app ${NN_DIR}/test/palm_in.bin ${ZEPHYR_BINARY_DIR}/include/generated/nn_palm_in.inc)
    generate_inc_file_for_target(app ${NN_DIR}/test/hand_in.bin ${ZEPHYR_BINARY_DIR}/include/generated/nn_hand_in.inc)
    target_compile_definitions(app PRIVATE APP_NN_TEST_INPUT=1)
  endif()
endif()
