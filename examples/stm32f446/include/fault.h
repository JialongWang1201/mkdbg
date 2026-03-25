#ifndef FAULT_H
#define FAULT_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
  FAULT_SRC_NONE = 0,
  FAULT_SRC_CPU = 1,
  FAULT_SRC_VM = 2
} FaultSource;

typedef enum {
  FAULT_CPU_HARD = 1,
  FAULT_CPU_USAGE = 2,
  FAULT_CPU_MEMMANAGE = 3
} FaultCpuType;

typedef struct {
  uint32_t magic;
  uint8_t src;
  uint8_t cpu_type;
  uint8_t vm_err;
  uint8_t priv;
  uint32_t cfsr;
  uint32_t hfsr;
  uint32_t mmfar;
  uint32_t bfar;
  uint32_t mpu_rbar;
  uint32_t mpu_rasr;
  uint32_t exc_return;
  uint32_t control;
  uint32_t msp;
  uint32_t psp;
  uint32_t pc;
  uint32_t lr;
  uint32_t sp;
  uint32_t xpsr;
  uint32_t vm_pc;
  uint32_t vm_ic;
  uint32_t vm_last_out;
  uint8_t mpu_region;
  uint8_t vm_op;
  uint8_t vm_dtop;
  uint8_t vm_rtop;
  char task[16];
} FaultRecord;

typedef struct {
  uint32_t boot_count;
  uint32_t reset_flags;
  uint8_t fault_reboot;
  uint8_t retained_valid;
  uint16_t reserved;
  FaultRecord retained;
} FaultRetentionInfo;

void fault_boot_init(uint32_t reset_flags);
void fault_report_cpu(FaultCpuType type, uint32_t *stacked, uint32_t exc_return);
void fault_report_vm(int vm_err, uint32_t pc, uint32_t ic, uint8_t op,
                     uint8_t dtop, uint8_t rtop, uint32_t last_out);
void fault_reboot_from_cpu_fault(void);
void fault_dispatch(const FaultRecord *record);
int fault_format_human(const volatile FaultRecord *record, char *buf, size_t buf_size);
int fault_last_boot_was_fault_reboot(void);

int fault_last_copy(FaultRecord *out);
int fault_retained_copy(FaultRetentionInfo *out);
void fault_emit_last_json(void);
void fault_emit_last_human(void);
void fault_emit_retained_json(void);
void fault_emit_retained_human(void);
void fault_emit_reset_cause_json(void);
void fault_emit_reset_cause_human(void);
uint32_t fault_history_count(void);
void fault_emit_dump_json(void);
void fault_emit_dump_human(void);

#endif
