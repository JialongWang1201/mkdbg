# Platform Narrative

MicroKernel-MPU is positioned as a **behavior lab for protected RTOS systems**.
The platform is intentionally built around three loops:

1. Generate behavior
2. Constrain execution
3. Explain failures

## 1) Generate Behavior

`VM32` is not treated as an "app runtime". It is the system behavior script engine.

Execution model:
- operator chooses a named scenario via `vm scenario <name>`
- runtime routes through `scenario_run(name)`
- scenario returns a normalized `ScenarioResult`

Primary assets:
- `src/scenario.c`
- `src/vm32_scenarios.c`
- `scenarios/*.asm`
- `scenarios/*.vm`

## 2) Constrain Execution

VM behavior is profiled by a bounded CFG contract before bounded execution:
- no `CALL/RET`
- no backward edges
- no out-of-window decode/targets

Firmware path:
- `vm verify <addr> [span]`
- `vm runb <addr> [span]`

Host path:
- `tools/vm32 verify <bin>`
- `tools/vm32 batch <file>`
- `tools/vm32 --json ...` for automation

## 3) Explain Failures

All CPU and VM failures are normalized into one `FaultRecord` pipeline:
- unified ingress: `fault_dispatch()`
- consistent snapshot fields (CPU + task + MPU + VM context)
- dual output: human-readable + JSON (UART)

Operator commands:
- `fault last`
- `fault dump`

## Naming Baseline

Use these names consistently in code, docs, and commit messages:
- `Scenario`: named system behavior workload
- `ScenarioResult`: normalized scenario completion contract
- `bounded profile`: VM CFG-safe subset for predictable step bounds
- `FaultRecord`: normalized CPU/VM fault snapshot
- `fault pipeline`: the end-to-end dispatch, retention, and emission flow

Avoid ambiguous aliases:
- "test case" when you mean runtime scenario
- "panic blob" when you mean `FaultRecord`
- "vm app" when you mean behavior script

## Acceptance Signals

Direction 1 + Direction 2 + Direction 4 are considered integrated when:
- `vm scenario <name>` runs all catalog entries through `scenario_run()`
- each scenario reports a `ScenarioResult`
- VM and CPU faults are visible via `fault last` / `fault dump`
- host automation can run verify/batch in JSON mode without fragile parsing
