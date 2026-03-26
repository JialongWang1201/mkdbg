## VM32 Debug Guide

### CLI Flow

- `vm reset`: clear VM state and memory.
- `vm load <addr> <b0> <b1> ...`: write bytes into VM memory.
- `vm step [n]`: execute N instructions (default 1).
- `vm run <addr> [max]`: run from address until HALT, error, or `max`.
- `vm trace on|off`: enable instruction trace output.

### Breakpoints

- `vm break <addr>`: set one breakpoint.
- `vm breaks`: show current breakpoint.
- `vm clear`: clear breakpoint.

Breakpoint hit behavior:
- Execution stops *before* the instruction at the breakpoint is executed.

### Trace Output

Trace line format:

```
VM32 pc=0x00000000 op=0x01 ds=0x00000001 rs=0x00000000 ic=1
```

Fields:
- `pc`: program counter (byte address).
- `op`: opcode byte at `pc`.
- `ds/rs`: current data/return stack depths.
- `ic`: instruction counter.
