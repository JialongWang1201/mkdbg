# Contributing to mkdbg

mkdbg receives evidence from targets that may be halted, rebooting, or only
partly responsive. Contributions should preserve that evidence and make it
clear which operations changed target state. This guide applies to the host
debugger, the reference firmware, and the tools in this repository.

## Start a change

For a bug report, include the mkdbg version or commit, host OS and architecture,
the command you ran, and the expected and observed result. For a target issue,
also include the board or chip revision, firmware build or ELF identity, and
transport or probe. A short sanitized log or a small fixture is usually enough.
Report vulnerabilities privately using the [security policy](SECURITY.md).

For changes to a protocol, both sides of a host–firmware boundary, or several
target architectures, outline the compatibility and validation plan in an
issue or draft PR early. Explain which behavior belongs to the transport,
architecture, or board.

Fork the repository if you need a place to push. Start feature, fix, and
documentation branches from the upstream `next` branch. Open the PR against
`next`. Maintainers promote changes to `main` for release.

```bash
git clone --recurse-submodules https://github.com/YOUR-GITHUB-USER/mkdbg.git
cd mkdbg
git remote add upstream https://github.com/JialongWang1201/mkdbg.git
git fetch upstream next
git switch -c fix/short-description upstream/next
git submodule update --init --recursive
```

## Code style

Match the surrounding code when editing an existing file. Give new files the
same style as their subsystem, and keep formatting-only changes in a separate
commit so reviewers can see the behavior change clearly.

- **C.** Use spaces. The native CLI and STM32F446 firmware commonly use two
  spaces per level. The debug session and architecture decoders commonly use
  four. For a new module with no nearby convention, use two spaces. See the
  [CLI probe commands](src/probe.c), [debug session](src/debug_session.c), and
  [firmware fault handling](examples/stm32f446/src/fault.c).
- **Rust.** Use default `rustfmt` formatting. See the
  [probe run control module](probe-bridge/src/run_control.rs).
- **Python.** Use four spaces per level, `snake_case` for functions and
  variables, and `PascalCase` for classes. Prefer the standard library for
  standalone diagnostics. See the [board evidence tool](tools/board_evidence.py).
- **Shell.** Follow the script's declared interpreter. Quote paths and argument
  expansions and pass external commands their arguments individually. See the
  [CI smoke script](scripts/ci_smoke.sh).

In new C code, put a function's opening brace on its own line. Put the opening
brace of a control block on the same line as its condition or keyword. Write
pointers as `const char *name`. Use `snake_case` for functions and variables,
`PascalCase` for project-defined types, and `UPPER_SNAKE_CASE` for new macros.
Use `static` for helpers used in one translation unit.

Use names that expose units or side effects, such as `timeout_ms` or
`halt_target`. Keep parsing and decoding separate from target I/O where
practical so they can be tested with fixtures. Comment invariants that a
reviewer cannot infer from the code, such as byte order, ownership transfers,
or why a memory barrier is required.

### Check formatting before opening a PR

From the repository root, check your pending edits and your branch's commits.

```bash
git diff --check HEAD
git diff --check upstream/next...HEAD
```

For changes to the Rust probe bridge, use the crate's format check. Remove
`--check` to apply `rustfmt`, then review the result.

```bash
cargo fmt --manifest-path probe-bridge/Cargo.toml --check
```

Run the existing tests for the area you changed. For example, run
`python3 tests/platform_diagnostics_test.py` for the platform diagnostic
tools. See the [host build and test commands](#build-and-test-on-the-host) and
[verification table](#choose-verification-for-the-change) for broader checks.

## Build and test on the host

Use CMake 3.20 or later, a C11 compiler, Git, and Python 3.10 or later.
Python enables the CLI integration tests registered by CMake. Install
`tclsh` to exercise simulated OpenOCD operations. Run the baseline suite from the
repository root on Linux or macOS. This configuration exercises the host
without a board, probe, or Rust toolchain.

```bash
cmake -S . -B build_host -DCMAKE_BUILD_TYPE=Debug \
  -DMKDBG_SKIP_LG2=ON -DMKDBG_SKIP_PROBE=ON
cmake --build build_host --parallel
ctest --test-dir build_host --output-on-failure
```

For a change to the probe bridge, also run its locked Rust tests and build the
native CLI with probe support in a separate directory. This requires Cargo and,
on Linux, the libudev development headers (`libudev-dev` on Debian and Ubuntu).

```bash
cargo test --locked --manifest-path probe-bridge/Cargo.toml
cmake -S . -B build_probe -DCMAKE_BUILD_TYPE=Release -DMKDBG_SKIP_PROBE=OFF
cmake --build build_probe --target mkdbg-native --parallel
```

Use the [developer guide](docs/DEVELOPER_GUIDE.md) for source layout and
architecture plug-ins. The [CI workflow](.github/workflows/ci.yml) covers
host tests, binary builds, and STM32 build and regression jobs. Its portable
crash-output job runs on Linux, macOS, and Windows. GitHub runs the workflow
on PRs targeting `next` or `main`.

## Engineering standards

- **Scope.** Keep each commit focused on one behavior. Trace the failure through
  its normal entry point before changing code. When the cause is shared
  state or a broken boundary, fix the ownership or contract at that boundary.
- **Host code.** Keep changed code warning-free under the target's compiler
  flags. The native CLI is built with `-Wall -Wextra -Werror`. Keep resource
  ownership and cleanup visible. Errors should identify the operation that
  failed and whether target state was confirmed.
- **Inputs and compatibility.** Treat UART and probe responses, ELF/DWARF files,
  and saved captures as untrusted input. Bound lengths and offsets before
  reading. Use explicit widths and byte order at protocol boundaries. Preserve
  supported CLI behavior and existing capture formats. Document migrations
  when a compatibility change is necessary.
- **Target and firmware code.** Account for alignment, linker layout, stack
  use, interrupt latency, and concurrency. Keep new capture work in fault
  and ISR paths bounded and allocation-free. Document which reset types can
  preserve a record and how the host identifies its matching image.
- **Debug state.** Distinguish reading state, halting, resuming, resetting, and
  writing. For operations that own temporary breakpoints or halts, check
  cleanup on timeout, partial failure, and session exit. Report unknown state
  if the target did not confirm the final state.
- **Dependencies.** Changes inside `deps/` belong in the relevant upstream
  repository. When updating a submodule pin, link the upstream change and
  verify the new pin with the mkdbg code that uses it.

## Choose verification for the change

| Area | Evidence reviewers need |
| --- | --- |
| CLI, parsers, replay, or transports | An affected command, recorded input, or test that covers the changed behavior. For parsing or run-control fixes, cover the relevant failure path. |
| Probe or session behavior | Host tests with a fake transport where possible. Include a real probe run when the change relies on halt, resume, reset, or device-specific behavior. |
| Firmware, ISR, linker, or reset retention | A cross-build for the reference firmware. Include a board run for changes that depend on reset, timing, memory layout, or hardware state. Distinguish each reset scenario tested. |
| Board bring-up or SoC capture | Identify the board revision, debug configuration, and channels or register ranges used. Report missing measurements and clock-alignment uncertainty when they affect the conclusion. |
| Documentation or install examples | Resolve links and check the changed commands against the current checkout. Test changed setup or install steps on the relevant host. |

Prefer a focused regression test when it can catch the specific failure. If a
test needs equipment you lack, record the host checks you ran and the remaining
hardware steps in the PR.

The STM32F446 build script requires the Arm GNU toolchain and CMake 3.24 or
later. The HIL gate also needs a compatible test board, OpenOCD, and Python
with `pyserial`.

```bash
bash examples/stm32f446/scripts/build.sh
bash examples/stm32f446/scripts/hil_gate.sh --port /dev/ttyACM0
```

Use the actual serial device for your board. The HIL gate can flash and reset
the target. Use a dedicated fixture or a board you have permission to modify.
For a hardware result, report the board and probe, firmware identity, command,
and pass or failure. Keep timing or reset claims tied to the case you ran.
See the [HIL workflow](.github/workflows/hil-gate.yml) for runner conditions and
the [STM32 example](examples/stm32f446/README.md) for board setup.

## Open a pull request

Use a short imperative summary and put the subsystem in the scope when it
helps reviewers locate the change. Use the same style for commit subjects
and PR titles.

```text
fix(debug): restore target state when detach fails
feat(firmware): preserve the faulting image build ID
docs: describe the board capture workflow
```

Push your topic branch to your fork before opening the PR.

```bash
git push --set-upstream origin HEAD
```

Explain the problem and the behavior change. Include reproducible validation
commands and results, plus any remaining hardware checks. Call out changes to
CLI or capture compatibility, hardware side effects, and dependency pins.
Update the [command reference](docs/COMMANDS.md) for changes to public flags,
configuration, or output consumers rely on.

Keep generated binaries, raw memory dumps, private firmware, credentials, and
company board data out of Git and public PR artifacts. Use small synthetic or
sanitized fixtures under `tests/fixtures/` when a regression needs a sample.
Address review comments and resolve failures in applicable CI jobs before
requesting merge.
