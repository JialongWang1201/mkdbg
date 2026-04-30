<img src="docs/assets/logo.svg" alt="mkdbg" width="400"/>

[![CI](https://github.com/JialongWang1201/mkdbg/actions/workflows/ci.yml/badge.svg)](https://github.com/JialongWang1201/mkdbg/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/JialongWang1201/mkdbg?include_prereleases&label=release)](https://github.com/JialongWang1201/mkdbg/releases/latest)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

Crash diagnostics and live debugging for embedded firmware over UART or hardware probe.

```
$ mkdbg attach --port /dev/ttyUSB0

FAULT: HardFault (STKERR — stack overflow)
  PC  0x0800a1f4  task_sensor_run+0x3e
  LR  0x08009e34  vTaskStartScheduler

Backtrace:
  #0  fault_handler
  #1  task_sensor_run
  #2  vTaskStartScheduler
```

---

## Installation

Download a pre-built binary from the [latest release](https://github.com/JialongWang1201/mkdbg/releases/latest):

| Platform | Binary |
|----------|--------|
| Linux x86\_64 | `mkdbg-native-linux-x86_64` |
| Linux arm64 | `mkdbg-native-linux-arm64` |
| macOS Apple Silicon | `mkdbg-native-darwin-arm64` |
| macOS Intel | `mkdbg-native-darwin-x86_64` |

Or use the install script:

```bash
curl -fsSL https://raw.githubusercontent.com/JialongWang1201/mkdbg/main/scripts/install.sh | bash
```

**Build from source** — requires `cmake`, a C compiler, and `cargo` for probe support:

```bash
git clone --recurse-submodules https://github.com/JialongWang1201/mkdbg
cmake -S mkdbg -B mkdbg/build -DCMAKE_BUILD_TYPE=Release
cmake --build mkdbg/build --target mkdbg-native
```

**Linux udev rules** (required for ST-Link / CMSIS-DAP / J-Link without root):

```bash
sudo cp tools/69-probe-rs.rules /etc/udev/rules.d/ && sudo udevadm control --reload
```

---

## Firmware agent

Link the `wire` agent into your firmware (~300 lines of C99, no OS dependencies):

```c
// HardFault_Handler:
wire_on_fault();

// UART driver:
void wire_uart_send(const uint8_t *buf, size_t len) { /* HAL */ }
void wire_uart_recv(uint8_t *buf, size_t len)       { /* HAL */ }
```

See [`docs/PORTING.md`](docs/PORTING.md). Reference implementation: [`examples/stm32f446/`](examples/stm32f446/).

---

## Usage

```bash
# Crash report over UART
mkdbg attach --port /dev/ttyUSB0

# Crash report via hardware probe (ST-Link / CMSIS-DAP / J-Link)
mkdbg attach --probe --chip STM32F446RETx

# Interactive debug TUI
mkdbg debug --port /dev/ttyUSB0 --elf build/firmware.elf
mkdbg debug --probe --chip STM32F446RETx --elf build/firmware.elf

# Causal analysis from fault event ring
mkdbg seam analyze capture.cfl

# GDB bridge (connects arm-none-eabi-gdb without a probe)
wire-host --port /dev/ttyUSB0
```

---

## Architecture support

|  | Cortex-M0/M3/M4/M7 | RISC-V 32 |
|--|:------------------:|:---------:|
| `mkdbg attach` | ✓ | ✓ |
| `mkdbg debug` (TUI) | ✓ | ✓ |
| Hardware probe | ✓ | — |
| FPU registers | ✓ | — |

---

## Documentation

| | |
|-|-|
| [`docs/COMMANDS.md`](docs/COMMANDS.md) | Command reference and config format |
| [`docs/PORTING.md`](docs/PORTING.md) | Porting the firmware agent |
| [`docs/DEVELOPER_GUIDE.md`](docs/DEVELOPER_GUIDE.md) | Build system, testing, arch plugins |

---

## License

MIT. The `seam` and `wire` submodules are MIT. libgit2 is MIT.
