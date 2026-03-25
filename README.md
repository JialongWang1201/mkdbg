<p align="center">
  <img src="docs/assets/logo.svg" alt="mkdbg" width="480"/>
</p>

<p align="center">
  <strong>UART-only crash diagnostics and fault triage for embedded systems.</strong><br>
  No JTAG. No SWD probe. No OpenOCD. Connect a serial cable, run <code>mkdbg attach</code>.
</p>

---

```
mkdbg attach --port /dev/ttyUSB0 --arch cortex-m

FAULT: HardFault
  CFSR : 0x00000400  (STKERR — push to stack failed)
  PC   : 0x0800a1f4  fault_handler+0x8
  LR   : 0x0800a1c2  task_sensor_run+0x3e
  SP   : 0x20001fd8

Backtrace (heuristic):
  #0  0x0800a1f4  fault_handler
  #1  0x0800a1c2  task_sensor_run
  #2  0x08009e34  vTaskStartScheduler
```

## What it is

mkdbg is a host-side debug CLI for embedded firmware. It reads crash state
from a target MCU over UART using the `wire` protocol, reconstructs causal
fault chains from `seam` event ring captures, and provides a terminal
dashboard with real-time probe, build, and git status.

The firmware side is two thin C agents you drop into your RTOS:

| Agent | What it does | Lines of code |
|-------|-------------|---------------|
| `wire` | Halts on fault, exposes registers + stack over UART | ~300 |
| `seam` | Records fault events in a ring buffer, emits a COBS bundle on demand | ~200 |

The host side is a single self-contained binary with no runtime dependencies.

## Why not probe-rs / pyOCD / OpenOCD?

Those tools require a JTAG or SWD debug probe (J-Link, ST-Link, DAPLink).
mkdbg uses the UART you already have for logging. Useful when:

- You're working remotely and can't plug in a probe
- Your board doesn't expose SWD
- You want crash reports from field units over a serial console
- You want `seam` post-mortem causal chain analysis without a live debug session

For interactive debugging (breakpoints, memory inspection), use mkdbg as a
GDB bridge: `wire-host` bridges UART to TCP so `arm-none-eabi-gdb` can connect
without a probe.

## Host tools

Three binaries are built from this repo:

| Binary | Role |
|--------|------|
| `mkdbg-native` | Main CLI: attach, seam analyze, dashboard, git, build, flash |
| `seam-analyze` | Standalone causal chain analysis — pipe a `.cfl` capture file |
| `wire-host` | TCP↔UART GDB bridge for full interactive GDB sessions |

### Build

```bash
git clone --recurse-submodules https://github.com/JialongWang1201/mkdbg
cd mkdbg
cmake -S . -B build_host
cmake --build build_host
# -> build_host/mkdbg-native  build_host/seam-analyze  build_host/wire-host
```

### Install

```bash
bash scripts/install.sh
# installs mkdbg-native as `mkdbg` on your PATH
```

Or one-liner:

```bash
curl -fsSL https://raw.githubusercontent.com/JialongWang1201/mkdbg/main/scripts/install.sh | sh
```

## Firmware agents

mkdbg works with any MCU. You implement two small UART hooks:

```c
// wire: called from your HardFault handler
void wire_on_fault(void);           // ~5 lines in your fault handler

// seam: called when the host requests a capture
void seam_uart_flush(uint8_t *buf, size_t len);  // ~10 lines, your UART HAL
```

See [`docs/PORTING.md`](docs/PORTING.md) for the full porting guide.

The STM32F446RE reference implementation is at
[`examples/stm32f446/`](examples/stm32f446/). Use it as a template.

## Typical session

```bash
# attach: zero-dependency crash report over UART
mkdbg attach --port /dev/ttyUSB0 --arch cortex-m

# seam: post-mortem causal chain from a captured event ring
mkdbg seam analyze capture.cfl

# dashboard: real-time terminal UI (probe + build + git)
mkdbg dashboard

# GDB bridge: full interactive session without a JTAG probe
wire-host --port /dev/ttyUSB0 --baud 115200
arm-none-eabi-gdb firmware.elf -ex "target remote :3333"
```

## Repository layout

```
mkdbg/
├── host/          CLI source (mkdbg-native, seam-analyze entry points)
├── deps/
│   ├── seam/      seam causal analysis agent (submodule)
│   ├── wire/      wire GDB RSP agent (submodule)
│   └── libgit2/   local git ops without requiring git CLI (submodule)
├── scripts/       install.sh, ci_smoke.sh
├── examples/
│   └── stm32f446/ STM32F446RE reference firmware (FreeRTOS + MPU)
└── docs/          PORTING.md, CLI reference, developer guide
```

## Dependencies

Host build: `cmake >= 3.20`, a C11 compiler (`gcc` or `clang`).
No other runtime dependencies. libgit2 is vendored as a submodule.

Firmware: any C99 cross-compiler for your target MCU.

## License

MIT — see [LICENSE](LICENSE).

The `deps/seam` and `deps/wire` submodules are also MIT.
`deps/libgit2` is MIT. CMSIS headers in `examples/stm32f446/cmsis/` are Apache-2.0.
FreeRTOS in `examples/stm32f446/freertos/` is MIT.
