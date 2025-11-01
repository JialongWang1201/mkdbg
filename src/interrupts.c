#include "FreeRTOS.h"
#include "task.h"
#include "portmacro.h"
#include "board.h"
#include "fault.h"
#include "stm32f4xx.h"

#ifndef APP_FAULT_VERBOSE
#define APP_FAULT_VERBOSE 0
#endif

#ifndef APP_DUMP_MPU_REGIONS
#define APP_DUMP_MPU_REGIONS 0
#endif

#if APP_DUMP_MPU_REGIONS
static void write_hex32(const char *label, uint32_t value)
{
  char buf[11];
  static const char hex[] = "0123456789ABCDEF";

  buf[0] = '0';
  buf[1] = 'x';
  for (int i = 0; i < 8; ++i) {
    uint32_t shift = (7U - (uint32_t)i) * 4U;
    buf[2 + i] = hex[(value >> shift) & 0xFU];
  }
  buf[10] = '\0';

  if (label) {
    board_uart_write(label);
  }
  board_uart_write(buf);
  board_uart_write("\r\n");
}

static void dump_mpu_regions(void)
{
  board_uart_write("MPU regions:\r\n");
  for (uint32_t i = 0; i < 8U; ++i) {
    MPU->RNR = i;
    uint32_t rbar = MPU->RBAR;
    uint32_t rasr = MPU->RASR;
    char prefix[4];
    prefix[0] = 'R';
    prefix[1] = (char)('0' + (char)i);
    prefix[2] = ':';
    prefix[3] = '\0';
    board_uart_write(prefix);
    board_uart_write("\r\n");
    write_hex32("RBAR=", rbar);
    write_hex32("RASR=", rasr);
  }
}
#endif

void vHardFaultHandlerC(uint32_t *stacked, uint32_t exc_return)
{
  fault_report_cpu(FAULT_CPU_HARD, stacked, exc_return);
  fault_reboot_from_cpu_fault();
}

void vUsageFaultHandlerC(uint32_t *stacked, uint32_t exc_return)
{
  fault_report_cpu(FAULT_CPU_USAGE, stacked, exc_return);
  fault_reboot_from_cpu_fault();
}

void vMemManageHandlerC(uint32_t *stacked, uint32_t exc_return)
{
  fault_report_cpu(FAULT_CPU_MEMMANAGE, stacked, exc_return);
#if APP_DUMP_MPU_REGIONS
  dump_mpu_regions();
#endif
  fault_reboot_from_cpu_fault();
}

__attribute__((naked)) void HardFault_Handler(void)
{
  __asm volatile(
    "tst lr, #4            \n"
    "ite eq                \n"
    "mrseq r0, msp         \n"
    "mrsne r0, psp         \n"
    "mov r1, lr            \n"
    "b vHardFaultHandlerC  \n"
  );
}

__attribute__((naked)) void UsageFault_Handler(void)
{
  __asm volatile(
    "tst lr, #4            \n"
    "ite eq                \n"
    "mrseq r0, msp         \n"
    "mrsne r0, psp         \n"
    "mov r1, lr            \n"
    "b vUsageFaultHandlerC \n"
  );
}

__attribute__((naked)) void MemManage_Handler(void)
{
  __asm volatile(
    "tst lr, #4             \n"
    "ite eq                 \n"
    "mrseq r0, msp          \n"
    "mrsne r0, psp          \n"
    "mov r1, lr             \n"
    "b vMemManageHandlerC   \n"
  );
}
