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

MicroKernel-MPU is a fault-containment-first STM32F446 platform built on FreeRTOS MPU for low-level bringup, driver isolation, VM workloads, and evidence-first fault triage.

It combines staged bringup, KDI driver domains, unified fault telemetry, a VM32 runtime, host tooling, and hardware regression gates so a stuck boot or runtime failure can be explained quickly.

## What You Get

- staged bringup with rerun, rollback, wait analysis, and dependency what-if
- KDI-style driver lifecycle and fault containment modeling
- snapshot + event-slice RCA with feature vectors and evidence event IDs
- VM32 runtime with bounded execution and policy control
- declarative bringup manifest compiler (`configs/bringup/manifest.yaml`)
- host tooling for triage bundles, terminal UI, profile compare, and HIL gates

## Quick Start

### Prerequisites

- `arm-none-eabi-gcc`
- `cmake`
- `openocd`
- `python3`
- `pyserial` for serial tools

```bash
pip3 install pyserial
```

### Build and Flash

```bash
bash tools/build.sh
bash tools/flash.sh
```

Optional build knobs:

```bash
VM32_MEM_SIZE=256 bash tools/build.sh
BOARD_UART_PORT=2 bash tools/build.sh
```

### First Board Commands

```text
disable
bringup check
bringup stage show
bringup stage wait
snapshot
fault last
```

### First Triage Loop

| Task | Command |
|---|---|
| See current boot stage | `bringup stage show` |
| Explain why boot is stuck | `bringup stage wait` |
| Capture the latest failure slice | `snapshot` |
| Estimate impact before touching a driver | `dep whatif <reset|throttle|deny> <driver>` |
| Review capability slack | `kdi cap review [driver]` |
| Open the terminal dashboard | `tools/vm32 bringup-ui --port /dev/cu.usbmodemXXXX` |

## System Layout

This is the fastest way to understand the repo at a glance.

### 1. Control Plane

```mermaid
flowchart LR
    Manifest["Manifest Source<br/>configs/bringup/manifest.yaml"] --> Compiler["Compiler / Generators<br/>tools/bringup_compile.py"]
    Compiler --> Host["Host Tooling<br/>tools/vm32 / triage_bundle.py"]
    Host --> CI["CI / HIL Gates"]
    Host --> Target["STM32F446 + FreeRTOS MPU"]
```

### 2. Target Runtime

```mermaid
flowchart TB
    CLI["CLI / Shell<br/>bringup.* fault.* dep.* snapshot vm.* sonic.*"] --> Bringup["Bringup Engine<br/>bringup_phase.c + dependency_graph.c"]
    Bringup --> Telemetry["Telemetry + Snapshot<br/>semantic telemetry / fault slices / evidence IDs"]
    Telemetry --> Analysis["Analysis Engine<br/>rule engine + model adapter seam"]

    subgraph Runtime["Inside the Target Runtime"]
        KDI["KDI Driver Domains<br/>lifecycle / restart / containment"]
        VM["VM32 Runtime<br/>bounded execution / policy monitor / enforce"]
        HW["Kernel / BSP / Hardware<br/>FreeRTOS MPU / UART / sensor / IRQ / DMA"]
    end

    Bringup --> KDI
    Telemetry --> VM
    Analysis --> HW
    KDI --> HW
    VM --> HW
```

### 3. Observation Flow

```mermaid
flowchart LR
    Source["UART / snapshot logs / triage bundle"] --> Tools["bringup_ui.py / triage_bundle.py / profile_compare.py"]
    Tools --> Decision["human triage / CI decision"]
```

<details>
<summary>Open the original system layout SVG</summary>

![MicroKernel-MPU system layout](docs/assets/system-layout.svg)

</details>

<details>
<summary>ASCII architecture snapshot</summary>

```text
+------------------------------ Host / CI ------------------------------+
| tools/vm32 wrapper | build/flash scripts | board regressions (serial) |
+-------------------------------+--------------------------------------+
                                |
                                v UART/OpenOCD
+-------------------------- Target: STM32F446 --------------------------+
| CLI / shell commands                                                 |
|   bringup.*   fault.*   vm.*   sonic.*   kdi.*                       |
+-------------------------------+--------------------------------------+
                                |
         +----------------------+----------------------+
         |                                             |
         v                                             v
+---------------------------+             +----------------------------+
| Bring-up Phase Model      |             | Fault Pipeline             |
| ROM -> MPU -> kernel      |             | CPU faults + VM faults     |
| -> driver subphases       |             | -> normalized records      |
| -> services -> workload   |             | -> CLI/JSON visibility     |
+-------------+-------------+             +-------------+--------------+
              |                                           ^
              v                                           |
+-------------+-------------------+          +------------+------------+
| KDI Driver / Fault Domains      |          | VM32 Runtime + Scenarios |
| per-driver lifecycle, restart,  |<---------| bounded exec, MIG policy |
| containment (driver != kernel)  |  I/O     | workloads / stress flows |
+-------------+-------------------+          +------------+------------+
              |                                           |
              +--------------------+----------------------+
                                   v
                        FreeRTOS MPU + BSP / hardware
```

</details>

## Terminal Dashboard

The terminal dashboard is the fastest operator surface for stuck-stage triage, fault slices, and dependency what-if.

![Bringup UI dashboard](docs/assets/bringup-ui-dashboard.svg)

Try it locally:

```bash
tools/vm32 bringup-ui --bundle-json tests/fixtures/triage/sample_bundle.json
tools/vm32 bringup-ui --bundle-json tests/fixtures/triage/sample_bundle.json --render-once --width 120 --height 38
```

Run it live against a board:

```bash
tools/vm32 bringup-ui --port /dev/cu.usbmodemXXXX --auto-refresh-s 5
```

Useful keys:

- `g` refresh one triage bundle
- `a` toggle auto refresh
- `+` / `-` change auto refresh interval
- `1` / `2` / `3` switch reset/throttle/deny view
- `[` / `]` cycle fault slices
- `q` quit

## Host Tooling

Use `tools/vm32` as the main host entrypoint.

```bash
tools/vm32 bringup-ui --bundle-json tests/fixtures/triage/sample_bundle.json --render-once
tools/vm32 triage-bundle --port /dev/cu.usbmodemXXXX
tools/vm32 triage-bundle --source-log tests/fixtures/triage/sample_snapshot.log --output build/sample.bundle.json
tools/vm32 triage-replay build --bundle-json tests/fixtures/triage/sample_bundle.json --output build/sample.replay.json
tools/vm32 triage-replay diff --baseline build/sample.replay.json --candidate build/sample.replay.json --fail-on-diff
tools/vm32 profile-compare --baseline logs/profile_a.log --candidate logs/profile_b.log --driver uart
bash tools/hil_gate.sh --port /dev/cu.usbmodemXXXX
python3 tools/regression_summary.py --output build/regression_summary.json
```

Hardware gate default pipeline:

```text
build -> flash -> bringup_regress -> kdi_driver_regress -> kdi_irq_regress
```

For full flags:

```bash
tools/vm32 <subcommand> --help
python3 tools/<script>.py --help
```

## Repo Map

- `src/` firmware runtime, CLI handlers, bringup/fault/KDI/VM32 logic
- `include/` public headers and subsystem interfaces
- `bsp/` board support and MPU demo hooks
- `scenarios/` VM scenario assets
- `tests/` host tests and regression fixtures
- `tools/` build/flash scripts, serial tools, dashboards, regressions
- `docs/` developer and architecture docs

## More Docs

README is now the entry page. The detailed engineering docs live here:

- `docs/DEVELOPER_GUIDE.md` for maintainer workflow and deeper operations
- `docs/generated/bringup_manifest.md` for the generated bringup phase/stage view
- `docs/PLATFORM_NARRATIVE.md` for naming and architecture narrative
- `docs/vm32_design.md` for the VM ISA/runtime design
- `docs/vm32_errors.md` for the VM error model
- `docs/vm32_debug.md` for VM debugging notes

<details>
<summary>Collapsed command families</summary>

### Runtime / bringup

- `bringup [check|json|mpu]`
- `bringup phase show|json|reset|run`
- `bringup phase rerun <rom|mpu|kernel|driver|diag|uart|sensor|vm|service|user>`
- `bringup phase rollback <phase>`
- `bringup phase inject <phase> [code]`
- `bringup phase clearfail <phase|all>`
- `bringup stage show|json|wait|wait-json`
- `snapshot`
- `fault last`
- `fault dump`
- `dep show|json`
- `dep impact <kernel|uart|sensor|vm|diag>`
- `dep whatif <reset|throttle|deny> <kernel|uart|sensor|vm|diag>`
- `kdi cap show|json|review|review-json [driver]`
- `profile [json|reset]`

### VM32 / policy / MicroSONiC-Lite

- `vm scenario list`
- `vm scenario <name>`
- `vm verify <addr> [span]`
- `vm runb <addr> [span]`
- `vm mig status`
- `vm mig mode <off|monitor|enforce>`
- `vm mig allow <uart_tx|uart_rx|led|ic|all>`
- `vm mig deny <uart_tx|uart_rx|led|ic|all>`
- `vm mig apply <json>`
- `sonic cap`
- `sonic show [db|running|candidate|config|appl|asic]`
- `sonic preset list|show|apply <name> [running|candidate]`
- `sonic commit [rollback_ms]`
- `sonic confirm`
- `sonic rollback [now]`
- `sonic abort`

### Board / regression entry points

```bash
tools/vm32 board-regress   --port /dev/cu.usbmodem21303
tools/vm32 sonic-regress   --port /dev/cu.usbmodem21303
tools/vm32 kdi-regress     --port /dev/cu.usbmodem21303
tools/vm32 irq-regress     --port /dev/cu.usbmodem21303
tools/vm32 bringup-regress --port /dev/cu.usbmodem21303
tools/vm32 profile-regress --port /dev/cu.usbmodem21303
tools/vm32 triage-bundle   --port /dev/cu.usbmodem21303
```

</details>

<details>
<summary>Useful local checks</summary>

```bash
./tools/vm32_host_tests.sh
./tools/kdi_host_tests.sh
./tools/sonic_lite_host_tests.sh
./tools/bringup_host_tests.sh
./tools/dependency_graph_host_tests.sh
./tools/bringup_compile_host_tests.sh
./tools/bringup_ui_host_tests.sh
./tools/analysis_engine_host_tests.sh
./tools/profile_compare_host_regress.sh
./tools/triage_bundle_host_tests.sh
./tools/triage_replay_host_tests.sh
./tools/regression_summary_host_tests.sh
```

</details>
