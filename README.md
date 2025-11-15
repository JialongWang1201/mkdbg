# MicroKernel-MPU

```text
+----------------------------------------------------------------------------------+
|  __  __ _                  _  __                    _   __  __ ____  _   _       |
| |  \/  (_) ___ _ __ ___   | |/ /___ _ __ _ __   ___| | |  \/  |  _ \| | | |      |
| | |\/| | |/ __| '__/ _ \  | ' // _ \ '__| '_ \ / _ \ | | |\/| | |_) | | | |      |
| | |  | | | (__| | | (_) | | . \  __/ |  | | | |  __/ | | |  | |  __/| |_| |      |
| |_|  |_|_|\___|_|  \___/  |_|\_\___|_|  |_| |_|\___|_| |_|  |_|_|    \___/       |
|                      hardware-first bringup and fault triage                      |
+----------------------------------------------------------------------------------+
```

MicroKernel-MPU is a fault-containment-first STM32F446 platform built on
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
- host tooling for `mkdbg`, terminal dashboard, triage bundles, and HIL gates

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
MicroKernel-MPU boot
Build id=0x1A2B3C4D git=1a2b3c4d clean profile=debug board=Nucleo-F446RE uart=USART2
```

## Debugging

Install `mkdbg` from a local checkout:

```bash
bash tools/install_mkdbg.sh
```

Native foundation preview: `bash tools/build_mkdbg_native.sh && build/mkdbg-native --version`

Or install it remotely:

```bash
curl -fsSL https://raw.githubusercontent.com/JialongWang1201/MicroKernel-MPU/main/tools/install_mkdbg.sh | sh
```

Typical flow:

```bash
mkdbg init --name microkernel --port /dev/cu.usbmodemXXXX
mkdbg doctor
mkdbg build
mkdbg flash
mkdbg probe halt
mkdbg incident open --name irq-timeout
mkdbg incident status
mkdbg capture bundle --port /dev/cu.usbmodemXXXX
mkdbg attach --break main --command continue --command bt --batch
mkdbg snapshot --port /dev/cu.usbmodemXXXX
mkdbg watch --target microkernel
```

Repo-local tools remain available when you are working inside this checkout:

```bash
tools/vm32 bringup-ui --bundle-json tests/fixtures/triage/sample_bundle.json --render-once
tools/vm32 triage-bundle --port /dev/cu.usbmodemXXXX
tools/vm32 triage-replay build --bundle-json tests/fixtures/triage/sample_bundle.json --output build/sample.replay.json
bash tools/hil_gate.sh --port /dev/cu.usbmodemXXXX
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
