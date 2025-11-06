# Developer Guide

Internal engineering handbook for maintainers and contributors.
For external onboarding, read `README.md` first.
Platform-level narrative and naming policy live in `docs/PLATFORM_NARRATIVE.md`.

## Table of Contents

- [1. Architecture](#1-architecture)
- [2. Operations](#2-operations)
- [3. Troubleshooting](#3-troubleshooting)
- [4. Contribution Guide](#4-contribution-guide)
- [5. Reference Map](#5-reference-map)

## 1. Architecture

### 1.1 System intent

MicroKernel-MPU is a reference platform for validating protected RTOS behavior under scripted load and fault conditions.

Primary goals:
- deterministic behavior generation via VM scenarios
- unified, explainable fault snapshots
- machine-parseable UART fault output

Non-goals:
- general application framework
- high-performance VM runtime

### 1.2 Runtime layers

1. BSP (`bsp/`): clock, GPIO, UART, ADC
2. RTOS + MPU (`freertos/`): privileged scheduler/kernel + unprivileged task sandbox
3. VM32 runtime (`src/vm32.c`): dual-stack interpreter + MMIO
4. Scenario engine (`src/scenario.c`, `src/vm32_scenarios.c`): named workload catalog
5. Fault pipeline (`src/fault.c`, `src/interrupts.c`): normalized `FaultRecord` + emitters
6. CLI (`src/main.c`): operator control plane

### 1.3 Isolation model

- Kernel tasks execute privileged.
- `mpu_user_task` executes unprivileged with explicit MPU regions.
- `mpu_overflow` intentionally violates RO protection to validate MemManage handling.

### 1.4 Scenario system (Direction 1)

Entry points:
- CLI: `vm scenario <name>`
- Runtime: `scenario_run(name)`

Scenario contract:
- catalog source: `src/vm32_scenarios.c`
- assets location: `scenarios/`
- formats: `.asm` and `.vm`
- output type: `ScenarioResult` (`status`, `vm_result`, `steps`, `vm_fault`)
- bounded profile: scenarios with VM bytecode are validated by `vm32_verify_bounded_cfg`
  before execution

Built-in scenarios:
- `mpu_overflow`
- `irq_starvation`
- `io_flood`

### 1.5 Fault pipeline (Direction 2)

All fault sources converge into `fault_dispatch()`.

CPU path:
- `HardFault_Handler` / `UsageFault_Handler` / `MemManage_Handler`
- bridge in `src/interrupts.c`
- snapshot built by `fault_report_cpu()`

VM path:
- VM runtime errors (`VM32_ERR_STACK`, `VM32_ERR_MEM`, `VM32_ERR_POLICY`)
- snapshot built by `fault_report_vm()`

Snapshot model (`FaultRecord`) includes:
- CPU context: `pc/lr/sp/xpsr/control/exc_return`
- fault status: `cfsr/hfsr/mmfar/bfar`
- isolation context: privilege + matched `mpu_region/rbar/rasr`
- VM context: `vm_err/vm_pc/vm_op/vm_ic/vm_dtop/vm_rtop/vm_last_out`

Output modes:
- human-readable line (interactive debugging)
- JSON line (host tooling)

CLI semantics:
- `fault last`: latest record (JSON)
- `fault dump`: history dump (human + JSON)
- history retention: latest 8 records, oldest to newest

### 1.6 Design rules

- Any new crash/fault path must flow through `fault_dispatch()`.
- Prefer scenario catalog updates over ad-hoc CLI branches for new workloads.
- Preserve privilege boundaries by default.
- Keep JSON output stable; add fields additively.
- Bounded profile constraints: no `CALL/RET`, no backward CFG edges, no out-of-window targets.

### 1.7 Naming baseline

Use canonical names across code, docs, and commit messages:
- `Scenario` for named runtime workloads
- `ScenarioResult` for scenario output/status contract
- `bounded profile` for CFG-safe VM execution subset
- `FaultRecord` for normalized CPU/VM fault snapshots

## 2. Operations

### 2.1 Toolchain prerequisites

- `arm-none-eabi-gcc`
- `cmake`
- `openocd`

Optional host utilities:
- `python3`
- `pyserial`

### 2.2 Build and flash

Build:
```bash
bash tools/build.sh
```

Flash:
```bash
bash tools/flash.sh
```

Common options:
```bash
VM32_MEM_SIZE=256 bash tools/build.sh
BOARD_UART_PORT=2 bash tools/build.sh
```

`mkdbg` wraps these same primitives behind one repo-aware CLI:

```bash
bash tools/install_mkdbg.sh
mkdbg init --name microkernel --port /dev/cu.usbmodemXXXX
mkdbg build
mkdbg flash
mkdbg attach
mkdbg hil --port /dev/cu.usbmodemXXXX
```

UART mapping:
- `1`: USART1 (PA9/PA10)
- `2`: USART2 (PA2/PA3, default)
- `3`: USART3 (PB10/PB11)

### 2.3 Core operator commands

VM:
- `vm reset`
- `vm load <addr> <b0> <b1> ...`
- `vm run <addr> [max]`
- `vm verify <addr> [span]`
- `vm runb <addr> [span]`
- `vm step [n]`
- `vm dump <addr> <len>`
- `vm patch <addr> <byte>`
- `vm mig status|mode|allow|deny|reset`
- `vm mig apply <json>` (compact JSON subset)

Scenario:
- `vm scenario list`
- `vm scenario <name>`

Fault:
- `fault last`
- `fault dump`

Bring-up:
- `bringup` / `bringup check`
- `bringup json`
- `bringup mpu`

MicroSONiC-Lite:
- `sonic cap`
- `sonic show [db|running|candidate|config|appl|asic]`
- `sonic get [running|candidate] <config|appl|asic> <key>`
- `sonic set <config|appl|asic> <key> <value>`
- `sonic diff [config|appl|asic]`
- `sonic history [n]`
- `sonic preset list|show|apply <name> [running|candidate]`
- `sonic apply mig [running|candidate]`
- `sonic apply vm [running|candidate]`
- `sonic commit [rollback_ms]`
- `sonic confirm`
- `sonic rollback [now]`
- `sonic abort`

### 2.4 Baseline regression

Run in this order:

```text
disable
vm scenario list
vm scenario io_flood
fault last
vm scenario irq_starvation
fault last
vm reset
vm load 0 3
vm run 0 1
fault last
fault dump
vm scenario mpu_overflow
```

Expected outcomes:
- `io_flood`: scenario result should be halted and no recorded fault
- `irq_starvation`: scenario result should be halted and no recorded fault
- `vm run 0 1` (with `0x03` at PC): VM fault emitted and captured
- `fault dump`: includes history records in chronological order
- `mpu_overflow`: CPU MemManage JSON emitted, then handler halt

Operational note:
- Execute `mpu_overflow` last in a test batch.

### 2.5 Host tooling

Assembler:
```bash
tools/vm32 asm demo.asm -o demo.bin
tools/vm32 asm demo.asm --load --base 0
```

Tiny C:
```bash
tools/vm32 tinyc demo/hello.c -o demo/hello.asm
```

Loader:
```bash
tools/vm32 load demo.bin --dry-run
tools/vm32 load demo.bin --port /dev/tty.usbmodemXXXX
```

Disassembler:
```bash
tools/vm32 disasm demo.bin --base 0
```

Verifier:
```bash
tools/vm32 verify demo/hello.bin
tools/vm32 --json verify demo/hello.bin
```

Batch execution:
```bash
tools/vm32 batch demo/vm32_smoke.batch
tools/vm32 --json batch demo/vm32_smoke.batch --keep-going
```

Batch grammar:
- one VM32 subcommand per line (no `tools/vm32` prefix)
- comments start with `#` or `;`
- `--keep-going` continues execution after failures

Exit code conventions:
- `0`: success
- `1`: semantic verification reject
- `2`: tooling/input failures

Smoke tests:
```bash
bash tools/vm32_ci.sh
bash tools/vm32_host_tests.sh
tools/vm32 sonic-regress --port /dev/cu.usbmodem21303
```

### 2.6 Declarative bring-up manifest

Bring-up stage names, phase aliases, stage-driver edges, and driver-resource edges are declared in:

- `configs/bringup/manifest.yaml`

Generate the committed runtime/doc artifacts with:

```bash
python3 tools/bringup_compile.py
```

Generated outputs:

- `include/bringup_manifest_gen.h`
- `docs/generated/bringup_manifest.md`

Validation:

```bash
bash tools/bringup_compile_host_tests.sh
```

### 2.7 Terminal dashboard

For a host-side operator console that borrows the split-pane terminal style, use:

```bash
tools/vm32 bringup-ui --port /dev/cu.usbmodemXXXX
tools/vm32 bringup-ui --port /dev/cu.usbmodemXXXX --auto-refresh-s 5
```

Offline render/test paths:

```bash
tools/vm32 bringup-ui --bundle-json tests/fixtures/triage/sample_bundle.json --render-once
bash tools/bringup_ui_host_tests.sh
```

## 3. Troubleshooting

### 3.1 CLI does not respond

Checks:
1. Ensure board is flashed and UART port is correct.
2. Send commands with slower character pacing if using custom serial scripts.
3. Confirm no terminal process is holding the same serial device.

### 3.2 `fault dump` shows no data

Checks:
1. Trigger a known VM fault probe: `vm reset`, `vm load 0 3`, `vm run 0 1`.
2. Run `fault last` to validate a latest record exists.
3. Run `fault dump` again.

### 3.3 `mpu_overflow` does not print expected CPU fault

Checks:
1. Verify scenario dispatch prints `Scenario: mpu_overflow` from user task.
2. Verify MPU is not disabled at build/runtime.
3. Ensure scenario runs last; CPU fault handler halts after emission.

### 3.4 Build succeeds but runtime regresses

Checks:
1. Compare latest commits touching `src/fault.c`, `src/main.c`, `bsp/mpu_demo.c`.
2. Re-run baseline regression sequence end-to-end.
3. Validate JSON schema changes did not break host parser assumptions.

## 4. Contribution Guide

### 4.1 Change policy

- Keep changes minimal and composable.
- One logical change per commit.
- Prefer additive contracts for CLI/JSON changes.

### 4.2 Commit quality bar

Each change should include:
1. clear rationale
2. implementation scoped to affected module
3. verification evidence (build + focused runtime checks)

Recommended commit style:
- `subsystem: intent`
- examples: `fault: add history ring`, `scenario: add workload metadata`

### 4.3 Fault and scenario extensions

When adding a new scenario:
1. add catalog entry in `src/vm32_scenarios.c`
2. keep execution via `scenario_run()`
3. define expected `ScenarioResult` behavior

When adding a new fault source:
1. map source into `FaultRecord`
2. route through `fault_dispatch()`
3. ensure `fault last` and `fault dump` visibility

### 4.4 Compatibility rules

- Do not remove existing JSON keys without migration notes.
- Keep CLI command grammar backward-compatible when possible.
- Maintain chronological ordering in fault history dump output.

## 5. Reference Map

Core files:
- `src/main.c`: bootstrap, task creation, CLI
- `src/vm32.c`: VM32 interpreter
- `src/scenario.c`: scenario execution pipeline
- `src/vm32_scenarios.c`: scenario catalog
- `src/fault.c`: fault normalization/history/output
- `src/interrupts.c`: CPU fault bridge
- `bsp/mpu_demo.c`: unprivileged behavior + MPU probes

Supporting docs:
- `README.md`: external quick-start
- `docs/vm32_design.md`: VM32 ISA/design
- `docs/vm32_errors.md`: VM error semantics
- `docs/vm32_debug.md`: VM debug and breakpoints
