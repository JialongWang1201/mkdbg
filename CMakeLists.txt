cmake_minimum_required(VERSION 3.20)
project(MicroKernel_MPU C ASM)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(VM32_MEM_SIZE "4096" CACHE STRING "VM32 memory size in bytes (256/1024/4096)")
set(BOARD_UART_PORT "2" CACHE STRING "UART port for VCP (1/2/3)")

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
  ${CMAKE_CURRENT_LIST_DIR}/cmsis/CMSIS/Include
  ${CMAKE_CURRENT_LIST_DIR}/cmsis/CMSIS/Device/ST/STM32F4xx/Include
  ${CMAKE_CURRENT_LIST_DIR}/freertos
  ${CMAKE_CURRENT_LIST_DIR}/freertos/Source/include
  ${CMAKE_CURRENT_LIST_DIR}/freertos/Source/portable/GCC/ARM_CM4_MPU
)
