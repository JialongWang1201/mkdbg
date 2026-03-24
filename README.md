# mkdbg

```text
+----------------------------------------------------------+
|                                                          |
|  _ __ ___ | | _____| | |__   __ _                        |
| | '_ ` _ \| |/ / _` | '_ \ / _` |                       |
| | | | | | |   < (_| | |_) | (_| |                        |
| |_| |_| |_|_|\_\__,_|_.__/ \__, |                        |
|                              |___/                        |
|         hardware-first bringup and fault triage           |
+----------------------------------------------------------+
```

mkdbg is a fault-containment-first STM32F446 platform built on
FreeRTOS MPU. It combines staged bring-up, KDI-style driver isolation, VM32
workloads, unified fault telemetry, and repo-aware host tooling so boot and
runtime failures can be explained quickly.

Please read [docs/DEVELOPER_GUIDE.md](docs/DEVELOPER_GUIDE.md) for the main
developer workflow and [docs/MKDBG.md](docs/MKDBG.md) for the supported debug
CLI.

## What It Provides

- staged bring-up with rerun, rollback, wait analysis, and dependency what-if
- KDI driver lifecycle and containment modeling
- snapshot and event-slice RCA with evidence IDs and replayable host artifacts
- VM32 runtime with bounded execution and policy control
- **seam** causal fault analysis: on-device event ring emitted over UART as a COBS-framed `.cfl` bundle; `mkdbg seam analyze` reconstructs the causal chain from the binary dump
- **wire** crash diagnostics over UART: on crash the firmware halts and exposes state over UART — `mkdbg attach --port` reads registers, CFSR, and a heuristic backtrace in under a second with no GDB, no JTAG, no OpenOCD (wire crash diagnostics are built directly into `mkdbg-native`; `wire-host` is only needed for full GDB bridge sessions)
- host tooling for `mkdbg`, terminal dashboard, triage bundles, and HIL gates

## Support Repos

Two focused C99 libraries ship as git submodules at `tools/`:

```
+-----------------------------------------------------+
|                       mkdbg                          |
|   fault.c  kdi.c  vm32.c  FreeRTOS  bsp/            |
|                                                      |
|   tools/seam/          tools/wire/                   |
|   seam_agent.h         wire.h                        |
|   (event ring)         (GDB RSP stub)                |
+----------+------------------+------------------------+
           |  .cfl bundle     |  raw RSP over UART
           v                  v
    seam-analyze           mkdbg-native (wire embedded)
    causal chain        attach: crash report (in-process)
    (post-mortem)       wire-host: TCP<->UART bridge (GDB)
```

| Repo | Role | When to use |
|------|------|-------------|
| [seam](https://github.com/JialongWang1201/seam) | Post-mortem causal chain reconstruction from the fault event ring | After a crash: `mkdbg seam analyze capture.cfl` |
| [wire](https://github.com/JialongWang1201/wire) | Crash diagnostics over UART — zero-dependency crash dump or full GDB bridge | Quick read: `mkdbg attach --port /dev/ttyUSB0` · GDB bridge: `wire-host --port /dev/ttyUSB0` |

Both are zero-dependency C99 libraries. Add both at once:

```bash
git submodule update --init --recursive
```

## Building

Prerequisites:

- `arm-none-eabi-gcc`
- `cmake`
- `openocd`
- `python3`
- `pyserial`

```bash
pip3 install pyserial
bash tools/build.sh
bash tools/flash.sh
```

Common build knobs:

```bash
VM32_MEM_SIZE=256 bash tools/build.sh
BOARD_UART_PORT=2 bash tools/build.sh
BUILD_PROFILE=debug bash tools/build.sh
```

The boot banner emits firmware identity on UART so host tooling can confirm the
exact image running on the board:

```text
mkdbg boot
Build id=0x1A2B3C4D git=1a2b3c4d clean profile=debug board=Nucleo-F446RE uart=USART2
```

## Debugging

Install `mkdbg` from a local checkout:

```bash
bash tools/install_mkdbg.sh
```

This installs the native C frontend as `mkdbg`. Preview build:

```bash
cmake -S . -B build_host && cmake --build build_host --target mkdbg_native_host
./build_host/mkdbg-native --version
```

Or install it remotely:

```bash
curl -fsSL https://raw.githubusercontent.com/JialongWang1201/mkdbg/main/tools/install_mkdbg.sh | sh
```

Remote native install currently builds from source and requires `curl` and `cc`.
If `MKDBG_INSTALL_BINARY_URL` is set, the installer downloads a prebuilt native
binary instead and skips the compiler requirement. If
`MKDBG_INSTALL_BINARY_BASE_URL` is set, the installer auto-selects
`mkdbg-native-<os>-<arch>` from that location.

Typical flow:

```bash
mkdbg init --name mkdbg --port /dev/cu.usbmodemXXXX
mkdbg doctor
mkdbg build
mkdbg flash
mkdbg probe halt
mkdbg incident open --name irq-timeout
mkdbg incident status
mkdbg capture bundle --port /dev/cu.usbmodemXXXX
mkdbg seam analyze /path/to/capture.cfl
mkdbg attach --port /dev/cu.usbmodemXXXX          # zero-dependency crash report (no GDB)
mkdbg attach --break main --command continue --command bt --batch  # GDB session
mkdbg snapshot --port /dev/cu.usbmodemXXXX
mkdbg watch --target mkdbg
```

Repo-local tools remain available when you are working inside this checkout:

```bash
tools/vm32 bringup-ui --bundle-json tests/fixtures/triage/sample_bundle.json --render-once
tools/vm32 triage-bundle --port /dev/cu.usbmodemXXXX
tools/vm32 triage-replay build --bundle-json tests/fixtures/triage/sample_bundle.json --output build/sample.replay.json
bash tools/hil_gate.sh --port /dev/cu.usbmodemXXXX
```

## Crash Diagnostics (wire)

When the firmware crashes, `wire` halts the CPU and exposes the fault state over
the same UART used by `mkdbg`. No JTAG probe or OpenOCD required.

Build the host tools:

```bash
cmake -S . -B build_host && cmake --build build_host
# -> build_host/mkdbg-native, build_host/wire-host
```

`mkdbg-native` embeds the wire crash diagnostics library directly.
`wire-host` is still built but only needed for full GDB bridge sessions.

### Zero-dependency crash readout

Read crash state without GDB — outputs registers, CFSR, and a heuristic backtrace
in under a second. No `wire-host` subprocess required:

```bash
# Read crash report, print human-readable summary
mkdbg attach --port /dev/cu.usbmodemXXXX --baud 115200
```

The dashboard (`mkdbg dashboard`) polls this automatically and shows crash state
in the PROBE panel in real time.

### Live GDB session

For full interactive debugging, `wire-host` bridges UART to TCP so GDB can
connect without OpenOCD:

```bash
# Terminal 1 — bridge UART to TCP port 3333
wire-host --port /dev/cu.usbmodemXXXX --baud 115200

# Terminal 2 — connect GDB
arm-none-eabi-gdb build/mkdbg.elf
(gdb) target remote :3333
(gdb) bt          # call stack at fault site
(gdb) info reg    # register state
(gdb) x/16xw $sp  # stack contents
(gdb) continue    # resume execution
```

For QEMU:

```bash
qemu-system-arm -M mps2-an385 -kernel build/mkdbg.elf -serial pty
# QEMU prints: char device redirected to /dev/pts/3
wire-host --port /dev/pts/3 --baud 0
```

## Documentation

- [docs/DEVELOPER_GUIDE.md](docs/DEVELOPER_GUIDE.md): build, flash, operator, and maintainer workflow
- [docs/MKDBG.md](docs/MKDBG.md): supported repo-aware debug CLI
- [docs/OVWATCH.md](docs/OVWATCH.md): target-adapter debug workflow notes
- [docs/generated/bringup_manifest.md](docs/generated/bringup_manifest.md): generated phase and stage view
- [docs/PLATFORM_NARRATIVE.md](docs/PLATFORM_NARRATIVE.md): naming and system narrative
- [docs/vm32_design.md](docs/vm32_design.md): VM ISA and runtime design
- [docs/vm32_debug.md](docs/vm32_debug.md): VM debugging notes
- [docs/vm32_errors.md](docs/vm32_errors.md): VM error model

## Repository Layout

- `src/`: firmware runtime, CLI handlers, bring-up, fault, KDI, and VM32 logic
- `include/`: subsystem interfaces and generated headers
- `bsp/`: board support and MPU demo hooks
- `configs/bringup/`: declarative bring-up manifest source
- `scenarios/`: VM scenario assets
- `tests/`: host tests and regression fixtures
- `tools/`: build, flash, debug, dashboard, replay, and regression tooling
- `docs/`: developer, architecture, and generated reference docs

## Local Checks

Core smoke and host checks:

```bash
./tools/ci_smoke.sh
./tools/vm32_ci.sh
./tools/build_identity_host_tests.sh
./tools/mkdbg_host_tests.sh
./tools/ovwatch_host_tests.sh
./tools/bringup_ui_host_tests.sh
./tools/triage_bundle_host_tests.sh
./tools/triage_replay_host_tests.sh
./tools/regression_summary_host_tests.sh
```

Board and regression entry points:

```bash
tools/vm32 board-regress   --port /dev/cu.usbmodem21303
tools/vm32 kdi-regress     --port /dev/cu.usbmodem21303
tools/vm32 irq-regress     --port /dev/cu.usbmodem21303
tools/vm32 bringup-regress --port /dev/cu.usbmodem21303
tools/vm32 profile-regress --port /dev/cu.usbmodem21303
tools/vm32 sonic-regress   --port /dev/cu.usbmodem21303
```

## License

MIT — see [LICENSE](LICENSE).

FreeRTOS is licensed under MIT. The `tools/seam` and `tools/wire` submodules are
also MIT. CMSIS headers are Apache-2.0.
