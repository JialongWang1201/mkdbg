#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vm32.h"

static char uart_tx[256];
static size_t uart_tx_len;
static char uart_rx[64];
static size_t uart_rx_len;
static size_t uart_rx_pos;
static int led_state;
static int led_toggle_count;

void board_uart_write(const char *s) {
  if (s == NULL) {
    return;
  }
  while (*s != '\0' && uart_tx_len + 1 < sizeof(uart_tx)) {
    uart_tx[uart_tx_len++] = *s++;
  }
  uart_tx[uart_tx_len] = '\0';
}

int board_uart_read_char(char *out) {
  if (out == NULL) {
    return 0;
  }
  if (uart_rx_pos >= uart_rx_len) {
    return 0;
  }
  *out = uart_rx[uart_rx_pos++];
  return 1;
}

void board_led_on(void) { led_state = 1; }
void board_led_off(void) { led_state = 0; }
void board_led_toggle(void) {
  led_state = !led_state;
  led_toggle_count++;
}

static void reset_io(void) {
  uart_tx_len = 0;
  uart_tx[0] = '\0';
  uart_rx_len = 0;
  uart_rx_pos = 0;
  led_state = 0;
  led_toggle_count = 0;
}

static void set_uart_rx(const char *s) {
  reset_io();
  if (s == NULL) {
    return;
  }
  uart_rx_len = strlen(s);
  if (uart_rx_len > sizeof(uart_rx)) {
    uart_rx_len = sizeof(uart_rx);
  }
  memcpy(uart_rx, s, uart_rx_len);
  uart_rx_pos = 0;
}

static void load_prog(Vm32 *vm, const uint8_t *prog, size_t len) {
  memset(vm->mem, 0, sizeof(vm->mem));
  if (len > sizeof(vm->mem)) {
    len = sizeof(vm->mem);
  }
  memcpy(vm->mem, prog, len);
  vm->pc = 0;
  vm->ic = 0;
  vm->dtop = 0;
  vm->rtop = 0;
  vm->flag_z = 0;
  vm->flag_n = 0;
  vm->trace = 0;
  vm->io_beat_div = 0;
  vm->last_out = 0;
  vm->bp_valid = 0;
  vm->bp_addr = 0;
}

static int run_until(Vm32 *vm, Vm32Result *out, uint32_t max_steps) {
  for (uint32_t i = 0; i < max_steps; i++) {
    Vm32Result res = vm32_step(vm);
    if (res != VM32_OK) {
      if (out != NULL) {
        *out = res;
      }
      return 0;
    }
  }
  return -1;
}

static int failures = 0;

static void check(int cond, const char *msg) {
  if (!cond) {
    printf("FAIL: %s\n", msg);
    failures++;
  }
}

static void test_push_halt(void) {
  Vm32 vm;
  vm32_reset(&vm);
  reset_io();
  uint8_t prog[] = {
    VM32_OP_PUSH, 0x04, 0x03, 0x02, 0x01,
    VM32_OP_HALT
  };
  load_prog(&vm, prog, sizeof(prog));
  Vm32Result res = VM32_OK;
  run_until(&vm, &res, 10);
  check(res == VM32_ERR_HALT, "push+halt result");
  check(vm.dtop == 1, "push+halt dtop");
  check(vm.ds[0] == 0x01020304U, "push+halt value");
}

static void test_stack_ops(void) {
  Vm32 vm;
  vm32_reset(&vm);
  reset_io();
  uint8_t prog[] = {
    VM32_OP_PUSH, 0x01, 0x00, 0x00, 0x00,
    VM32_OP_PUSH, 0x02, 0x00, 0x00, 0x00,
    VM32_OP_SWAP,
    VM32_OP_OVER,
    VM32_OP_ADD,
    VM32_OP_DROP,
    VM32_OP_HALT
  };
  load_prog(&vm, prog, sizeof(prog));
  Vm32Result res = VM32_OK;
  run_until(&vm, &res, 20);
  check(res == VM32_ERR_HALT, "stack ops result");
  check(vm.dtop == 1, "stack ops dtop");
  check(vm.ds[0] == 2U, "stack ops value");
}

static void test_alu(void) {
  Vm32 vm;
  vm32_reset(&vm);
  reset_io();
  uint8_t prog[] = {
    VM32_OP_PUSH, 0x05, 0x00, 0x00, 0x00,
    VM32_OP_PUSH, 0x03, 0x00, 0x00, 0x00,
    VM32_OP_SUB,
    VM32_OP_PUSH, 0x01, 0x00, 0x00, 0x00,
    VM32_OP_SHL,
    VM32_OP_HALT
  };
  load_prog(&vm, prog, sizeof(prog));
  Vm32Result res = VM32_OK;
  run_until(&vm, &res, 20);
  check(res == VM32_ERR_HALT, "alu result");
  check(vm.dtop == 1, "alu dtop");
  check(vm.ds[0] == 4U, "alu value");
}

static void test_load_store(void) {
  Vm32 vm;
  vm32_reset(&vm);
  reset_io();
  const uint8_t addr = 0x40;
  uint8_t prog[] = {
    VM32_OP_PUSH, 0x44, 0x33, 0x22, 0x11,
    VM32_OP_PUSH, addr, 0x00, 0x00, 0x00,
    VM32_OP_STORE,
    VM32_OP_PUSH, addr, 0x00, 0x00, 0x00,
    VM32_OP_LOAD,
    VM32_OP_HALT
  };
  load_prog(&vm, prog, sizeof(prog));
  Vm32Result res = VM32_OK;
  run_until(&vm, &res, 30);
  check(res == VM32_ERR_HALT, "load/store result");
  check(vm.dtop == 1, "load/store dtop");
  check(vm.ds[0] == 0x11223344U, "load/store value");
  check(vm.mem[addr] == 0x44, "store byte0");
  check(vm.mem[addr + 1] == 0x33, "store byte1");
  check(vm.mem[addr + 2] == 0x22, "store byte2");
  check(vm.mem[addr + 3] == 0x11, "store byte3");
}

static void test_control_flow(void) {
  Vm32 vm;
  vm32_reset(&vm);
  reset_io();
  uint8_t prog[] = {
    VM32_OP_PUSH, 0x00, 0x00, 0x00, 0x00,
    VM32_OP_JZ, 0x05,
    VM32_OP_PUSH, 0xEF, 0xBE, 0xAD, 0xDE,
    VM32_OP_HALT
  };
  load_prog(&vm, prog, sizeof(prog));
  Vm32Result res = VM32_OK;
  run_until(&vm, &res, 20);
  check(res == VM32_ERR_HALT, "jz result");
  check(vm.dtop == 1, "jz dtop");
  check(vm.ds[0] == 0U, "jz value");
}

static void test_call_ret(void) {
  Vm32 vm;
  vm32_reset(&vm);
  reset_io();
  uint8_t prog[] = {
    VM32_OP_CALL, 0x01,
    VM32_OP_HALT,
    VM32_OP_PUSH, 0x07, 0x00, 0x00, 0x00,
    VM32_OP_RET
  };
  load_prog(&vm, prog, sizeof(prog));
  Vm32Result res = VM32_OK;
  run_until(&vm, &res, 20);
  check(res == VM32_ERR_HALT, "call/ret result");
  check(vm.dtop == 1, "call/ret dtop");
  check(vm.ds[0] == 7U, "call/ret value");
}

static void test_io_uart(void) {
  Vm32 vm;
  vm32_reset(&vm);
  reset_io();
  uint8_t prog[] = {
    VM32_OP_PUSH, 0x41, 0x00, 0x00, 0x00,
    VM32_OP_PUSH, 0xF0, 0x0F, 0x00, 0x00,
    VM32_OP_OUT,
    VM32_OP_HALT
  };
  load_prog(&vm, prog, sizeof(prog));
  Vm32Result res = VM32_OK;
  run_until(&vm, &res, 10);
  check(res == VM32_ERR_HALT, "uart out result");
  check(vm.last_out == 0x41U, "uart out last_out");
  check(uart_tx_len > 0 && uart_tx[0] == 'A', "uart out buffer");
}

static void test_io_led(void) {
  Vm32 vm;
  vm32_reset(&vm);
  reset_io();
  uint8_t prog[] = {
    VM32_OP_PUSH, 0x01, 0x00, 0x00, 0x00,
    VM32_OP_PUSH, 0xF8, 0x0F, 0x00, 0x00,
    VM32_OP_OUT,
    VM32_OP_PUSH, 0x00, 0x00, 0x00, 0x00,
    VM32_OP_PUSH, 0xF8, 0x0F, 0x00, 0x00,
    VM32_OP_OUT,
    VM32_OP_HALT
  };
  load_prog(&vm, prog, sizeof(prog));
  Vm32Result res = VM32_OK;
  run_until(&vm, &res, 20);
  check(res == VM32_ERR_HALT, "led out result");
  check(led_state == 0, "led final state");
}

static void test_io_uart_in(void) {
  Vm32 vm;
  vm32_reset(&vm);
  set_uart_rx("Z");
  uint8_t prog[] = {
    VM32_OP_PUSH, 0xF4, 0x0F, 0x00, 0x00,
    VM32_OP_IN,
    VM32_OP_HALT
  };
  load_prog(&vm, prog, sizeof(prog));
  Vm32Result res = VM32_OK;
  run_until(&vm, &res, 10);
  check(res == VM32_ERR_HALT, "uart in result");
  check(vm.dtop == 1, "uart in dtop");
  check(vm.ds[0] == (uint32_t)'Z', "uart in value");
}

static void test_errors(void) {
  Vm32 vm;
  vm32_reset(&vm);
  reset_io();
  uint8_t prog1[] = { VM32_OP_DROP };
  load_prog(&vm, prog1, sizeof(prog1));
  Vm32Result res = vm32_step(&vm);
  check(res == VM32_ERR_STACK, "drop underflow");

  uint8_t prog2[] = { 0x99 };
  load_prog(&vm, prog2, sizeof(prog2));
  res = vm32_step(&vm);
  check(res == VM32_ERR_MEM, "illegal opcode");
}

static void test_wrap_store(void) {
  Vm32 vm;
  vm32_reset(&vm);
  reset_io();
  uint32_t addr = (uint32_t)(VM32_MEM_SIZE - 2U);
  uint8_t prog[] = {
    VM32_OP_PUSH, 0xDD, 0xCC, 0xBB, 0xAA,
    VM32_OP_PUSH,
    (uint8_t)(addr & 0xFFU),
    (uint8_t)((addr >> 8) & 0xFFU),
    (uint8_t)((addr >> 16) & 0xFFU),
    (uint8_t)((addr >> 24) & 0xFFU),
    VM32_OP_STORE,
    VM32_OP_HALT
  };
  load_prog(&vm, prog, sizeof(prog));
  Vm32Result res = VM32_OK;
  run_until(&vm, &res, 20);
  check(res == VM32_ERR_HALT, "wrap store result");
  check(vm.mem[addr % VM32_MEM_SIZE] == 0xDD, "wrap byte0");
  check(vm.mem[(addr + 1U) % VM32_MEM_SIZE] == 0xCC, "wrap byte1");
  check(vm.mem[(addr + 2U) % VM32_MEM_SIZE] == 0xBB, "wrap byte2");
  check(vm.mem[(addr + 3U) % VM32_MEM_SIZE] == 0xAA, "wrap byte3");
}

static void test_mig_monitor_mode(void) {
  Vm32 vm;
  Vm32MigStatus st;
  vm32_reset(&vm);
  reset_io();
  vm32_mig_reset(&vm);
  vm32_mig_set_mode(&vm, VM32_MIG_MODE_MONITOR);
  vm32_mig_deny(&vm, VM32_MIG_RES_UART_TX);

  uint8_t prog[] = {
    VM32_OP_PUSH, 0x41, 0x00, 0x00, 0x00,
    VM32_OP_PUSH, 0xF0, 0x0F, 0x00, 0x00,
    VM32_OP_OUT,
    VM32_OP_HALT
  };
  load_prog(&vm, prog, sizeof(prog));
  Vm32Result res = VM32_OK;
  run_until(&vm, &res, 10);
  vm32_mig_status(&vm, &st);

  check(res == VM32_ERR_HALT, "mig monitor result");
  check(uart_tx_len > 0 && uart_tx[0] == 'A', "mig monitor allows write");
  check(st.mode == VM32_MIG_MODE_MONITOR, "mig monitor mode");
  check(st.violations == 1U, "mig monitor violation count");
  check(st.enforce_blocked == 0U, "mig monitor blocked flag");
  check(st.last_write == 1U, "mig monitor last write");
  check(strcmp(vm32_mig_resource_name(st.last_resource), "uart_tx") == 0, "mig monitor resource");
}

static void test_mig_enforce_mode(void) {
  Vm32 vm;
  Vm32MigStatus st;
  vm32_reset(&vm);
  reset_io();
  vm32_mig_reset(&vm);
  vm32_mig_set_mode(&vm, VM32_MIG_MODE_ENFORCE);
  vm32_mig_deny(&vm, VM32_MIG_RES_UART_TX);

  uint8_t prog[] = {
    VM32_OP_PUSH, 0x41, 0x00, 0x00, 0x00,
    VM32_OP_PUSH, 0xF0, 0x0F, 0x00, 0x00,
    VM32_OP_OUT,
    VM32_OP_HALT
  };
  load_prog(&vm, prog, sizeof(prog));
  Vm32Result res = VM32_OK;
  run_until(&vm, &res, 10);
  vm32_mig_status(&vm, &st);

  check(res == VM32_ERR_POLICY, "mig enforce result");
  check(uart_tx_len == 0, "mig enforce blocks write");
  check(st.mode == VM32_MIG_MODE_ENFORCE, "mig enforce mode");
  check(st.violations == 1U, "mig enforce violation count");
  check(st.enforce_blocked == 1U, "mig enforce blocked flag");
  check(st.last_write == 1U, "mig enforce last write");
  check(strcmp(vm32_mig_resource_name(st.last_resource), "uart_tx") == 0, "mig enforce resource");
}

static void test_mig_persist_across_vm_reset(void) {
  Vm32 vm;
  Vm32MigStatus st;
  vm32_reset(&vm);
  reset_io();
  vm32_mig_reset(&vm);
  vm32_mig_set_mode(&vm, VM32_MIG_MODE_MONITOR);
  vm32_mig_deny(&vm, VM32_MIG_RES_LED | VM32_MIG_RES_IC);
  vm32_reset(&vm);
  vm32_mig_status(&vm, &st);

  check(st.mode == VM32_MIG_MODE_MONITOR, "mig mode persists after vm reset");
  check((st.allow_mask & VM32_MIG_RES_LED) == 0U, "mig deny led persists");
  check((st.allow_mask & VM32_MIG_RES_IC) == 0U, "mig deny ic persists");
}

int main(void) {
  test_push_halt();
  test_stack_ops();
  test_alu();
  test_load_store();
  test_control_flow();
  test_call_ret();
  test_io_uart();
  test_io_led();
  test_io_uart_in();
  test_errors();
  test_wrap_store();
  test_mig_monitor_mode();
  test_mig_enforce_mode();
  test_mig_persist_across_vm_reset();

  if (failures == 0) {
    printf("vm32_host_tests: OK\n");
    return 0;
  }
  printf("vm32_host_tests: FAIL (%d)\n", failures);
  return 1;
}
