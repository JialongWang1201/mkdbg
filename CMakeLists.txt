cmake_minimum_required(VERSION 3.20)
project(MicroKernel_MPU C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ── Host-only targets (skipped when cross-compiling for ARM) ─────────────────
if(NOT CMAKE_CROSSCOMPILING)

# ── seam_lib: shared host-side analysis sources ──────────────────────────────
add_library(seam_lib OBJECT
  tools/seam/src/seam_analyze.c
  tools/seam/src/seam_rules.c
  tools/seam/src/seam_print.c
)
target_include_directories(seam_lib PUBLIC
  ${CMAKE_CURRENT_LIST_DIR}/tools/seam/include
)
target_compile_options(seam_lib PRIVATE -Wall -Wextra -O2)

add_executable(mkdbg_native_host
  tools/mkdbg_native.c
  tools/mkdbg_util.c
  tools/mkdbg_process.c
  tools/mkdbg_config.c
  tools/mkdbg_incident.c
  tools/mkdbg_parse.c
  tools/mkdbg_core.c
  tools/mkdbg_launcher.c
  tools/mkdbg_git.c
  tools/mkdbg_probe.c
  tools/mkdbg_action.c
  tools/mkdbg_serial.c
  tools/mkdbg_seam.c
  $<TARGET_OBJECTS:seam_lib>
)
set_target_properties(mkdbg_native_host PROPERTIES
  OUTPUT_NAME mkdbg-native
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
target_compile_options(mkdbg_native_host PRIVATE
  -Wall
  -Wextra
  -Werror
  -O2
)
target_include_directories(mkdbg_native_host PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/tools/seam/include
)

# ── seam-analyze: standalone thin shim over mkdbg_cmd_seam ───────────────────
add_executable(seam_analyze_host
  tools/seam_main.c
  tools/mkdbg_seam.c
  $<TARGET_OBJECTS:seam_lib>
)
set_target_properties(seam_analyze_host PROPERTIES
  OUTPUT_NAME seam-analyze
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
target_compile_options(seam_analyze_host PRIVATE
  -Wall
  -Wextra
  -Werror
  -O2
)
target_include_directories(seam_analyze_host PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/tools/seam/include
)

# ── seam-analyze CI ctest ─────────────────────────────────────────────────────
enable_testing()
add_test(
  NAME seam_analyze_kdi_cascade
  COMMAND $<TARGET_FILE:seam_analyze_host>
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/seam/test/fixtures/normal_kdi_cascade.cfl
)
set_tests_properties(seam_analyze_kdi_cascade PROPERTIES
  PASS_REGULAR_EXPRESSION "VERDICT:"
)

endif() # NOT CMAKE_CROSSCOMPILING

set(VM32_MEM_SIZE "4096" CACHE STRING "VM32 memory size in bytes (256/1024/4096)")
set(BOARD_UART_PORT "2" CACHE STRING "UART port for VCP (1/2/3)")
set(BUILD_PROFILE "default" CACHE STRING "Build profile label embedded in firmware identity")

set(BUILD_INFO_FIRMWARE_NAME "MicroKernel-MPU")
set(BUILD_INFO_BOARD_NAME "Nucleo-F446RE")
set(BUILD_INFO_GIT_SHA "nogit")
set(BUILD_INFO_GIT_DIRTY 0)
set(BUILD_INFO_GIT_STATE "clean")
set(BUILD_INFO_ID_HEX "0x4D4B4442U")

find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_LIST_DIR}" rev-parse --short=8 HEAD
    OUTPUT_VARIABLE BUILD_INFO_GIT_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE BUILD_INFO_GIT_SHA_RC
  )
  if(NOT BUILD_INFO_GIT_SHA_RC EQUAL 0 OR BUILD_INFO_GIT_SHA STREQUAL "")
    set(BUILD_INFO_GIT_SHA "nogit")
  else()
    string(TOUPPER "${BUILD_INFO_GIT_SHA}" BUILD_INFO_GIT_SHA_UPPER)
    if(BUILD_INFO_GIT_SHA_UPPER MATCHES "^[0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F]$")
      set(BUILD_INFO_ID_HEX "0x${BUILD_INFO_GIT_SHA_UPPER}U")
    endif()
  endif()

  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_LIST_DIR}" status --porcelain --untracked-files=no
    OUTPUT_VARIABLE BUILD_INFO_GIT_STATUS
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE BUILD_INFO_GIT_STATUS_RC
  )
  if(BUILD_INFO_GIT_STATUS_RC EQUAL 0 AND NOT BUILD_INFO_GIT_STATUS STREQUAL "")
    set(BUILD_INFO_GIT_DIRTY 1)
    set(BUILD_INFO_GIT_STATE "dirty")
  endif()
endif()

configure_file(
  ${CMAKE_CURRENT_LIST_DIR}/cmake/build_info.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/generated/build_info.h
  @ONLY
)

set(LINKER_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/linker/STM32F446ZE_FLASH.ld)
set(STARTUP_FILE ${CMAKE_CURRENT_LIST_DIR}/startup/startup_stm32f446xx.s)

add_executable(${PROJECT_NAME}.elf
  src/main.c
  src/bringup_phase.c
  src/semantic_telemetry.c
  src/fault.c
  src/interrupts.c
  src/vm32.c
  src/vm32_scenarios.c
  src/scenario.c
  src/sonic_lite.c
  src/kdi.c
  src/dependency_graph.c
  src/analysis_engine.c
  src/syscalls.c
  bsp/board.c
  bsp/mpu_demo.c
  bsp/system_stm32f4xx.c
  bsp/seam_port.c
  freertos/Source/list.c
  freertos/Source/queue.c
  freertos/Source/tasks.c
  freertos/Source/timers.c
  freertos/Source/event_groups.c
  freertos/Source/stream_buffer.c
  freertos/Source/croutine.c
  freertos/Source/portable/Common/mpu_wrappers_v2.c
  freertos/Source/portable/GCC/ARM_CM4_MPU/port.c
  freertos/Source/portable/GCC/ARM_CM4_MPU/mpu_wrappers_v2_asm.c
  freertos/Source/portable/MemMang/heap_4.c
  ${STARTUP_FILE}
)

target_compile_definitions(${PROJECT_NAME}.elf PRIVATE
  STM32F446xx
  VM32_MEM_SIZE=${VM32_MEM_SIZE}
  BOARD_UART_PORT=${BOARD_UART_PORT}
)

target_compile_options(${PROJECT_NAME}.elf PRIVATE
  -mcpu=cortex-m4
  -mthumb
  -mfpu=fpv4-sp-d16
  -mfloat-abi=hard
  -ffunction-sections
  -fdata-sections
  -Wall
  -Wextra
)

target_link_options(${PROJECT_NAME}.elf PRIVATE
  -T${LINKER_SCRIPT}
  -mcpu=cortex-m4
  -mthumb
  -mfpu=fpv4-sp-d16
  -mfloat-abi=hard
  -Wl,--gc-sections
  -Wl,-Map=${PROJECT_NAME}.map
  -specs=nosys.specs
  -specs=nano.specs
)

add_custom_command(TARGET ${PROJECT_NAME}.elf POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${PROJECT_NAME}.elf> ${PROJECT_NAME}.hex
  COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${PROJECT_NAME}.elf> ${PROJECT_NAME}.bin
  COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${PROJECT_NAME}.elf>
)
target_include_directories(${PROJECT_NAME}.elf PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/include
  ${CMAKE_CURRENT_BINARY_DIR}/generated
  ${CMAKE_CURRENT_LIST_DIR}/cmsis/CMSIS/Include
  ${CMAKE_CURRENT_LIST_DIR}/cmsis/CMSIS/Device/ST/STM32F4xx/Include
  ${CMAKE_CURRENT_LIST_DIR}/freertos
  ${CMAKE_CURRENT_LIST_DIR}/freertos/Source/include
  ${CMAKE_CURRENT_LIST_DIR}/freertos/Source/portable/GCC/ARM_CM4_MPU
  ${CMAKE_CURRENT_LIST_DIR}/tools/seam/include
)
