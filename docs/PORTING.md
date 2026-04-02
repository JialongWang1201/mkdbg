# Porting Guide — wire + seam on a new MCU

This guide explains how to add mkdbg support to any microcontroller.
You need to implement two thin C functions and call one hook from your fault handler.
No OS or debug probe required.

---

## What you are porting

mkdbg is two halves:

```
  Host (your laptop)           Target (your MCU)
  ─────────────────            ─────────────────────────────
  mkdbg-native ◄──── UART ────► wire agent  (wire_on_fault)
                                seam agent  (optional — event ring)
```

**wire** is the crash reporter: when the MCU faults, it halts and sends CPU
state over UART.  The host reads it and prints a crash report.

**seam** is the causal chain recorder: a 64-entry ring buffer you feed
events into during normal operation.  When the crash happens, the ring is
included in the crash dump so you can see *what led to the fault*.

---

## Step 1 — UART send/receive

Implement two functions in your BSP or UART driver:

```c
#include "wire_agent.h"

/* Send len bytes over the debug UART. Blocking. */
void wire_uart_send(const uint8_t *buf, size_t len)
{
    HAL_UART_Transmit(&huart2, (uint8_t *)buf, len, HAL_MAX_DELAY);
}

/* Receive len bytes from the debug UART. Blocking. */
void wire_uart_recv(uint8_t *buf, size_t len)
{
    HAL_UART_Receive(&huart2, buf, len, HAL_MAX_DELAY);
}
```

Use whatever UART you already use for logging.  Any baud rate works; 115200
is the default mkdbg expects.  The functions must be **blocking** — do not
return until all bytes are sent/received.

---

## Step 2 — fault handler hook

Call `wire_on_fault()` from your HardFault handler (and any other fault
handlers you want to catch):

```c
/* startup/fault_handler.c */
#include "wire_agent.h"

void HardFault_Handler(void)
{
    wire_on_fault();   /* halts CPU, then enters RSP stub loop over UART */
    /* never returns */
}
```

`wire_on_fault()` does three things:
1. Disables interrupts.
2. Enters a minimal GDB RSP stub that answers `?`, `g`, and `m` commands.
3. Never returns — the MCU stays halted until reset.

---

## Step 3 — link wire_agent into your firmware

Add the wire agent source to your build.  The agent is a single C file with
no dependencies beyond your `wire_uart_send` / `wire_uart_recv`:

```
deps/wire/src/wire_agent.c
deps/wire/include/wire_agent.h
```

CMake example:

```cmake
target_sources(my_firmware PRIVATE
  ${WIRE_DIR}/src/wire_agent.c
)
target_include_directories(my_firmware PRIVATE
  ${WIRE_DIR}/include
)
```

No OS tick, no heap, no CMSIS required.  The agent is ~300 lines of C99.

---

## Step 4 — attach after a crash

```bash
mkdbg attach --port /dev/ttyUSB0 --arch cortex-m
```

mkdbg reads the halt signal, all 17 Cortex-M registers, the CFSR fault
status register, and 256 bytes of stack, then prints a human-readable report
in under a second.

---

## Optional — break-in (soft-halt a running MCU)

By default `wire_uart_try_read()` is a weak stub that always returns 0.
Override it in your BSP to allow the host to halt a running MCU without a
hardware debug probe:

```c
/* bsp/uart.c — non-blocking UART read for break-in detection */
#include "wire.h"

int wire_uart_try_read(uint8_t *byte)
{
    if (__HAL_UART_GET_FLAG(&huart2, UART_FLAG_RXNE)) {
        *byte = (uint8_t)huart2.Instance->RDR;
        return 1;
    }
    return 0;
}
```

Then call `wire_poll_break_in()` from your main loop or RTOS idle hook:

```c
for (;;) {
    wire_poll_break_in();   /* detects Ctrl-C (0x03) from mkdbg */
    do_work();
}
```

When the host sends Ctrl-C (via `mkdbg debug` → `interrupt` command or `i`
key in TUI mode), `wire_poll_break_in()` pends DebugMonitor and the CPU
halts at the next instruction.

Requires `WIRE_LIVE_DEBUG=1` and `wire_enable_debug_monitor()` to have been
called (done automatically by `wire_init()` when `WIRE_LIVE_DEBUG` is
defined).

---

## Optional — seam event ring

seam lets you record what happened *before* the crash.  Feed it events during
normal operation:

```c
#include "seam_agent.h"

/* anywhere in your firmware — task dispatch, state changes, etc. */
seam_emit(SEAM_EVT_TASK_SWITCH, task_id);
seam_emit(SEAM_EVT_IRQ_ENTER,   irq_num);
```

The ring buffer is 64 entries, zero-malloc.  On fault, `wire_on_fault()`
includes the ring in the crash dump automatically if seam is linked.

Analyze a captured dump:

```bash
mkdbg seam analyze capture.cfl
```

---

## Porting checklist

```
[ ] implement wire_uart_send(buf, len)  — blocking UART write
[ ] implement wire_uart_recv(buf, len)  — blocking UART read
[ ] call wire_on_fault() from HardFault_Handler
[ ] link deps/wire/src/wire_agent.c into firmware
[ ] flash and test: trigger a HardFault, run mkdbg attach
[ ] optional: link deps/seam/src/seam_agent.c, call seam_emit()
[ ] optional: override wire_uart_try_read(), call wire_poll_break_in() in main loop
```

---

## Reference implementation

The STM32F446RE port lives in `examples/stm32f446/`.  It covers:

- UART HAL wiring (`examples/stm32f446/bsp/uart.c`)
- FreeRTOS HardFault handler (`examples/stm32f446/src/interrupts.c`)
- CMake integration (`examples/stm32f446/CMakeLists.txt`)

---

## Troubleshooting

**`mkdbg attach` times out immediately**

The MCU is not halted in the RSP stub.  Check:
- `wire_on_fault()` is actually called from your fault handler
- `wire_uart_send` / `wire_uart_recv` use the same UART port you pass to `--port`
- Baud rate matches (default 115200)

**Garbage characters on attach**

Baud rate mismatch.  Pass `--baud <rate>` to mkdbg.

**Only zeros in the register dump**

`parse_registers` got a short RSP response.  Verify `wire_uart_recv` blocks
until all bytes arrive (not just one read-and-return).

**Live debug breakpoints not firing on Cortex-M7 or Cortex-M33**

These cores use FPBv2 (Flash Patch and Breakpoint revision 2).  The wire
agent auto-detects the revision at runtime by reading `FPB_CTRL.REV`
(bits [31:28]).  FPBv2 requires a KEY bit alongside ENABLE in the same
write to `FP_CTRL` — a read-modify-write is silently ignored.

`wire_enable_debug_monitor()` handles this automatically.  If breakpoints
still do not fire, check that `DEMCR.MON_EN` is set and that DebugMonitor
is not masked by `BASEPRI` or `PRIMASK` on your target.
