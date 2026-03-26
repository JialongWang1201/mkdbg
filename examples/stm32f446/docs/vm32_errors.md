## VM32 Error Codes

VM32 returns a small numeric status from `vm32_step()` / `vm32_run()` so the CLI
can stop cleanly and report the reason.

### Codes

- `0` (`VM32_OK`): normal step/run completed.
- `1` (`VM32_ERR_HALT`): HALT instruction executed.
- `2` (`VM32_ERR_STACK`): stack underflow/overflow detected.
- `3` (`VM32_ERR_MEM`): invalid memory access or illegal opcode.
- `4` (`VM32_ERR_CFG`): rejected by bounded CFG policy checker.
- `5` (`VM32_ERR_POLICY`): blocked by VM MIG policy in enforce mode.

### Typical Causes

- `VM32_ERR_STACK`: `DROP` on empty stack, `RET` with empty return stack,
  or any op needing more operands than available.
- `VM32_ERR_MEM`: load/store address invalid, or unknown opcode encountered.
- `VM32_ERR_CFG`: disallowed control-flow shape (e.g. backward edge, CALL/RET,
  branch target outside bounded window).
- `VM32_ERR_POLICY`: IO access denied by policy (e.g. `vm mig deny uart_tx`
  with `vm mig mode enforce`).

### CLI Behavior

- `vm step` and `vm run` stop on any non-zero error code.
- `vm break` stops execution before executing the instruction at the breakpoint.
