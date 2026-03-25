cmake_minimum_required(VERSION 3.20)
project(MicroKernel_MPU C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ── Repo root (two levels up from this file) ──────────────────────────────────
set(MKDBG_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")

# ── Build identity ────────────────────────────────────────────────────────────
set(VM32_MEM_SIZE  "4096" CACHE STRING "VM32 memory size in bytes (256/1024/4096)")
set(BOARD_UART_PORT "2"   CACHE STRING "UART port for VCP (1/2/3)")
set(BUILD_PROFILE  "default" CACHE STRING "Build profile label embedded in firmware identity")

set(BUILD_INFO_FIRMWARE_NAME "MicroKernel-MPU")
set(BUILD_INFO_BOARD_NAME    "Nucleo-F446RE")
set(BUILD_INFO_GIT_SHA       "nogit")
set(BUILD_INFO_GIT_DIRTY     0)
set(BUILD_INFO_GIT_STATE     "clean")
set(BUILD_INFO_ID_HEX        "0x4D4B4442U")

find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${MKDBG_ROOT}" rev-parse --short=8 HEAD
    OUTPUT_VARIABLE BUILD_INFO_GIT_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _GIT_SHA_RC
  )
  if(NOT _GIT_SHA_RC EQUAL 0 OR BUILD_INFO_GIT_SHA STREQUAL "")
    set(BUILD_INFO_GIT_SHA "nogit")
  else()
    string(TOUPPER "${BUILD_INFO_GIT_SHA}" _SHA_UPPER)
    if(_SHA_UPPER MATCHES "^[0-9A-F]{8}$")
      set(BUILD_INFO_ID_HEX "0x${_SHA_UPPER}U")
    endif()
  endif()

  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${MKDBG_ROOT}" status --porcelain --untracked-files=no
    OUTPUT_VARIABLE _GIT_STATUS
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _GIT_STATUS_RC
  )
  if(_GIT_STATUS_RC EQUAL 0 AND NOT _GIT_STATUS STREQUAL "")
    set(BUILD_INFO_GIT_DIRTY 1)
    set(BUILD_INFO_GIT_STATE "dirty")
  endif()
endif()

configure_file(
  ${CMAKE_CURRENT_LIST_DIR}/cmake/build_info.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/generated/build_info.h
  @ONLY
)

# ── wire_firmware: GDB RSP stub (cross-compiled for ARM) ─────────────────────
add_library(wire_firmware OBJECT
  ${MKDBG_ROOT}/deps/wire/src/wire_rsp.c
  ${MKDBG_ROOT}/deps/wire/src/wire_regs_cm.c
  ${MKDBG_ROOT}/deps/wire/src/wire_exception.c
)
target_include_directories(wire_firmware PUBLIC
  ${MKDBG_ROOT}/deps/wire/include
)
target_compile_options(wire_firmware PRIVATE
  -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
  -Wall -Wextra -O2
)

# ── Firmware ELF ──────────────────────────────────────────────────────────────
set(LINKER_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/linker/STM32F446ZE_FLASH.ld)
set(STARTUP_FILE  ${CMAKE_CURRENT_LIST_DIR}/startup/startup_stm32f446xx.s)

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
  bsp/wire_port.c
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
  $<TARGET_OBJECTS:wire_firmware>
)

target_compile_definitions(${PROJECT_NAME}.elf PRIVATE
  STM32F446xx
  VM32_MEM_SIZE=${VM32_MEM_SIZE}
  BOARD_UART_PORT=${BOARD_UART_PORT}
  SEAM_RING_SIZE=8
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
  COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:${PROJECT_NAME}.elf> ${PROJECT_NAME}.hex
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
  ${MKDBG_ROOT}/deps/seam/include
  ${MKDBG_ROOT}/deps/wire/include
)
