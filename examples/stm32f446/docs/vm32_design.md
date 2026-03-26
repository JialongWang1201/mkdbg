## VM32 Design (32-bit, dual-stack, no unconditional jump)

### Goals
- 32-bit data width.
- Two stacks: data stack + return stack.
- No unconditional jump instruction (control flow via conditional + return stack).
- Instruction counter (IC) drives IO pacing.
- Hot load + live patch over UART.
- Trace output for observability.

### Core State
- `pc` (u32): program counter in VM memory (byte address).
- `ic` (u32): instruction counter, increments per instruction.
- `ds[]` (u32), `rs[]` (u32): data/return stacks.
- `mem[]` (u8): VM memory image.
- `flags`: Z/N for basic conditionals.

### Memory Model
- VM memory size: 256 B / 1 KB / 4 KB (build-time). All addressing wraps modulo size.
- IO region is memory-mapped:
  - `0x0FF0`: UART TX (write).
  - `0x0FF4`: UART RX (read, 0 if empty).
  - `0x0FF8`: GPIO/LED (write).
  - `0x0FFC`: TIMER/IC (read-only).
- Optional MIG policy layer:
  - mode `off|monitor|enforce`
  - ACL mask over `uart_tx|uart_rx|led|ic`
  - monitor counts violations; enforce stops with `VM32_ERR_POLICY`

### Instruction Format
- Fixed 1-byte opcode.
- Optional immediate (32-bit little-endian).

### Instruction Set (MVP)
Stack:
- `PUSH imm32`, `DUP`, `DROP`, `SWAP`, `OVER`
ALU:
- `ADD`, `SUB`, `AND`, `OR`, `XOR`, `NOT`, `SHL`, `SHR`
Memory:
- `LOAD`, `STORE` (addr from stack)
Control:
- `JZ rel8` (relative branch if TOS == 0)
- `CALL rel8` (push return addr to RS, branch)
- `RET`
IO:
- `IN`, `OUT` (addr from stack)
System:
- `HALT`, `NOP`

### No Unconditional Jump
- Use `JZ` + `NOT` pattern for conditional flow.
- Use `CALL/RET` for subroutine sequencing.

### Bounded CFG Profile
- Optional bounded mode validates control flow before run.
- Validation window is `[entry, entry + span)`, without wrap-around.
- Rejected patterns:
  - `CALL` / `RET`
  - backward edges (`target <= current_pc`)
  - branch/fallthrough outside window
- On success, runtime gets a deterministic upper bound (`max_steps`) based on
  reachable instruction nodes.
- CLI:
  - `vm verify <addr> [span]`
  - `vm runb <addr> [span]`

### Instruction Counter IO
- IC increments every instruction.
- Optional pacing: if `ic % N == 0`, toggle heartbeat LED or yield.

### Hot Load / Patch
- UART commands:
  - `vm load <addr> <hex...>`: write bytes into VM memory.
  - `vm run <addr>`: set PC and run.
  - `vm step <n>`: execute N steps.
  - `vm dump <addr> <len>`: read memory.
  - `vm reset`: clear state.
  - `vm mig ...`: manage VM IO ACL policy and audit counters.
  - `vm mig apply <json>`: northbound-style policy update (`mode/allow/deny/reset`).

### Trace
- UART trace lines: `PC OPCODE DS_TOP RS_TOP FLAGS IC`.
- Enable/disable at runtime with `vm trace on|off`.
