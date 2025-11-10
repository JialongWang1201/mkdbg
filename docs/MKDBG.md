# mkdbg

`mkdbg` is the supported public debug CLI for this repo. It is a repo-aware
embedded debug wrapper that sits on top of the existing build, flash, OpenOCD,
GDB, serial, and HIL tooling.

It is intentionally not a replacement for GDB. The goal is to make
common embedded flows feel like a single operator-facing CLI:

- initialize one local config
- register one or more repos
- build / flash / attach / snapshot / run HIL
- keep repo-specific commands in configuration instead of shell history

## Install

Local checkout:

```bash
bash tools/install_mkdbg.sh
```

One-line remote install:

```bash
curl -fsSL https://raw.githubusercontent.com/JialongWang1201/MicroKernel-MPU/main/tools/install_mkdbg.sh | sh
```

## Quick Start

Initialize the current repo as a `MicroKernel-MPU` target:

```bash
mkdbg init --name microkernel --port /dev/cu.usbmodem21303
```

Then use the common flows:

```bash
mkdbg doctor
mkdbg build
mkdbg flash
mkdbg attach
mkdbg attach --break main --command continue --command bt --batch
mkdbg snapshot --port /dev/cu.usbmodem21303
mkdbg hil --port /dev/cu.usbmodem21303
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
- `mkdbg build`
- `mkdbg flash`
- `mkdbg hil`
- `mkdbg snapshot`
- `mkdbg attach`
- `mkdbg run`

It intentionally does not try to replace:

- native GDB commands
- OpenOCD board configuration internals
- repo-specific build systems
