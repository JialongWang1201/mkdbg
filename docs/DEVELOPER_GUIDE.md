# Developer Guide

Internal engineering handbook for mkdbg maintainers and contributors.
For user-facing onboarding, read `README.md` first.
For porting to a new MCU, read `docs/PORTING.md`.

---

## Table of Contents

- [1. Architecture](#1-architecture)
- [2. Source layout](#2-source-layout)
- [3. Build](#3-build)
- [4. Adding a new arch plugin](#4-adding-a-new-arch-plugin)
- [5. Testing](#5-testing)
- [6. Contribution guide](#6-contribution-guide)

---

## 1. Architecture

mkdbg is a host CLI for embedded crash diagnostics over UART.

```
  ┌──────────────────────────────────────────────────────────┐
  │  mkdbg-native (host)                                     │
  │                                                          │
  │  main.c ──► core.c ──► action.c                          │
  │                  │         │                             │
  │                  │         ├──► wire.c ──► wire_crash_lib│
  │                  │         │         (RSP over UART)     │
  │                  │         ├──► seam.c ──► seam_lib      │
  │                  │         │         (causal analysis)   │
  │                  │         └──► dashboard.c (TUI)        │
  │                  │                                       │
  │                  └──► git.c, incident.c, probe.c, ...   │
  │                                                          │
  │  arch/ ──► arch_registry.c ──► cortex_m.c               │
  │            (decode_crash plugin interface)               │
  └──────────────────────────────────────────────────────────┘
                    │ UART
  ┌──────────────────────────────────────────────────────────┐
  │  Target firmware                                         │
  │  wire_agent.c  — RSP stub, halts on fault                │
  │  seam_agent.c  — 64-entry event ring (optional)          │
  └──────────────────────────────────────────────────────────┘
```

### Key data flow — `mkdbg attach`

1. `wire.c` forks `wire-host --dump` (or calls `wire_probe_dump()` directly)
2. `wire_crash.c` exchanges three RSP transactions with the halted MCU:
   - `?` → halt signal
   - `g` → 17 registers
   - `m <sp>,100` → 256 stack bytes
   - `m e000ed28,4` → CFSR
3. Result is a JSON `WireCrashReport`
4. `action.c` formats and prints the crash report

The `arch/` layer provides architecture-specific decode (CFSR bits,
heuristic backtrace, register names) via a plugin interface so future
architectures do not require changes to the core.

---

## 2. Source layout

```
src/                   mkdbg host CLI (C11, no external deps beyond deps/)
  main.c               entry point, argument dispatch
  core.c               repo + session state
  action.c             command handlers (attach, dashboard, seam, ...)
  wire.c               wire probe integration
  seam.c               seam causal-chain integration
  dashboard.c          TUI (termbox2)
  git.c                libgit2 wrappers
  probe.c              device probe detection
  incident.c           incident metadata
  config.c             ~/.mkdbg/config persistence
  serial.c             serial port helpers
  launcher.c           subprocess management
  parse.c              JSON/text parsing helpers
  process.c            child process management
  util.c               string, path, die()
  mkdbg.h              all types and function declarations
  termbox2.h           vendored single-header TUI library

arch/                  MCU architecture plugins
  arch.h               MkdbgArch interface + mkdbg_arch_find()
  arch_registry.c      static registry of built-in arch implementations
  cortex_m.c           Cortex-M: CFSR decode, heuristic backtrace, registers

deps/                  git submodules
  wire/                RSP stub firmware agent + host crash library
  seam/                fault event ring firmware agent + host analyzer
  libgit2/             local git ops (no git CLI required at runtime)

tests/
  arch_test.c          arch plugin registry smoke test
  fixtures/            test input files

examples/stm32f446/    STM32F446RE reference implementation
  src/                 firmware source
  scripts/             build, flash, CI scripts
  docs/                STM32-specific documentation
```

---

## 3. Build

**Prerequisites:** `cmake >= 3.20`, a C11 compiler, `git` with submodules.

```bash
git clone --recurse-submodules https://github.com/JialongWang1201/mkdbg
cmake -S mkdbg -B mkdbg/build_host -DCMAKE_BUILD_TYPE=Release
cmake --build mkdbg/build_host --target mkdbg-native wire-host seam-analyze
```

Binaries land in `build_host/`:
- `mkdbg-native` — main CLI
- `wire-host` — TCP↔UART bridge for GDB
- `seam-analyze` — standalone causal-chain analyzer

**Without libgit2** (if submodule not initialised):

The `git` panel in the dashboard falls back to calling the `git` CLI
subprocess.  Everything else works normally.

**Run host tests:**

```bash
cmake --build build_host --target arch_test
ctest --test-dir build_host -V
```

---

## 4. Adding a new arch plugin

Each arch plugin is one `.c` file in `arch/`.

**Step 1 — create `arch/your_arch.c`:**

```c
#include "arch.h"
#include <stdint.h>
#include <string.h>

static int your_arch_decode_crash(const uint8_t *raw, size_t len,
                                   MkdbgCrashReport *out)
{
    if (!raw || len < YOUR_ARCH_RAW_MIN || !out) return -1;
    /* decode raw bytes into out->regs[], out->cfsr, out->cfsr_decoded, etc. */
    return 0;
}

const MkdbgArch your_arch_arch = {
    .name         = "your-arch",   /* matched by --arch flag */
    .decode_crash = your_arch_decode_crash,
};
```

**Step 2 — register in `arch/arch_registry.c`:**

```c
extern const MkdbgArch your_arch_arch;

static const MkdbgArch *arches[] = {
    &cortex_m_arch,
    &your_arch_arch,   /* add here */
    NULL,
};
```

**Step 3 — add to `CMakeLists.txt`:**

```cmake
add_executable(mkdbg-native
  ...
  arch/your_arch.c      # add this line
  ...
)
```

**Step 4 — test:**

```bash
./build_host/mkdbg-native --version        # must still build and run
./build_host/arch_test                     # lookup tests
./build_host/mkdbg-native attach --port /dev/ttyUSB0 --arch your-arch
```

---

## 5. Testing

### Host unit tests

```bash
# arch plugin registry
cmake --build build_host --target arch_test
./build_host/arch_test

# seam-analyze integration
cmake --build build_host --target seam-analyze
./build_host/seam-analyze deps/seam/test/fixtures/normal_kdi_cascade.cfl

# STM32 reference build + scripts (requires arm-none-eabi-gcc)
bash examples/stm32f446/scripts/ci_smoke.sh
```

### CI

All jobs are in `.github/workflows/ci.yml`:

| Job | What it runs |
|-----|-------------|
| `native-host-artifacts` | CMake build for linux/darwin, binary smoke test |
| `smoke` | STM32 firmware build + `ci_smoke.sh` |
| `host-tests` | vm32, kdi, sonic-lite, bringup, analysis-engine host tests |
| `host-regress` | vm32 CI, profile compare, triage regression |

---

## 6. Contribution guide

### Change policy

- One logical change per commit.
- Minimal diff: change only what is necessary.
- New C code must compile with `-Wall -Wextra -Werror`.
- CI must pass before merging.

### Commit style

```
subsystem: short description

Optional body explaining why.
```

Examples:
- `arch: add ESP32 DPORT register decoder`
- `wire: increase RSP timeout to 5s`
- `docs: fix PORTING.md baud rate example`

### Adding a command

1. Add a handler function in `src/action.c`.
2. Wire it into the dispatch table in `src/core.c`.
3. Document in `docs/COMMANDS.md`.

### Extending WireCrashReport

`WireCrashReport` is defined in `src/mkdbg.h`.  Add fields additively — do
not remove or reorder existing fields, as the JSON parser in `src/wire.c`
matches by field name.

### Submodule policy

`deps/wire`, `deps/seam`, `deps/libgit2` are external submodules.  Do not
commit changes to them in this repo — send PRs upstream instead.
