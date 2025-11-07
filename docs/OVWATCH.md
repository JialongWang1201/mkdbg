# ovwatch

`ovwatch` is a repo-aware debug entrypoint with its own target model and
command surface. It is meant to be your project-facing debugger CLI,
while target-specific debug transports stay behind adapter definitions.

Current built-in adapters:

- `microkernel-mpu`
- `tahoeos`
- `generic`

## Install

Local checkout:

```bash
bash tools/install_ovwatch.sh
```

One-line remote install:

```bash
curl -fsSL https://raw.githubusercontent.com/JialongWang1201/MicroKernel-MPU/main/tools/install_ovwatch.sh | sh
```

## Quick Start

Initialize the current repo as a `MicroKernel-MPU` target:

```bash
ovwatch init --name microkernel --adapter microkernel-mpu
ovwatch doctor
ovwatch attach-plan
ovwatch debug
```

Register another checkout with the `TahoeOS` adapter:

```bash
ovwatch target add tahoe \
  --adapter tahoeos \
  --path ../TahoeOS \
  --build-cmd "make -j4" \
  --run-cmd "make run" \
  --debug-cmd "./scripts/debug_session.sh"
```

Then inspect the resolved target plan:

```bash
ovwatch target list
ovwatch doctor tahoe
ovwatch run tahoe --dry-run
ovwatch attach-plan tahoe
ovwatch debug tahoe --dry-run
```

## Config

`ovwatch` searches upward from the current working directory for
`.ovwatch.toml`.

Example:

```toml
version = 1
default_target = "microkernel"

[targets."microkernel"]
adapter = "microkernel-mpu"
path = "."
build_cmd = "bash tools/build.sh"
flash_cmd = "bash tools/flash.sh"
probe_transport = "openocd-gdb"
probe_config = "tools/openocd.cfg"
probe_endpoint = "localhost:3333"
debug_client = "arm-none-eabi-gdb"
elf_path = "build/MicroKernel_MPU.elf"
serial_port = "/dev/cu.usbmodem21303"
```

Template fields:

- `{target}`
- `{target_root}`
- `{elf_path}`
- `{probe_config}`
- `{probe_endpoint}`
- `{serial_port}`

## Scope

Current foundation supports:

- `ovwatch init`
- `ovwatch target add`
- `ovwatch target list`
- `ovwatch doctor`
- `ovwatch build`
- `ovwatch flash`
- `ovwatch run`
- `ovwatch attach-plan`
- `ovwatch debug`

This does not claim to implement a native debug protocol yet. The first
goal is a stable project-facing CLI and target adapter contract.
