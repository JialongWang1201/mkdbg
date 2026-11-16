# mkdbg

`mkdbg` is the supported public debug CLI for this repo. It is a repo-aware
embedded debug wrapper that sits on top of the existing build, flash, OpenOCD,
GDB, serial, and HIL tooling.

It is intentionally not a replacement for GDB. The goal is to make
common embedded flows feel like a single operator-facing CLI:

- initialize one local config
- register one or more repos or target aliases
- build / flash / attach / snapshot / run HIL
- capture one triage bundle for later replay and sharing
- persist one active incident session around captured artifacts
- keep repo-specific commands in configuration instead of shell history

## Install

Local checkout:

```bash
bash tools/install_mkdbg.sh
```

This installs the native C frontend as `mkdbg`.

One-line remote install:

```bash
curl -fsSL https://raw.githubusercontent.com/JialongWang1201/mkdbg/main/scripts/install.sh | bash
```

The remote installer currently builds the native frontend from source and
requires `curl` and `cc`. If `MKDBG_INSTALL_BINARY_URL` is set, it downloads a
prebuilt native binary instead. If `MKDBG_INSTALL_BINARY_BASE_URL` is set, it
downloads `mkdbg-native-<os>-<arch>` for the current host.

## Quick Start

Initialize the current repo as a `MicroKernel-MPU` target:

```bash
mkdbg init --name microkernel --port /dev/cu.usbmodem21303
```

Then use the common flows:

```bash
mkdbg doctor
mkdbg doctor --live --port /dev/cu.usbmodem21303
mkdbg build
mkdbg flash
mkdbg probe status --target microkernel
mkdbg probe halt
mkdbg probe flash
mkdbg incident open --name irq-timeout
mkdbg incident status
mkdbg incident export
mkdbg attach
mkdbg attach --port /dev/cu.usbmodem21303 --explain
mkdbg attach --break main --command continue --command bt --batch
mkdbg capture bundle --port /dev/cu.usbmodem21303
mkdbg capture bundle --source-log tests/fixtures/triage/sample_snapshot.log --output build/mkdbg.bundle.json
mkdbg snapshot --port /dev/cu.usbmodem21303
mkdbg hil --port /dev/cu.usbmodem21303
mkdbg watch --target microkernel
mkdbg watch --target microkernel --bundle-json tests/fixtures/triage/sample_bundle.json --render-once
mkdbg replay build/mkdbg.bundle.json
mkdbg replay build/mkdbg.bundle.json --timeline
mkdbg replay build/mkdbg.bundle.json --timeline --event 2
mkdbg replay build/mkdbg.bundle.json --timeline --event 2 --context 2
mkdbg replay build/mkdbg.bundle.json --timeline --fault
mkdbg replay build/mkdbg.bundle.json --timeline --fault --context 2
mkdbg diff before.bundle.json after.bundle.json
```

Scripted attach flows can chain GDB actions without dropping into an
interactive prompt. `--break` adds `break <location>`, `--command`
adds one `-ex` command, and `--batch` exits after the commands finish:

```bash
mkdbg attach --break HardFault_Handler --command continue --command bt --batch
mkdbg attach --command "monitor reset halt" --command "info registers" --batch
```

## External Repos

Register another checkout with generic commands:

```bash
mkdbg repo add tahoe \
  --path ../TahoeOS \
  --build-cmd "make -j4" \
  --flash-cmd "./scripts/flash.sh" \
  --hil-cmd "./scripts/hil.sh --port {port}" \
  --attach-cmd "gdb build/tahoe.elf"
```

Run that repo explicitly:

```bash
mkdbg build tahoe
mkdbg hil tahoe --port /dev/cu.usbmodem21303
mkdbg run --repo tahoe -- make test
```

`target` is a user-facing alias for the same stored repo definitions:

```bash
mkdbg target add tahoe --path ../TahoeOS --build-cmd "make -j4"
mkdbg target list
mkdbg target use tahoe
mkdbg build --target tahoe
mkdbg run --target tahoe -- make test
```

### Board and SoC debug

Set `openocd_cfg` to the OpenOCD configuration for the board revision. Use
`mkdbg probe status --target <name> --dry-run` to check which file will be used.
The live command starts an OpenOCD session, polls examined targets, and prints
the `targets` table. An unsuccessful poll or zero polled targets gives a
nonzero exit status. Use a board configuration that leaves `init` to mkdbg.
Use this command outside an active debug session. OpenOCD initialization runs
board and target hooks, and some target drivers clear hardware breakpoints.
Power rails, reset timing, and boot progress require separate measurements.

Keep one profile per board revision. The profile maps probe wiring, instrument
channels, edge thresholds, and expected delays from a common power-on trigger.
Feed voltage and logic instrument exports as CSV with columns
`time_ns,channel,value`. Each instrument's `offset_ns` maps its sample clock to
the capture clock, and `error_ns` bounds the alignment error. Use an ID unique to
that synchronized capture for `clock`.

```bash
python3 tools/board_evidence.py tests/fixtures/platform/board.json \
  --samples scope=tests/fixtures/platform/scope.csv \
  --samples logic=tests/fixtures/platform/logic.csv
```

A transition is bounded by the samples on both sides of the threshold. The
report retains uncertainty when that interval overlaps an expected timing
limit or the CPU observation window. Pass a same-clock CPU capture with
`--cpu-state` to compare the intervals. A boot marker confirms execution reached
the instrumented point. Confirm the first instruction with a reset-vector
breakpoint or execution trace specific to the target.

### SoC core and trace inspection

Maintain one `soc.json` per chip revision. Name the power and reset domains,
map each core to its OpenOCD target and local GDB port, and declare bounded
physical address spaces and safe read ranges for status, error, and trace.
The simulated profile at `tests/fixtures/platform/soc.json` shows the schema.

Start the board's OpenOCD server with Tcl and GDB endpoints bound to loopback.
Use one operator per debug session and a board configuration with independent
halt/resume behavior for each core when using a declared halt group.

```bash
python3 tools/soc_debug.py board/soc.json capture
python3 tools/soc_debug.py board/soc.json capture --halt-group application-and-mcu
python3 tools/soc_debug.py board/soc.json attach mcu-0 --elf build/firmware.elf
```

The capture records initial run states and trace/error words. A halt group
issues all halt requests before waiting and restores previously running cores
when the command exits, including after a failed capture. If a transport or
restore error occurs, verify target states through the existing session.
The halt sequence has an observation window and can have inter-core skew.
Use `--clock` with the same calibrated capture identifier as the board profile
to compare that window to instrument events.

Only configure memory-mapped ranges the target permits reading. A register
with a read side effect needs target-specific handling. Interpret transaction
IDs, latch semantics, reset persistence, and trace records using the chip
revision's register map. Before tape-out, record those semantics for the
fabric errors, power-domain state, boot milestones, and trace buffers needed
for bring-up.

[OpenOCD Tcl RPC documentation](https://openocd.org/doc-release/html/Tcl-Scripting-API.html)

### Firmware identity after reset

On the STM32F446 example, send `fault evidence json` through the UART console
after reboot and save the reply. It includes the reset flags and, for a retained
CPU fault, the faulting boot, PC, LR, and GNU build ID of the faulting image.

```bash
python3 tools/firmware_evidence.py fault-uart.log --elf build/MicroKernel_MPU.elf
```

The ID comes from the loaded ELF note. The verifier compares the full ID
before resolving PC and LR. The previous boot also reports the last FreeRTOS
task switch and the exception number of any instrumented ISR active at reset.
The reference instruments SysTick. For another interrupt, pair
`fault_note_isr_enter()` with `fault_note_isr_exit(previous)` in its handler.
A zero exception number means no instrumented ISR was active. Task and cycle
values describe the last recorded execution, with cycle counts modulo 2^32.

The optional `--with-board` HIL checks exercise `reset cause json` and
`fault retained json`. For the three reset scenarios, build the reference
firmware with `-DMKDBG_FAULT_HIL=ON` and use a dedicated test board:

| Scenario | Trigger on the board | Expected record |
| --- | --- | --- |
| HardFault | Send `fault hil hardfault` | Preceding boot's HardFault frame with exact image ID |
| Deadlock | Send `fault hil deadlock` | Independent watchdog flag and previous-boot task context |
| Power loss | Remove target power and power on again | Power-on or brownout flag and invalidated SRAM evidence |

After each reboot, send `fault evidence json` and capture it into a separate log.
Add `--expect hardfault`, `--expect deadlock`, or `--expect power-loss` to check
the relevant record. HardFault and watchdog checks require the test ELF. For
power loss, omit `--elf` because the expected evidence is a fresh boot.

[GNU linker build ID options](https://www.sourceware.org/binutils/docs/ld/Options.html)

## Config

`mkdbg` searches upward from the current working directory for
`.mkdbg.toml`.

Example:

```toml
version = 1
default_repo = "microkernel"

[repos."microkernel"]
preset = "microkernel-mpu"
path = "."
port = "/dev/cu.usbmodem21303"
build_cmd = "bash tools/build.sh"
flash_cmd = "bash tools/flash.sh"
hil_cmd = "bash tools/hil_gate.sh --port {port}"
snapshot_cmd = "python3 tools/triage_bundle.py --port {port} --output {snapshot_output}"
elf_path = "build/MicroKernel_MPU.elf"
snapshot_output = "build/mkdbg.bundle.json"
openocd_cfg = "tools/openocd.cfg"
gdb = "arm-none-eabi-gdb"
gdb_target = "localhost:3333"
```

Supported template fields:

- `{repo}`
- `{repo_root}`
- `{port}`
- `{elf_path}`
- `{openocd_cfg}`
- `{snapshot_output}`
- `{gdb_target}`

## Scope

Current MVP supports:

- `mkdbg --version`
- `mkdbg init`
- `mkdbg doctor`
- `mkdbg repo add`
- `mkdbg repo list`
- `mkdbg repo use`
- `mkdbg target add`
- `mkdbg target list`
- `mkdbg target use`
- `mkdbg build`
- `mkdbg flash`
- `mkdbg incident open`
- `mkdbg incident status`
- `mkdbg incident close`
- `mkdbg incident export`
- `mkdbg probe status`
- `mkdbg probe reset`
- `mkdbg probe halt`
- `mkdbg probe resume`
- `mkdbg probe flash`
- `mkdbg probe read32`
- `mkdbg probe write32`
- `mkdbg capture bundle`
- `mkdbg hil`
- `mkdbg snapshot`
- `mkdbg attach`
- `mkdbg replay`
- `mkdbg replay --timeline`
- `mkdbg diff`
- `mkdbg run`
- `mkdbg watch`
- `mkdbg seam analyze`
- `mkdbg debug`

It intentionally does not try to replace:

- native GDB commands
- OpenOCD board configuration internals
- repo-specific build systems
