<img src="docs/assets/logo.svg" alt="mkdbg" width="360"/>

[![CI](https://github.com/JialongWang1201/mkdbg/actions/workflows/ci.yml/badge.svg)](https://github.com/JialongWang1201/mkdbg/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/JialongWang1201/mkdbg?include_prereleases&label=release)](https://github.com/JialongWang1201/mkdbg/releases/latest)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

## Embedded firmware debugger. No debug probe required.

mkdbg diagnoses crashes and runs a live debug session on your MCU over a **plain UART connection** — the same serial cable you already use for logs. If you do have an ST-Link or CMSIS-DAP, it works with that too.

No OpenOCD configuration. No GDB install. No JTAG adapter.

```
$ mkdbg attach --port /dev/ttyUSB0

FAULT: HardFault (STKERR — stack overflow)
  PC  0x0800a1f4  task_sensor_run+0x3e
  LR  0x08009e34  vTaskStartScheduler

Backtrace:
  #0  fault_handler
  #1  task_sensor_run     ← likely culprit
  #2  vTaskStartScheduler
```

---

## Install

```bash
curl -fsSL https://raw.githubusercontent.com/JialongWang1201/mkdbg/main/scripts/install.sh | bash
```

Or download a pre-built binary directly from the [latest release](https://github.com/JialongWang1201/mkdbg/releases/latest):

| Platform | Binary |
|----------|--------|
| Linux x86\_64 | `mkdbg-native-linux-x86_64` |
| Linux arm64 | `mkdbg-native-linux-arm64` |
| macOS Apple Silicon | `mkdbg-native-darwin-arm64` |
| macOS Intel | `mkdbg-native-darwin-x86_64` |

---

## Add to your firmware

Link the `wire` agent into your project — ~300 lines of C99, no OS dependencies:

```c
// 1. Call from your fault handler:
wire_on_fault();

// 2. Implement two UART functions:
void wire_uart_send(const uint8_t *buf, size_t len) { /* your HAL */ }
void wire_uart_recv(uint8_t *buf, size_t len)       { /* your HAL */ }
```

That's the entire integration. Works with FreeRTOS, Zephyr, bare-metal, or any custom RTOS.

Full porting guide: [`docs/PORTING.md`](docs/PORTING.md) · Reference implementation: [`examples/stm32f446/`](examples/stm32f446/)

---

## What you can do

**Crash report** — decode a fault the moment it happens:
```bash
mkdbg attach --port /dev/ttyUSB0
mkdbg attach --probe --chip STM32F446RETx   # hardware probe
```

**Live debug session** — breakpoints, step, registers, inline disassembly, FreeRTOS task names:
```bash
mkdbg debug --port /dev/ttyUSB0 --elf build/firmware.elf
mkdbg debug --probe --chip STM32F446RETx --elf build/firmware.elf
```

**Causal analysis** — trace the event chain that *led* to the crash:
```bash
mkdbg seam analyze capture.cfl
```

**GDB bridge** — connect `arm-none-eabi-gdb` without a hardware probe:
```bash
wire-host --port /dev/ttyUSB0
```

---

## Architecture support

| | Cortex-M (M0/M3/M4/M7) | RISC-V 32 |
|--|:---:|:---:|
| Crash report | ✓ | ✓ |
| Live debug TUI | ✓ | ✓ |
| FPU registers | ✓ | — |
| Hardware probe | ✓ | — |

The built-in disassembler covers the full Thumb/Thumb-2 instruction set including FPU (VFPv4). No dependency on `objdump` or `addr2line`.

---

## Hardware probe support

mkdbg supports ST-Link, CMSIS-DAP, and J-Link via [probe-rs](https://probe.rs).

On Linux, USB access requires a udev rule:
```bash
sudo cp tools/69-probe-rs.rules /etc/udev/rules.d/ && sudo udevadm control --reload
```

---

## Documentation

| | |
|-|-|
| [`docs/COMMANDS.md`](docs/COMMANDS.md) | Full command reference and config format |
| [`docs/PORTING.md`](docs/PORTING.md) | Porting the firmware agent to a new board |
| [`docs/DEVELOPER_GUIDE.md`](docs/DEVELOPER_GUIDE.md) | Build system, testing, adding arch plugins |

---

## License

MIT. The `wire` and `seam` submodules are MIT. libgit2 is MIT.
