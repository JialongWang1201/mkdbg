#ifndef VM32_H
#define VM32_H

#include <stdint.h>
#include <stddef.h>

#ifndef VM32_MEM_SIZE
#define VM32_MEM_SIZE 4096U
#endif

#if (VM32_MEM_SIZE != 256U) && (VM32_MEM_SIZE != 1024U) && (VM32_MEM_SIZE != 4096U)
#error "VM32_MEM_SIZE must be 256, 1024, or 4096 bytes"
#endif
#define VM32_DS_SIZE 64U
#define VM32_RS_SIZE 32U

typedef enum {
  VM32_OK = 0,
  VM32_ERR_HALT = 1,
  VM32_ERR_STACK = 2,
  VM32_ERR_MEM = 3,
  VM32_ERR_CFG = 4,
  VM32_ERR_POLICY = 5
} Vm32Result;

typedef enum {
  VM32_CFG_OK = 0,
  VM32_CFG_BAD_ARG = 1,
  VM32_CFG_ILLEGAL_OP = 2,
  VM32_CFG_CALL_RET_FORBIDDEN = 3,
  VM32_CFG_BACK_EDGE = 4,
  VM32_CFG_TARGET_OOB = 5,
  VM32_CFG_DECODE_OOB = 6
} Vm32CfgReason;

typedef struct {
  uint32_t entry;
  uint32_t span;
  uint32_t reachable;
  uint32_t max_steps;
  uint32_t reject_pc;
  uint8_t reject_op;
  Vm32CfgReason reason;
} Vm32CfgReport;

typedef enum {
  VM32_OP_NOP = 0x00,
  VM32_OP_PUSH = 0x01,
  VM32_OP_DUP = 0x02,
  VM32_OP_DROP = 0x03,
  VM32_OP_SWAP = 0x04,
  VM32_OP_OVER = 0x05,
  VM32_OP_ADD = 0x10,
  VM32_OP_SUB = 0x11,
  VM32_OP_AND = 0x12,
  VM32_OP_OR  = 0x13,
  VM32_OP_XOR = 0x14,
  VM32_OP_NOT = 0x15,
  VM32_OP_SHL = 0x16,
  VM32_OP_SHR = 0x17,
  VM32_OP_LOAD = 0x20,
  VM32_OP_STORE = 0x21,
  VM32_OP_JZ = 0x30,
  VM32_OP_CALL = 0x31,
  VM32_OP_RET = 0x32,
  VM32_OP_IN = 0x40,
  VM32_OP_OUT = 0x41,
  VM32_OP_HALT = 0xFF
} Vm32Opcode;

typedef enum {
  VM32_MIG_MODE_OFF = 0,
  VM32_MIG_MODE_MONITOR = 1,
  VM32_MIG_MODE_ENFORCE = 2
} Vm32MigMode;

typedef enum {
  VM32_MIG_RES_UART_TX = (1U << 0),
  VM32_MIG_RES_UART_RX = (1U << 1),
  VM32_MIG_RES_LED = (1U << 2),
  VM32_MIG_RES_IC = (1U << 3),
  VM32_MIG_RES_ALL = (VM32_MIG_RES_UART_TX | VM32_MIG_RES_UART_RX |
                      VM32_MIG_RES_LED | VM32_MIG_RES_IC)
} Vm32MigResourceMask;

typedef struct {
  Vm32MigMode mode;
  uint8_t enforce_blocked;
  uint32_t allow_mask;
  uint32_t violations;
  uint32_t last_addr;
  uint8_t last_write;
  uint8_t last_resource;
} Vm32MigStatus;

typedef struct {
  uint8_t mem[VM32_MEM_SIZE];
  uint32_t pc;
  uint32_t ic;
  uint32_t ds[VM32_DS_SIZE];
  uint32_t rs[VM32_RS_SIZE];
  uint8_t dtop;
  uint8_t rtop;
  uint8_t flag_z;
  uint8_t flag_n;
  uint8_t trace;
  uint8_t last_op;
  uint32_t io_beat_div;
  uint32_t last_out;
  uint8_t bp_valid;
  uint32_t bp_addr;
  uint8_t mig_mode;
  uint32_t mig_allow_mask;
  uint32_t mig_violations;
  uint32_t mig_last_addr;
  uint8_t mig_last_write;
  uint8_t mig_last_resource;
  uint8_t mig_enforce_blocked;
  uint8_t mig_initialized;
} Vm32;

void vm32_reset(Vm32 *vm);
Vm32Result vm32_step(Vm32 *vm);
Vm32Result vm32_verify_bounded_cfg(const Vm32 *vm, uint32_t entry, uint32_t span, Vm32CfgReport *report);
const char *vm32_cfg_reason_name(Vm32CfgReason reason);
const char *vm32_mig_mode_name(Vm32MigMode mode);
const char *vm32_mig_resource_name(uint8_t resource);
void vm32_mig_set_mode(Vm32 *vm, Vm32MigMode mode);
Vm32MigMode vm32_mig_get_mode(const Vm32 *vm);
void vm32_mig_allow(Vm32 *vm, uint32_t mask);
void vm32_mig_deny(Vm32 *vm, uint32_t mask);
void vm32_mig_reset(Vm32 *vm);
void vm32_mig_status(const Vm32 *vm, Vm32MigStatus *out);

#endif
