#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include "stm32f4xx.h"
#include "static_assert.h"

#define configUSE_PREEMPTION                    1
#define configUSE_TIME_SLICING                 1
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0
#define configCPU_CLOCK_HZ                      (SystemCoreClock)
#define configTICK_RATE_HZ                      ((TickType_t)1000)
#define configMAX_PRIORITIES                    5
#define configMINIMAL_STACK_SIZE                ((uint16_t)128)
#define configTOTAL_HEAP_SIZE                   ((size_t)(32 * 1024))
#define configMAX_TASK_NAME_LEN                 16
#define configUSE_16_BIT_TICKS                  0
#define configIDLE_SHOULD_YIELD                 1
#define configUSE_MUTEXES                       1
#define configQUEUE_REGISTRY_SIZE               0
#define configCHECK_FOR_STACK_OVERFLOW          2
#define configUSE_RECURSIVE_MUTEXES             0
#define configUSE_MALLOC_FAILED_HOOK            1
#define configUSE_COUNTING_SEMAPHORES           1
#define configSUPPORT_STATIC_ALLOCATION         1
#define configSUPPORT_DYNAMIC_ALLOCATION        1

#define configUSE_PORT_OPTIMISED_TASK_SELECTION 0
#define configENABLE_MPU                        1
#define configUSE_MPU_WRAPPERS                  1
#define configUSE_MPU_WRAPPERS_V1               0
#define configTOTAL_MPU_REGIONS                 8
#define configINCLUDE_APPLICATION_DEFINED_PRIVILEGED_FUNCTIONS 0
#define configALLOW_UNPRIVILEGED_CRITICAL_SECTIONS 0
#define configENFORCE_SYSTEM_CALLS_FROM_KERNEL_ONLY 0
#define configPROTECTED_KERNEL_OBJECT_POOL_SIZE 12
#define configSYSTEM_CALL_STACK_SIZE            512
#define configENABLE_ACCESS_CONTROL_LIST        1
#define configCHECK_HANDLER_INSTALLATION       0

/* Set to 1 to disable the MPU at runtime for debugging. */
#ifndef APP_DISABLE_MPU
#define APP_DISABLE_MPU 0
#endif

#define configUSE_TRACE_FACILITY                0
#define configUSE_STATS_FORMATTING_FUNCTIONS    0

#define configUSE_TIMERS                        0

#define INCLUDE_vTaskDelay                      1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_xTaskGetSchedulerState          1

#ifdef __NVIC_PRIO_BITS
#define configPRIO_BITS __NVIC_PRIO_BITS
#else
#define configPRIO_BITS 4
#endif

/* NVIC priority uses the top configPRIO_BITS bits of the 8-bit field. */
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY         15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY    5
#define configPRIO_SHIFT (8U - configPRIO_BITS)

#define configKERNEL_INTERRUPT_PRIORITY \
  (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << configPRIO_SHIFT)
#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
  (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << configPRIO_SHIFT)

STATIC_ASSERT((configPRIO_BITS >= 1) && (configPRIO_BITS <= 8), nvic_prio_bits_range);
STATIC_ASSERT(configLIBRARY_LOWEST_INTERRUPT_PRIORITY < (1U << configPRIO_BITS), nvic_low_prio_range);
STATIC_ASSERT(configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY < (1U << configPRIO_BITS), nvic_max_syscall_range);
STATIC_ASSERT((configMAX_SYSCALL_INTERRUPT_PRIORITY & ((1U << configPRIO_SHIFT) - 1U)) == 0,
              max_syscall_prio_align);

#define portCONFIGURE_TIMER_FOR_RUN_TIME_STATS()
#define portGET_RUN_TIME_COUNTER_VALUE()

#define traceISR_ENTER()
#define traceISR_EXIT()
#define traceISR_EXIT_TO_SCHEDULER()

/* ── seam RTOS instrumentation ───────────────────────────────────────────── */
/* seam_agent.h has no FreeRTOS dependency so there is no circular include.  */
/* The macros below are identical to seam_freertos.h; they are inlined here  */
/* because seam_freertos.h includes FreeRTOS.h which would be circular.      */
/* Macros expand at use-site (tasks.c / queue.c) where task.h is in scope.   */
#include "seam_agent.h"

#define traceTASK_SWITCHED_IN() \
    seam_emit(CFL_LAYER_RTOS, CFL_EV_TASK_SWITCH, \
              (uint32_t)(uintptr_t)xTaskGetCurrentTaskHandle(), 0, 0, 0)

#define traceBLOCKED_ON_QUEUE_PEEK_FROM_ISR() \
    seam_emit(CFL_LAYER_RTOS, CFL_EV_TASK_BLOCK, \
              (uint32_t)(uintptr_t)xTaskGetCurrentTaskHandle(), 0, 0, 0)

#define traceQUEUE_SEND_FAILED(pxQueue) \
    seam_emit(CFL_LAYER_RTOS, CFL_EV_QUEUE_FULL, \
              (uint32_t)(uintptr_t)(pxQueue), \
              (uint32_t)(uintptr_t)xTaskGetCurrentTaskHandle(), 0, 0)

#endif
