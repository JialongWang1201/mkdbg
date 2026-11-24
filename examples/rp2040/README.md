# Raspberry Pi Pico (RP2040)

This bring-up starts with the Raspberry Pi Pico and a CMSIS-DAP SWD probe.
OpenOCD handles board discovery and programming. The native mkdbg probe bridge
selects RP2040 core 0 for interactive live debug.

Connect the probe to the Pico's SWD pins and power the Pico. Use an ELF built
with debug information, such as the `swift-blinky.elf` output of a `Debug`
build of the Embedded Swift Pico SDK example. Use the upstream setup guide
and a Swift toolchain that ships the Embedded Swift standard library for
`armv6m-none-none-eabi`.
[Debug Probe wiring](https://www.raspberrypi.com/documentation/microcontrollers/debug-probe.html) ·
[Pico SDK setup guide](https://github.com/swiftlang/swift-embedded-examples/blob/d606e60/Sources/EmbeddedSwift/EmbeddedSwift.docc/Examples/RPiPicoGuide.md)

## Build the Swift ELF

Select the Swift snapshot pinned by the example repository and set
`PICO_SDK_PATH` and `PICO_TOOLCHAIN_PATH` as described in the setup guide.
For Pico SDK 2.2.0, apply the `hardware_sync` compatibility patch used by
the upstream CI workflow. [Swift snapshot](https://github.com/swiftlang/swift-embedded-examples/blob/d606e60/.swift-version) ·
[Pico SDK CI setup](https://github.com/swiftlang/swift-embedded-examples/blob/d606e60/.github/workflows/build-rpi-pico-sdk.yml)

From `rpi-pico-blink-sdk`, build the ELF with debug information.

```bash
cmake -S . -B build -G Ninja -DPICO_BOARD=pico -DCMAKE_BUILD_TYPE=Debug \
  -DPICO_NO_PICOTOOL=1
cmake --build build --parallel
```

## Register the board

Run these commands from the firmware project's directory. If the project
needs a `.mkdbg.toml`, first run `mkdbg init --preset generic --name pico`.
Set `MKDBG_SOURCE` to the mkdbg checkout and adjust `elf_path` for the
firmware build you want to debug.

```bash
MKDBG_SOURCE=/absolute/path/to/mkdbg
elf_path=build/swift-blinky.elf

mkdbg target add pico --path "$PWD" --preset generic --elf-path "$elf_path" \
  --chip RP2040 --arch cortex-m \
  --openocd-cfg "$MKDBG_SOURCE/examples/rp2040/openocd.cfg"
mkdbg probe status --target pico --dry-run
mkdbg probe status --target pico
```

Check that the target table includes `rp2040.core0` and `rp2040.core1`.
Run `mkdbg probe flash --target pico` when you want to program the selected
ELF and reset the board. Finish the OpenOCD operation before opening the
native debug session so the probe is available.

## Verify native live debug

Build mkdbg with probe support enabled to run this check.
[Probe build steps](../../CONTRIBUTING.md#build-and-test-on-the-host)

```bash
mkdbg debug --target pico --probe
```

Run `regs` and check that it lists 17 registers, including PC, SP and xPSR.
Use `arm-none-eabi-objdump -d -l "$elf_path"`
to find an instruction address inside the repeating loop in `Main.swift`.
Use `break` with that address and `continue` to reach it even if the board
already ran its startup code. Check the reported Swift source line, then
use `step` to advance one instruction. Use `quit` to clear the session's
breakpoints and resume the core. Check that the LED starts blinking again.
Record the board revision, probe, ELF, and results when reporting a hardware test.

The Swift Blink image reached the loop breakpoint and stepped to the next
Swift source line in an [rp2040js](https://github.com/wokwi/rp2040js) simulation
connected to mkdbg over a pseudo-terminal. Flashing over OpenOCD and live
debugging through CMSIS-DAP SWD still need a hardware run.
