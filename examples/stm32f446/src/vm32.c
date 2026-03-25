#include "vm32.h"
#include "board.h"
#include "seam_agent.h"
#include <string.h>
#include <stdio.h>

#define VM32_IO_UART_TX 0x0FF0U
#define VM32_IO_UART_RX 0x0FF4U
#define VM32_IO_LED     0x0FF8U
#define VM32_IO_IC      0x0FFCU

static uint8_t g_cfg_seen[VM32_MEM_SIZE];
static uint16_t g_cfg_queue[VM32_MEM_SIZE];

#define VM32_MIG_RESOURCE_NONE 0U
#define VM32_MIG_RESOURCE_UART_TX 1U
#define VM32_MIG_RESOURCE_UART_RX 2U
#define VM32_MIG_RESOURCE_LED 3U
#define VM32_MIG_RESOURCE_IC 4U

static uint32_t vm32_opcode_len(uint8_t op)
{
  if (op == VM32_OP_PUSH) {
    return 5U;
  }
  if (op == VM32_OP_JZ || op == VM32_OP_CALL) {
    return 2U;
  }
  switch (op) {
    case VM32_OP_NOP:
    case VM32_OP_DUP:
    case VM32_OP_DROP:
    case VM32_OP_SWAP:
    case VM32_OP_OVER:
    case VM32_OP_ADD:
    case VM32_OP_SUB:
    case VM32_OP_AND:
    case VM32_OP_OR:
    case VM32_OP_XOR:
    case VM32_OP_NOT:
    case VM32_OP_SHL:
    case VM32_OP_SHR:
    case VM32_OP_LOAD:
    case VM32_OP_STORE:
    case VM32_OP_RET:
    case VM32_OP_IN:
    case VM32_OP_OUT:
    case VM32_OP_HALT:
      return 1U;
    default:
      return 0U;
  }
}

static uint8_t vm32_mig_resource_for_addr(uint32_t addr)
{
  if (addr >= VM32_IO_UART_TX && addr < (VM32_IO_UART_TX + 4U)) {
    return VM32_MIG_RESOURCE_UART_TX;
  }
  if (addr >= VM32_IO_UART_RX && addr < (VM32_IO_UART_RX + 4U)) {
    return VM32_MIG_RESOURCE_UART_RX;
  }
  if (addr >= VM32_IO_LED && addr < (VM32_IO_LED + 4U)) {
    return VM32_MIG_RESOURCE_LED;
  }
  if (addr >= VM32_IO_IC && addr < (VM32_IO_IC + 4U)) {
    return VM32_MIG_RESOURCE_IC;
  }
  return VM32_MIG_RESOURCE_NONE;
}

static uint32_t vm32_mig_resource_to_mask(uint8_t resource)
{
  switch (resource) {
    case VM32_MIG_RESOURCE_UART_TX:
      return VM32_MIG_RES_UART_TX;
    case VM32_MIG_RESOURCE_UART_RX:
      return VM32_MIG_RES_UART_RX;
    case VM32_MIG_RESOURCE_LED:
      return VM32_MIG_RES_LED;
    case VM32_MIG_RESOURCE_IC:
      return VM32_MIG_RES_IC;
    default:
      return 0U;
  }
}

const char *vm32_mig_mode_name(Vm32MigMode mode)
{
  switch (mode) {
    case VM32_MIG_MODE_OFF:
      return "off";
    case VM32_MIG_MODE_MONITOR:
      return "monitor";
    case VM32_MIG_MODE_ENFORCE:
      return "enforce";
    default:
      return "unknown";
  }
}

const char *vm32_mig_resource_name(uint8_t resource)
{
  switch (resource) {
    case VM32_MIG_RESOURCE_UART_TX:
      return "uart_tx";
    case VM32_MIG_RESOURCE_UART_RX:
      return "uart_rx";
    case VM32_MIG_RESOURCE_LED:
      return "led";
    case VM32_MIG_RESOURCE_IC:
      return "ic";
    default:
      return "none";
  }
}

void vm32_mig_set_mode(Vm32 *vm, Vm32MigMode mode)
{
  if (vm == NULL) {
    return;
  }
  vm->mig_mode = (uint8_t)mode;
  vm->mig_initialized = 1U;
}

Vm32MigMode vm32_mig_get_mode(const Vm32 *vm)
{
  if (vm == NULL) {
    return VM32_MIG_MODE_OFF;
  }
  return (Vm32MigMode)vm->mig_mode;
}

void vm32_mig_allow(Vm32 *vm, uint32_t mask)
{
  if (vm == NULL) {
    return;
  }
  vm->mig_allow_mask |= (mask & VM32_MIG_RES_ALL);
  vm->mig_initialized = 1U;
}

void vm32_mig_deny(Vm32 *vm, uint32_t mask)
{
  if (vm == NULL) {
    return;
  }
  vm->mig_allow_mask &= ~(mask & VM32_MIG_RES_ALL);
  vm->mig_initialized = 1U;
}

void vm32_mig_reset(Vm32 *vm)
{
  if (vm == NULL) {
    return;
  }
  vm->mig_mode = VM32_MIG_MODE_OFF;
  vm->mig_allow_mask = VM32_MIG_RES_ALL;
  vm->mig_violations = 0U;
  vm->mig_last_addr = 0U;
  vm->mig_last_write = 0U;
  vm->mig_last_resource = VM32_MIG_RESOURCE_NONE;
  vm->mig_enforce_blocked = 0U;
  vm->mig_initialized = 1U;
}

void vm32_mig_status(const Vm32 *vm, Vm32MigStatus *out)
{
  if (vm == NULL || out == NULL) {
    return;
  }
  out->mode = (Vm32MigMode)vm->mig_mode;
  out->enforce_blocked = vm->mig_enforce_blocked;
  out->allow_mask = vm->mig_allow_mask;
  out->violations = vm->mig_violations;
  out->last_addr = vm->mig_last_addr;
  out->last_write = vm->mig_last_write;
  out->last_resource = vm->mig_last_resource;
}

static void vm32_mig_track_violation(Vm32 *vm, uint32_t addr, uint8_t is_write, uint8_t resource)
{
  vm->mig_violations++;
  vm->mig_last_addr = addr;
  vm->mig_last_write = is_write;
  vm->mig_last_resource = resource;
}

static uint8_t vm32_mig_check_io(Vm32 *vm, uint32_t addr, uint8_t is_write)
{
  uint8_t resource;
  uint32_t mask;

  if (vm == NULL) {
    return 0U;
  }

  resource = vm32_mig_resource_for_addr(addr);
  if (resource == VM32_MIG_RESOURCE_NONE) {
    return 1U;
  }

  mask = vm32_mig_resource_to_mask(resource);
  if ((vm->mig_allow_mask & mask) != 0U) {
    return 1U;
  }

  if (vm->mig_mode == VM32_MIG_MODE_MONITOR || vm->mig_mode == VM32_MIG_MODE_ENFORCE) {
    vm32_mig_track_violation(vm, addr, is_write, resource);
  }
  if (vm->mig_mode == VM32_MIG_MODE_ENFORCE) {
    vm->mig_enforce_blocked = 1U;
    return 0U;
  }
  return 1U;
}

const char *vm32_cfg_reason_name(Vm32CfgReason reason)
{
  switch (reason) {
    case VM32_CFG_OK:
      return "ok";
    case VM32_CFG_BAD_ARG:
      return "bad_arg";
    case VM32_CFG_ILLEGAL_OP:
      return "illegal_op";
    case VM32_CFG_CALL_RET_FORBIDDEN:
      return "call_ret_forbidden";
    case VM32_CFG_BACK_EDGE:
      return "back_edge";
    case VM32_CFG_TARGET_OOB:
      return "target_oob";
    case VM32_CFG_DECODE_OOB:
      return "decode_oob";
    default:
      return "unknown";
  }
}

Vm32Result vm32_verify_bounded_cfg(const Vm32 *vm, uint32_t entry, uint32_t span, Vm32CfgReport *report)
{
  uint16_t q_head = 0U;
  uint16_t q_tail = 0U;
  uint32_t window_end = entry + span;

  if (report != NULL) {
    memset(report, 0, sizeof(*report));
    report->entry = entry;
    report->span = span;
    report->reason = VM32_CFG_BAD_ARG;
  }

  if (vm == NULL || span == 0U || entry >= VM32_MEM_SIZE || window_end > VM32_MEM_SIZE) {
    return VM32_ERR_CFG;
  }

  memset(g_cfg_seen, 0, sizeof(g_cfg_seen));
  g_cfg_seen[entry] = 1U;
  g_cfg_queue[q_tail++] = (uint16_t)entry;

  while (q_head < q_tail) {
    uint32_t pc = g_cfg_queue[q_head++];
    uint8_t op = vm->mem[pc];
    uint32_t len = vm32_opcode_len(op);
    uint32_t next_pc = pc + len;
    uint32_t succ_a = 0U;
    uint32_t succ_b = 0U;
    uint8_t succ_count = 0U;

    if (len == 0U) {
      if (report != NULL) {
        report->reject_pc = pc;
        report->reject_op = op;
        report->reason = VM32_CFG_ILLEGAL_OP;
      }
      return VM32_ERR_CFG;
    }
    if (pc < entry || next_pc > window_end) {
      if (report != NULL) {
        report->reject_pc = pc;
        report->reject_op = op;
        report->reason = VM32_CFG_DECODE_OOB;
      }
      return VM32_ERR_CFG;
    }
    if (op == VM32_OP_CALL || op == VM32_OP_RET) {
      if (report != NULL) {
        report->reject_pc = pc;
        report->reject_op = op;
        report->reason = VM32_CFG_CALL_RET_FORBIDDEN;
      }
      return VM32_ERR_CFG;
    }

    if (op == VM32_OP_HALT) {
      succ_count = 0U;
    } else if (op == VM32_OP_JZ) {
      int8_t rel = (int8_t)vm->mem[pc + 1U];
      int32_t target = (int32_t)next_pc + (int32_t)rel;

      if (next_pc >= window_end || target < (int32_t)entry || target >= (int32_t)window_end) {
        if (report != NULL) {
          report->reject_pc = pc;
          report->reject_op = op;
          report->reason = VM32_CFG_TARGET_OOB;
        }
        return VM32_ERR_CFG;
      }

      succ_a = next_pc;
      succ_b = (uint32_t)target;
      succ_count = 2U;
    } else {
      if (next_pc >= window_end) {
        if (report != NULL) {
          report->reject_pc = pc;
          report->reject_op = op;
          report->reason = VM32_CFG_TARGET_OOB;
        }
        return VM32_ERR_CFG;
      }
      succ_a = next_pc;
      succ_count = 1U;
    }

    for (uint8_t i = 0U; i < succ_count; ++i) {
      uint32_t succ = (i == 0U) ? succ_a : succ_b;

      if (succ <= pc) {
        if (report != NULL) {
          report->reject_pc = pc;
          report->reject_op = op;
          report->reason = VM32_CFG_BACK_EDGE;
        }
        return VM32_ERR_CFG;
      }
      if (g_cfg_seen[succ] == 0U) {
        g_cfg_seen[succ] = 1U;
        g_cfg_queue[q_tail++] = (uint16_t)succ;
      }
    }
  }

  if (report != NULL) {
    report->reason = VM32_CFG_OK;
    report->reachable = (uint32_t)q_tail;
    report->max_steps = (uint32_t)q_tail;
  }
  return VM32_OK;
}

void vm32_reset(Vm32 *vm)
{
  uint8_t mig_initialized;
  uint8_t mig_mode;
  uint32_t mig_allow_mask;
  uint32_t mig_violations;
  uint32_t mig_last_addr;
  uint8_t mig_last_write;
  uint8_t mig_last_resource;

  if (vm == NULL) {
    return;
  }

  mig_initialized = vm->mig_initialized;
  mig_mode = vm->mig_mode;
  mig_allow_mask = vm->mig_allow_mask;
  mig_violations = vm->mig_violations;
  mig_last_addr = vm->mig_last_addr;
  mig_last_write = vm->mig_last_write;
  mig_last_resource = vm->mig_last_resource;

  memset(vm->mem, 0, sizeof(vm->mem));
  vm->pc = 0;
  vm->ic = 0;
  vm->dtop = 0;
  vm->rtop = 0;
  vm->flag_z = 0;
  vm->flag_n = 0;
  vm->trace = 0;
  vm->last_op = 0;
  vm->io_beat_div = 0;
  vm->last_out = 0;
  vm->bp_valid = 0;
  vm->bp_addr = 0;
  vm->mig_enforce_blocked = 0U;

  if (mig_initialized == 0U) {
    vm->mig_mode = VM32_MIG_MODE_OFF;
    vm->mig_allow_mask = VM32_MIG_RES_ALL;
    vm->mig_violations = 0U;
    vm->mig_last_addr = 0U;
    vm->mig_last_write = 0U;
    vm->mig_last_resource = VM32_MIG_RESOURCE_NONE;
    vm->mig_initialized = 1U;
  } else {
    vm->mig_mode = mig_mode;
    vm->mig_allow_mask = mig_allow_mask;
    vm->mig_violations = mig_violations;
    vm->mig_last_addr = mig_last_addr;
    vm->mig_last_write = mig_last_write;
    vm->mig_last_resource = mig_last_resource;
    vm->mig_initialized = 1U;
  }
}

static uint8_t vm32_fetch8(Vm32 *vm)
{
  uint32_t addr = vm->pc % VM32_MEM_SIZE;
  uint8_t v = vm->mem[addr];
  vm->pc = (vm->pc + 1U) % VM32_MEM_SIZE;
  return v;
}

static uint32_t vm32_fetch32(Vm32 *vm)
{
  uint32_t v = 0;
  v |= (uint32_t)vm32_fetch8(vm);
  v |= (uint32_t)vm32_fetch8(vm) << 8;
  v |= (uint32_t)vm32_fetch8(vm) << 16;
  v |= (uint32_t)vm32_fetch8(vm) << 24;
  return v;
}

static Vm32Result vm32_push(Vm32 *vm, uint32_t v)
{
  if (vm->dtop >= VM32_DS_SIZE) {
    return VM32_ERR_STACK;
  }
  vm->ds[vm->dtop++] = v;
  vm->flag_z = (v == 0U);
  vm->flag_n = (v >> 31) & 1U;
  return VM32_OK;
}

static Vm32Result vm32_pop(Vm32 *vm, uint32_t *out)
{
  if (vm->dtop == 0) {
    return VM32_ERR_STACK;
  }
  vm->dtop--;
  if (out != NULL) {
    *out = vm->ds[vm->dtop];
  }
  return VM32_OK;
}

static Vm32Result vm32_rpush(Vm32 *vm, uint32_t v)
{
  if (vm->rtop >= VM32_RS_SIZE) {
    return VM32_ERR_STACK;
  }
  vm->rs[vm->rtop++] = v;
  return VM32_OK;
}

static Vm32Result vm32_rpop(Vm32 *vm, uint32_t *out)
{
  if (vm->rtop == 0) {
    return VM32_ERR_STACK;
  }
  vm->rtop--;
  if (out != NULL) {
    *out = vm->rs[vm->rtop];
  }
  return VM32_OK;
}

static uint8_t vm32_read8(Vm32 *vm, uint32_t addr)
{
  if (addr == VM32_IO_UART_RX) {
    char c;
    if (board_uart_read_char(&c)) {
      return (uint8_t)c;
    }
    return 0;
  }
  if (addr == VM32_IO_IC) {
    return (uint8_t)(vm->ic & 0xFFU);
  }
  uint32_t a = addr % VM32_MEM_SIZE;
  return vm->mem[a];
}

static void vm32_write8(Vm32 *vm, uint32_t addr, uint8_t v)
{
  if (addr == VM32_IO_UART_TX) {
    char out[2] = { (char)v, '\0' };
    board_uart_write(out);
    vm->last_out = v;
    return;
  }
  if (addr == VM32_IO_LED) {
    if (v & 1U) {
      board_led_on();
    } else {
      board_led_off();
    }
    return;
  }
  uint32_t a = addr % VM32_MEM_SIZE;
  vm->mem[a] = v;
}

static int8_t vm32_fetch8s(Vm32 *vm)
{
  return (int8_t)vm32_fetch8(vm);
}

static void vm32_trace(Vm32 *vm, uint8_t op)
{
  if (vm == NULL || vm->trace == 0U) {
    return;
  }
  char buf[96];
  uint32_t ds_top = (vm->dtop > 0) ? vm->ds[vm->dtop - 1] : 0U;
  uint32_t rs_top = (vm->rtop > 0) ? vm->rs[vm->rtop - 1] : 0U;
  snprintf(buf, sizeof(buf),
           "vm pc=%lu op=0x%02X ds=%lu rs=%lu z=%u n=%u ic=%lu\r\n",
           (unsigned long)vm->pc,
           (unsigned)op,
           (unsigned long)ds_top,
           (unsigned long)rs_top,
           (unsigned)vm->flag_z,
           (unsigned)vm->flag_n,
           (unsigned long)vm->ic);
  board_uart_write(buf);
}

Vm32Result vm32_step(Vm32 *vm)
{
  if (vm == NULL) {
    return VM32_ERR_MEM;
  }

  uint8_t op = vm32_fetch8(vm);
  vm->last_op = op;
  vm->ic++;
  if (vm->io_beat_div != 0U && (vm->ic % vm->io_beat_div) == 0U) {
    board_led_toggle();
  }
  vm32_trace(vm, op);

  switch (op) {
    case VM32_OP_NOP:
      return VM32_OK;
    case VM32_OP_HALT:
      return VM32_ERR_HALT;
    case VM32_OP_PUSH: {
      uint32_t imm = vm32_fetch32(vm);
      return vm32_push(vm, imm);
    }
    case VM32_OP_DUP: {
      if (vm->dtop == 0) {
        return VM32_ERR_STACK;
      }
      return vm32_push(vm, vm->ds[vm->dtop - 1]);
    }
    case VM32_OP_DROP: {
      return vm32_pop(vm, NULL);
    }
    case VM32_OP_SWAP: {
      if (vm->dtop < 2) {
        return VM32_ERR_STACK;
      }
      uint32_t a = vm->ds[vm->dtop - 1];
      vm->ds[vm->dtop - 1] = vm->ds[vm->dtop - 2];
      vm->ds[vm->dtop - 2] = a;
      return VM32_OK;
    }
    case VM32_OP_OVER: {
      if (vm->dtop < 2) {
        return VM32_ERR_STACK;
      }
      return vm32_push(vm, vm->ds[vm->dtop - 2]);
    }
    case VM32_OP_ADD: {
      uint32_t a, b;
      if (vm->dtop < 2U) {
        return VM32_ERR_STACK;
      }
      if (vm32_pop(vm, &b) != VM32_OK || vm32_pop(vm, &a) != VM32_OK) {
        return VM32_ERR_STACK;
      }
      return vm32_push(vm, a + b);
    }
    case VM32_OP_SUB: {
      uint32_t a, b;
      if (vm->dtop < 2U) {
        return VM32_ERR_STACK;
      }
      if (vm32_pop(vm, &b) != VM32_OK || vm32_pop(vm, &a) != VM32_OK) {
        return VM32_ERR_STACK;
      }
      return vm32_push(vm, a - b);
    }
    case VM32_OP_AND: {
      uint32_t a, b;
      if (vm->dtop < 2U) {
        return VM32_ERR_STACK;
      }
      if (vm32_pop(vm, &b) != VM32_OK || vm32_pop(vm, &a) != VM32_OK) {
        return VM32_ERR_STACK;
      }
      return vm32_push(vm, a & b);
    }
    case VM32_OP_OR: {
      uint32_t a, b;
      if (vm->dtop < 2U) {
        return VM32_ERR_STACK;
      }
      if (vm32_pop(vm, &b) != VM32_OK || vm32_pop(vm, &a) != VM32_OK) {
        return VM32_ERR_STACK;
      }
      return vm32_push(vm, a | b);
    }
    case VM32_OP_XOR: {
      uint32_t a, b;
      if (vm->dtop < 2U) {
        return VM32_ERR_STACK;
      }
      if (vm32_pop(vm, &b) != VM32_OK || vm32_pop(vm, &a) != VM32_OK) {
        return VM32_ERR_STACK;
      }
      return vm32_push(vm, a ^ b);
    }
    case VM32_OP_NOT: {
      if (vm->dtop == 0) {
        return VM32_ERR_STACK;
      }
      vm->ds[vm->dtop - 1] = ~vm->ds[vm->dtop - 1];
      vm->flag_z = (vm->ds[vm->dtop - 1] == 0U);
      vm->flag_n = (vm->ds[vm->dtop - 1] >> 31) & 1U;
      return VM32_OK;
    }
    case VM32_OP_SHL: {
      uint32_t a, b;
      if (vm->dtop < 2U) {
        return VM32_ERR_STACK;
      }
      if (vm32_pop(vm, &b) != VM32_OK || vm32_pop(vm, &a) != VM32_OK) {
        return VM32_ERR_STACK;
      }
      return vm32_push(vm, a << (b & 31U));
    }
    case VM32_OP_SHR: {
      uint32_t a, b;
      if (vm->dtop < 2U) {
        return VM32_ERR_STACK;
      }
      if (vm32_pop(vm, &b) != VM32_OK || vm32_pop(vm, &a) != VM32_OK) {
        return VM32_ERR_STACK;
      }
      return vm32_push(vm, a >> (b & 31U));
    }
    case VM32_OP_LOAD: {
      uint32_t addr;
      if (vm32_pop(vm, &addr) != VM32_OK) {
        return VM32_ERR_STACK;
      }
      if (!vm32_mig_check_io(vm, addr, 0U) ||
          !vm32_mig_check_io(vm, addr + 1U, 0U) ||
          !vm32_mig_check_io(vm, addr + 2U, 0U) ||
          !vm32_mig_check_io(vm, addr + 3U, 0U)) {
        seam_emit(CFL_LAYER_VM, CFL_EV_VM_POLICY_FAIL, addr, 0U /*LOAD*/, 0, 0);
        return VM32_ERR_POLICY;
      }
      uint32_t v = 0;
      v |= (uint32_t)vm32_read8(vm, addr);
      v |= (uint32_t)vm32_read8(vm, addr + 1U) << 8;
      v |= (uint32_t)vm32_read8(vm, addr + 2U) << 16;
      v |= (uint32_t)vm32_read8(vm, addr + 3U) << 24;
      return vm32_push(vm, v);
    }
    case VM32_OP_STORE: {
      uint32_t addr, v;
      if (vm->dtop < 2U) {
        return VM32_ERR_STACK;
      }
      if (vm32_pop(vm, &addr) != VM32_OK || vm32_pop(vm, &v) != VM32_OK) {
        return VM32_ERR_STACK;
      }
      if (!vm32_mig_check_io(vm, addr, 1U) ||
          !vm32_mig_check_io(vm, addr + 1U, 1U) ||
          !vm32_mig_check_io(vm, addr + 2U, 1U) ||
          !vm32_mig_check_io(vm, addr + 3U, 1U)) {
        seam_emit(CFL_LAYER_VM, CFL_EV_VM_POLICY_FAIL, addr, 1U /*STORE*/, 0, 0);
        return VM32_ERR_POLICY;
      }
      vm32_write8(vm, addr, (uint8_t)(v & 0xFFU));
      vm32_write8(vm, addr + 1U, (uint8_t)((v >> 8) & 0xFFU));
      vm32_write8(vm, addr + 2U, (uint8_t)((v >> 16) & 0xFFU));
      vm32_write8(vm, addr + 3U, (uint8_t)((v >> 24) & 0xFFU));
      return VM32_OK;
    }
    case VM32_OP_IN: {
      uint32_t addr;
      if (vm32_pop(vm, &addr) != VM32_OK) {
        return VM32_ERR_STACK;
      }
      if (!vm32_mig_check_io(vm, addr, 0U)) {
        seam_emit(CFL_LAYER_VM, CFL_EV_VM_POLICY_FAIL, addr, 2U /*IN*/, 0, 0);
        return VM32_ERR_POLICY;
      }
      uint32_t v = vm32_read8(vm, addr);
      return vm32_push(vm, v);
    }
    case VM32_OP_OUT: {
      uint32_t addr, v;
      if (vm->dtop < 2U) {
        return VM32_ERR_STACK;
      }
      if (vm32_pop(vm, &addr) != VM32_OK || vm32_pop(vm, &v) != VM32_OK) {
        return VM32_ERR_STACK;
      }
      if (!vm32_mig_check_io(vm, addr, 1U)) {
        seam_emit(CFL_LAYER_VM, CFL_EV_VM_POLICY_FAIL, addr, 3U /*OUT*/, 0, 0);
        return VM32_ERR_POLICY;
      }
      vm32_write8(vm, addr, (uint8_t)(v & 0xFFU));
      return VM32_OK;
    }
    case VM32_OP_JZ: {
      int8_t rel = vm32_fetch8s(vm);
      if (vm->dtop == 0) {
        return VM32_ERR_STACK;
      }
      if (vm->ds[vm->dtop - 1] == 0U) {
        vm->pc = (uint32_t)((int32_t)vm->pc + (int32_t)rel) % VM32_MEM_SIZE;
      }
      return VM32_OK;
    }
    case VM32_OP_CALL: {
      int8_t rel = vm32_fetch8s(vm);
      if (vm32_rpush(vm, vm->pc) != VM32_OK) {
        return VM32_ERR_STACK;
      }
      vm->pc = (uint32_t)((int32_t)vm->pc + (int32_t)rel) % VM32_MEM_SIZE;
      return VM32_OK;
    }
    case VM32_OP_RET: {
      uint32_t ret;
      if (vm32_rpop(vm, &ret) != VM32_OK) {
        return VM32_ERR_STACK;
      }
      vm->pc = ret % VM32_MEM_SIZE;
      return VM32_OK;
    }
    default:
      return VM32_ERR_MEM;
  }
}
