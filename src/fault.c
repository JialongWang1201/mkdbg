#include "fault.h"
#include "board.h"
#include "stm32f4xx.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include <stdio.h>

#ifndef APP_FAULT_VERBOSE
#define APP_FAULT_VERBOSE 0
#endif

#define FAULT_MAGIC 0xFA17EADU
#define FAULT_HISTORY_LEN 8U
#define FAULT_RETENTION_MAGIC 0xFA17C0DEU
#define FAULT_RETENTION_VERSION 1U

typedef struct {
  uint32_t magic;
  uint32_t version;
  uint32_t boot_count;
  uint32_t last_reset_flags;
  uint32_t retained_fault_valid;
  uint32_t fault_reboot_pending;
  FaultRecord retained_fault;
} FaultRetentionBlob;

static volatile FaultRecord g_fault_last;
static volatile uint32_t g_fault_valid = 0U;
static volatile FaultRecord g_fault_history[FAULT_HISTORY_LEN];
static volatile uint32_t g_fault_hist_head = 0U;
static volatile uint32_t g_fault_hist_count = 0U;
static volatile FaultRetentionBlob g_fault_retained
  __attribute__((section(".fault_retained"), aligned(64)));
static uint32_t g_fault_boot_reset_flags = 0U;
static uint8_t g_fault_boot_fault_reboot = 0U;
static char g_fault_emit_buf[512];

static const char *fault_cpu_name(FaultCpuType type)
{
  switch (type) {
    case FAULT_CPU_HARD:
      return "HardFault";
    case FAULT_CPU_USAGE:
      return "UsageFault";
    case FAULT_CPU_MEMMANAGE:
      return "MemManage";
    default:
      return "Unknown";
  }
}

static void fault_copy_task(char *dst, size_t max)
{
  if (dst == NULL || max == 0U) {
    return;
  }
  dst[0] = '\0';
  if (__get_IPSR() != 0U) {
    return;
  }
  if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
    return;
  }
  const char *name = pcTaskGetName(NULL);
  if (name == NULL) {
    return;
  }
  size_t i = 0;
  for (; i + 1U < max && name[i] != '\0'; ++i) {
    dst[i] = name[i];
  }
  dst[i] = '\0';
}

static void fault_retention_init_if_needed(void)
{
  if (g_fault_retained.magic == FAULT_RETENTION_MAGIC &&
      g_fault_retained.version == FAULT_RETENTION_VERSION) {
    return;
  }
  memset((void *)&g_fault_retained, 0, sizeof(g_fault_retained));
  g_fault_retained.magic = FAULT_RETENTION_MAGIC;
  g_fault_retained.version = FAULT_RETENTION_VERSION;
}

static void fault_store_retained_cpu(const FaultRecord *src)
{
  if (src == NULL) {
    return;
  }
  fault_retention_init_if_needed();
  g_fault_retained.retained_fault = *src;
  g_fault_retained.retained_fault_valid = 1U;
  g_fault_retained.fault_reboot_pending = 1U;
}

static const char *fault_reset_primary_name(uint32_t flags)
{
  if ((flags & RCC_CSR_LPWRRSTF) != 0U) {
    return "low_power";
  }
  if ((flags & RCC_CSR_WWDGRSTF) != 0U) {
    return "window_watchdog";
  }
  if ((flags & RCC_CSR_IWDGRSTF) != 0U) {
    return "independent_watchdog";
  }
  if ((flags & RCC_CSR_SFTRSTF) != 0U) {
    return "software";
  }
  if ((flags & RCC_CSR_BORRSTF) != 0U) {
    return "brown_out";
  }
  if ((flags & RCC_CSR_PORRSTF) != 0U) {
    return "power_on";
  }
  if ((flags & RCC_CSR_PINRSTF) != 0U) {
    return "pin";
  }
  return "unknown";
}

static void fault_capture_mpu_region(FaultRecord *r)
{
  uint32_t saved_rnr;
  uint32_t addr;

  if (r == NULL) {
    return;
  }

  r->mpu_region = 0xFFU;
  r->mpu_rbar = 0U;
  r->mpu_rasr = 0U;

  addr = r->mmfar;
  if ((r->cfsr & SCB_CFSR_MMARVALID_Msk) == 0U) {
    addr = r->pc;
  }

  saved_rnr = MPU->RNR;
  for (uint32_t i = 0U; i < 8U; ++i) {
    uint32_t rbar;
    uint32_t rasr;
    uint32_t size_field;
    uint32_t region_size;
    uint32_t base;

    MPU->RNR = i;
    rbar = MPU->RBAR;
    rasr = MPU->RASR;
    if ((rasr & MPU_RASR_ENABLE_Msk) == 0U) {
      continue;
    }

    size_field = (rasr & MPU_RASR_SIZE_Msk) >> MPU_RASR_SIZE_Pos;
    if (size_field > 30U) {
      continue;
    }
    region_size = 1UL << (size_field + 1U);
    base = rbar & MPU_RBAR_ADDR_Msk;

    if (addr >= base && (addr - base) < region_size) {
      r->mpu_region = (uint8_t)i;
      r->mpu_rbar = rbar;
      r->mpu_rasr = rasr;
      break;
    }
  }
  MPU->RNR = saved_rnr;
}

int fault_format_human(const volatile FaultRecord *record, char *buf, size_t buf_size)
{
  int n = 0;
  const volatile FaultRecord *r = record;

  if (r == NULL || buf == NULL || buf_size == 0U) {
    return 0;
  }

  if (r->src == FAULT_SRC_CPU) {
    n = snprintf(buf, buf_size,
                 "Fault(cpu:%s) pc=0x%08lX lr=0x%08lX sp=0x%08lX task=%s priv=%s cfsr=0x%08lX hfsr=0x%08lX mmfar=0x%08lX bfar=0x%08lX mpu_region=%u",
                 fault_cpu_name((FaultCpuType)r->cpu_type),
                 (unsigned long)r->pc,
                 (unsigned long)r->lr,
                 (unsigned long)r->sp,
                 (r->task[0] != '\0') ? r->task : "-",
                 r->priv ? "unpriv" : "priv",
                 (unsigned long)r->cfsr,
                 (unsigned long)r->hfsr,
                 (unsigned long)r->mmfar,
                 (unsigned long)r->bfar,
                 (unsigned)r->mpu_region);
  } else if (r->src == FAULT_SRC_VM) {
    n = snprintf(buf, buf_size,
                 "Fault(vm) err=%u pc=0x%04lX op=0x%02X ic=%lu dtop=%u rtop=%u last_out=%lu task=%s",
                 (unsigned)r->vm_err,
                 (unsigned long)r->vm_pc,
                 (unsigned)r->vm_op,
                 (unsigned long)r->vm_ic,
                 (unsigned)r->vm_dtop,
                 (unsigned)r->vm_rtop,
                 (unsigned long)r->vm_last_out,
                 (r->task[0] != '\0') ? r->task : "-");
  } else {
    n = snprintf(buf, buf_size, "Fault(unknown)");
  }

  if (n < 0) {
    return 0;
  }
  return n;
}

static void fault_emit_human(const volatile FaultRecord *r)
{
  if (fault_format_human(r, g_fault_emit_buf, sizeof(g_fault_emit_buf)) <= 0) {
    return;
  }
  board_uart_write(g_fault_emit_buf);
  board_uart_write("\r\n");
}

static void fault_emit_human_record(const volatile FaultRecord *r)
{
  if (r == NULL) {
    return;
  }
  if (r->src == FAULT_SRC_CPU || r->src == FAULT_SRC_VM) {
    fault_emit_human(r);
    return;
  }
  board_uart_write("Unknown fault type\r\n");
}

static void fault_emit_json_cpu(const volatile FaultRecord *r)
{
  const char *name = fault_cpu_name((FaultCpuType)r->cpu_type);
  const char *priv = r->priv ? "unpriv" : "priv";
  if (r->task[0] != '\0') {
    snprintf(g_fault_emit_buf, sizeof(g_fault_emit_buf),
             "{\"type\":\"fault\",\"src\":\"cpu\",\"name\":\"%s\","
             "\"pc\":\"0x%08lX\",\"lr\":\"0x%08lX\",\"sp\":\"0x%08lX\","
             "\"xpsr\":\"0x%08lX\",\"control\":\"0x%08lX\","
             "\"cfsr\":\"0x%08lX\",\"hfsr\":\"0x%08lX\","
             "\"mmfar\":\"0x%08lX\",\"bfar\":\"0x%08lX\","
             "\"mpu_region\":%u,\"mpu_rbar\":\"0x%08lX\",\"mpu_rasr\":\"0x%08lX\","
             "\"exc_return\":\"0x%08lX\",\"task\":\"%s\",\"priv\":\"%s\"}\r\n",
             name,
             (unsigned long)r->pc,
             (unsigned long)r->lr,
             (unsigned long)r->sp,
             (unsigned long)r->xpsr,
             (unsigned long)r->control,
             (unsigned long)r->cfsr,
             (unsigned long)r->hfsr,
             (unsigned long)r->mmfar,
             (unsigned long)r->bfar,
             (unsigned)r->mpu_region,
             (unsigned long)r->mpu_rbar,
             (unsigned long)r->mpu_rasr,
             (unsigned long)r->exc_return,
             r->task,
             priv);
  } else {
    snprintf(g_fault_emit_buf, sizeof(g_fault_emit_buf),
             "{\"type\":\"fault\",\"src\":\"cpu\",\"name\":\"%s\","
             "\"pc\":\"0x%08lX\",\"lr\":\"0x%08lX\",\"sp\":\"0x%08lX\","
             "\"xpsr\":\"0x%08lX\",\"control\":\"0x%08lX\","
             "\"cfsr\":\"0x%08lX\",\"hfsr\":\"0x%08lX\","
             "\"mmfar\":\"0x%08lX\",\"bfar\":\"0x%08lX\","
             "\"mpu_region\":%u,\"mpu_rbar\":\"0x%08lX\",\"mpu_rasr\":\"0x%08lX\","
             "\"exc_return\":\"0x%08lX\",\"priv\":\"%s\"}\r\n",
             name,
             (unsigned long)r->pc,
             (unsigned long)r->lr,
             (unsigned long)r->sp,
             (unsigned long)r->xpsr,
             (unsigned long)r->control,
             (unsigned long)r->cfsr,
             (unsigned long)r->hfsr,
             (unsigned long)r->mmfar,
             (unsigned long)r->bfar,
             (unsigned)r->mpu_region,
             (unsigned long)r->mpu_rbar,
             (unsigned long)r->mpu_rasr,
             (unsigned long)r->exc_return,
             priv);
  }
  board_uart_write(g_fault_emit_buf);
}

static void fault_emit_json_vm(const volatile FaultRecord *r)
{
  snprintf(g_fault_emit_buf, sizeof(g_fault_emit_buf),
           "{\"type\":\"fault\",\"src\":\"vm\",\"name\":\"VmFault\","
           "\"err\":%u,\"pc\":\"0x%04lX\",\"op\":\"0x%02X\","
           "\"ic\":%lu,\"dtop\":%u,\"rtop\":%u,\"last_out\":%lu}\r\n",
           (unsigned)r->vm_err,
           (unsigned long)r->vm_pc,
           (unsigned)r->vm_op,
           (unsigned long)r->vm_ic,
           (unsigned)r->vm_dtop,
           (unsigned)r->vm_rtop,
           (unsigned long)r->vm_last_out);
  board_uart_write(g_fault_emit_buf);
}

static void fault_emit_json_record(const volatile FaultRecord *r)
{
  if (r == NULL) {
    return;
  }
  if (r->src == FAULT_SRC_CPU) {
    fault_emit_json_cpu(r);
    return;
  }
  if (r->src == FAULT_SRC_VM) {
    fault_emit_json_vm(r);
    return;
  }
  board_uart_write("{\"type\":\"fault\",\"src\":\"unknown\"}\r\n");
}

static void fault_store(const FaultRecord *src)
{
  uint32_t head;

  if (src == NULL) {
    return;
  }

  g_fault_last = *src;
  g_fault_valid = 1U;
  head = g_fault_hist_head;
  g_fault_history[head] = *src;
  g_fault_hist_head = (head + 1U) % FAULT_HISTORY_LEN;
  if (g_fault_hist_count < FAULT_HISTORY_LEN) {
    g_fault_hist_count++;
  }
}

void fault_dispatch(const FaultRecord *record)
{
  if (record == NULL) {
    return;
  }

  fault_store(record);

  if (g_fault_last.src == FAULT_SRC_CPU) {
#if APP_FAULT_VERBOSE
    fault_emit_human(&g_fault_last);
#endif
    fault_emit_json_record(&g_fault_last);
    return;
  }

  if (g_fault_last.src == FAULT_SRC_VM) {
    fault_emit_human_record(&g_fault_last);
    fault_emit_json_record(&g_fault_last);
    return;
  }

  fault_emit_json_record(&g_fault_last);
}

void fault_boot_init(uint32_t reset_flags)
{
  fault_retention_init_if_needed();
  g_fault_boot_reset_flags = reset_flags;
  g_fault_boot_fault_reboot =
    (g_fault_retained.fault_reboot_pending != 0U &&
     g_fault_retained.retained_fault_valid != 0U) ? 1U : 0U;
  g_fault_retained.boot_count++;
  g_fault_retained.last_reset_flags = reset_flags;
  g_fault_retained.fault_reboot_pending = 0U;
}

void fault_report_cpu(FaultCpuType type, uint32_t *stacked, uint32_t exc_return)
{
  FaultRecord r;
  memset(&r, 0, sizeof(r));
  r.magic = FAULT_MAGIC;
  r.src = FAULT_SRC_CPU;
  r.cpu_type = (uint8_t)type;
  r.exc_return = exc_return;
  r.control = __get_CONTROL();
  r.msp = __get_MSP();
  r.psp = __get_PSP();
  r.priv = (r.control & 1U) ? 1U : 0U;
  r.cfsr = SCB->CFSR;
  r.hfsr = SCB->HFSR;
  r.mmfar = SCB->MMFAR;
  r.bfar = SCB->BFAR;

  if (stacked != NULL) {
    uint32_t *frame = stacked;
    if ((exc_return & (1U << 4)) == 0U) {
      frame = stacked + 18U; /* Skip FP context if present. */
    }
    r.pc = frame[6];
    r.lr = frame[5];
    r.xpsr = frame[7];
  }
  r.sp = (exc_return & (1U << 2)) ? r.psp : r.msp;
  fault_copy_task(r.task, sizeof(r.task));
  fault_capture_mpu_region(&r);
  fault_store_retained_cpu(&r);
  fault_dispatch(&r);
}

void fault_report_vm(int vm_err, uint32_t pc, uint32_t ic, uint8_t op,
                     uint8_t dtop, uint8_t rtop, uint32_t last_out)
{
  FaultRecord r;
  memset(&r, 0, sizeof(r));
  r.magic = FAULT_MAGIC;
  r.src = FAULT_SRC_VM;
  r.vm_err = (uint8_t)vm_err;
  r.vm_pc = pc;
  r.vm_ic = ic;
  r.vm_op = op;
  r.vm_dtop = dtop;
  r.vm_rtop = rtop;
  r.vm_last_out = last_out;
  /* Avoid task API calls in VM fault fast path; they are not required for correctness. */
  r.task[0] = '\0';
  fault_dispatch(&r);
}

void fault_reboot_from_cpu_fault(void)
{
  board_uart_write("fault reboot: system reset\r\n");
  board_delay_ms(20);
  board_system_reset();
  for (;;) {
  }
}

int fault_last_boot_was_fault_reboot(void)
{
  return (g_fault_boot_fault_reboot != 0U) ? 1 : 0;
}

int fault_last_copy(FaultRecord *out)
{
  if (out == NULL || g_fault_valid == 0U) {
    return 0;
  }
  *out = g_fault_last;
  return 1;
}

int fault_retained_copy(FaultRetentionInfo *out)
{
  if (out == NULL) {
    return 0;
  }
  fault_retention_init_if_needed();
  memset(out, 0, sizeof(*out));
  out->boot_count = g_fault_retained.boot_count;
  out->reset_flags = g_fault_boot_reset_flags;
  out->fault_reboot = g_fault_boot_fault_reboot;
  out->retained_valid = (uint8_t)(g_fault_retained.retained_fault_valid != 0U);
  if (out->retained_valid != 0U) {
    out->retained = g_fault_retained.retained_fault;
  }
  return 1;
}

void fault_emit_last_json(void)
{
  if (g_fault_valid == 0U) {
    board_uart_write("{\"type\":\"fault\",\"src\":\"none\"}\r\n");
    return;
  }
  fault_emit_json_record(&g_fault_last);
}

void fault_emit_last_human(void)
{
  if (g_fault_valid == 0U) {
    board_uart_write("No fault recorded\r\n");
    return;
  }
  fault_emit_human_record(&g_fault_last);
}

void fault_emit_retained_json(void)
{
  fault_retention_init_if_needed();
  snprintf(g_fault_emit_buf, sizeof(g_fault_emit_buf),
           "{\"type\":\"fault-retained\",\"valid\":%u,\"fault_reboot\":%u,"
           "\"reset_primary\":\"%s\",\"reset_flags\":\"0x%08lX\",\"boot_count\":%lu}\r\n",
           (unsigned)(g_fault_retained.retained_fault_valid != 0U),
           (unsigned)g_fault_boot_fault_reboot,
           fault_reset_primary_name(g_fault_boot_reset_flags),
           (unsigned long)g_fault_boot_reset_flags,
           (unsigned long)g_fault_retained.boot_count);
  board_uart_write(g_fault_emit_buf);
  if (g_fault_retained.retained_fault_valid != 0U) {
    fault_emit_json_record(&g_fault_retained.retained_fault);
  }
}

void fault_emit_retained_human(void)
{
  fault_retention_init_if_needed();
  snprintf(g_fault_emit_buf, sizeof(g_fault_emit_buf),
           "retained fault: valid=%u reboot=%s reset=%s flags=0x%08lX boots=%lu\r\n",
           (unsigned)(g_fault_retained.retained_fault_valid != 0U),
           g_fault_boot_fault_reboot ? "yes" : "no",
           fault_reset_primary_name(g_fault_boot_reset_flags),
           (unsigned long)g_fault_boot_reset_flags,
           (unsigned long)g_fault_retained.boot_count);
  board_uart_write(g_fault_emit_buf);
  if (g_fault_retained.retained_fault_valid == 0U) {
    board_uart_write("retained fault record: none\r\n");
    return;
  }
  fault_emit_human_record(&g_fault_retained.retained_fault);
}

void fault_emit_reset_cause_json(void)
{
  fault_retention_init_if_needed();
  snprintf(g_fault_emit_buf, sizeof(g_fault_emit_buf),
           "{\"type\":\"reset-cause\",\"primary\":\"%s\",\"flags\":\"0x%08lX\","
           "\"fault_reboot\":%u,\"boot_count\":%lu}\r\n",
           fault_reset_primary_name(g_fault_boot_reset_flags),
           (unsigned long)g_fault_boot_reset_flags,
           (unsigned)g_fault_boot_fault_reboot,
           (unsigned long)g_fault_retained.boot_count);
  board_uart_write(g_fault_emit_buf);
}

void fault_emit_reset_cause_human(void)
{
  fault_retention_init_if_needed();
  snprintf(g_fault_emit_buf, sizeof(g_fault_emit_buf),
           "reset cause: %s flags=0x%08lX fault_reboot=%s boots=%lu\r\n",
           fault_reset_primary_name(g_fault_boot_reset_flags),
           (unsigned long)g_fault_boot_reset_flags,
           g_fault_boot_fault_reboot ? "yes" : "no",
           (unsigned long)g_fault_retained.boot_count);
  board_uart_write(g_fault_emit_buf);
}

uint32_t fault_history_count(void)
{
  return g_fault_hist_count;
}

void fault_emit_dump_json(void)
{
  uint32_t count = g_fault_hist_count;
  uint32_t head = g_fault_hist_head;

  if (count == 0U) {
    board_uart_write("{\"type\":\"fault\",\"src\":\"none\"}\r\n");
    return;
  }

  for (uint32_t i = 0U; i < count; ++i) {
    uint32_t idx = (head + FAULT_HISTORY_LEN - count + i) % FAULT_HISTORY_LEN;
    fault_emit_json_record(&g_fault_history[idx]);
  }
}

void fault_emit_dump_human(void)
{
  uint32_t count = g_fault_hist_count;
  uint32_t head = g_fault_hist_head;

  if (count == 0U) {
    board_uart_write("No fault recorded\r\n");
    return;
  }

  for (uint32_t i = 0U; i < count; ++i) {
    uint32_t idx = (head + FAULT_HISTORY_LEN - count + i) % FAULT_HISTORY_LEN;
    fault_emit_human_record(&g_fault_history[idx]);
  }
}
