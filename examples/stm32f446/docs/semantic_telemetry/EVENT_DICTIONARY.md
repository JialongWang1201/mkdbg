# Semantic Telemetry Event Dictionary (v1)

This document defines the semantic meaning of telemetry event types used by the
MicroKernel-MPU System Intelligence Layer foundation.

Scope for Step 1 (schema-first):
- unify machine-readable records into three object classes
- define stable event categories for causal reasoning
- standardize correlation/tracing fields (`corr_id`, `span_id`, `parent_span_id`)

The corresponding schema is `docs/semantic_telemetry/schema.json`.

## Object Classes

### `EVENT`

Represents something that happened at a point in time.

Use for:
- calls, decisions, transitions, faults, resets, stage progress, policy changes
- causal edges in the future event graph

### `STATE_SNAPSHOT`

Represents a point-in-time structured state capture.

Use for:
- bring-up state dumps
- KDI driver state summaries
- fault snapshots
- DMA ring ownership/pressure snapshots

### `RESOURCE_METRIC`

Represents a numeric measurement for a resource (count/rate/level).

Use for:
- queue depth / watermark
- drops / retries / reclaim failures
- IRQ rate / deferred backlog rate
- DMA ring occupancy / pressure / throughput

## Required Correlation Semantics

### `corr_id` (required)

`corr_id` ties records to the same causal session or operation boundary.

Typical examples:
- one CLI command execution
- one driver probe attempt
- one driver reset/recovery cycle
- one bring-up phase run/rerun sequence
- one policy apply transaction

Guidelines:
- all records emitted as part of the same logical action share the same `corr_id`
- periodic/background metrics may use a reserved `corr_id` such as `bg` or `periodic`
- a new user-triggered command should normally start a new `corr_id`

### `span_id` / `parent_span_id` (optional, recommended)

These fields allow tracing-style nesting within one `corr_id`.

Example chain:
- `KDI_CALL` (`span_id=s1`)
- `IRQ` caused by that action (`span_id=s2`, `parent_span_id=s1`)
- `DMA` completion event (`span_id=s3`, `parent_span_id=s2`)

This enables future graph traversal such as:
- KDI call -> IRQ -> deferred work -> DMA -> fault

## Replay / Diff Normalization

Deterministic replay is the host-side normalization step that turns captured
telemetry or triage bundles into stable regression artifacts.

Replay identity should prefer:
- `corr_id`, stage, event ordering, and stable message/type content
- sorted flag sets and explicit empty objects/lists for absent optional data

Replay diff should avoid using wall-clock-only fields such as `age_ms` as the
primary identity key, because those values are observational and expected to
drift between runs even when causal structure is unchanged.

## Event Type Dictionary (`EVENT.type`)

The `type` field is a stable category, not a free-form log string.
Detailed subtype data goes in `data`.

### `COMMAND`

Semantic meaning:
- a host/CLI command boundary or high-level operation request

Use when:
- a CLI command starts/completes (`bringup phase run`, `kdi driver reset uart`)
- a host tool triggers a regression step or scripted action

Recommended `data` keys:
- `command`
- `args`
- `phase` (if applicable)
- `result`

Why it matters:
- natural root node for `corr_id`
- lets RCA start from operator intent, not only low-level consequences

### `KDI_CALL`

Semantic meaning:
- a kernel-driver interface contract invocation or response

Use when:
- KDI entry invoked by driver/kernel
- KDI call accepted/rejected/completed
- contract checks or return codes are produced

Recommended `data` keys:
- `kdi_op`
- `driver`
- `cap_token_id` (masked/truncated if needed)
- `rc`

Why it matters:
- primary kernel-driver boundary signal for contract enforcement analysis

### `CAPABILITY_CHECK`

Semantic meaning:
- a capability/token permission decision at the boundary

Use when:
- capability validated, denied, expired, or mismatched
- permission scope is narrowed/expanded at runtime

Recommended `data` keys:
- `driver`
- `capability`
- `decision` (`allow`/`deny`)
- `reason`
- `token_state`

Why it matters:
- feeds cap minimization and policy drift analysis later

### `STATE_TRANSITION`

Semantic meaning:
- a lifecycle/state machine edge for a tracked entity

Use when:
- driver lifecycle changes (`ACTIVE -> ERROR`, `RESET -> REINIT`)
- bring-up phase slot status changes (`pending -> done`, `done -> rolled_back`)
- runtime modes change statefully

Required/expected fields:
- `state_from`
- `state_to`

Recommended `data` keys:
- `entity_kind`
- `entity_id`
- `reason`
- `rc`

Why it matters:
- event graph transition edges are directly built from this type

### `IRQ`

Semantic meaning:
- interrupt context activity and IRQ-path decisions

Use when:
- ISR entry/exit
- IRQ storm detection/throttling
- IRQ path rejects unsafe operation

Recommended `data` keys:
- `irq_name` or `irqn`
- `action` (`enter`, `exit`, `throttle`, `deny`)
- `latency_us`
- `deferred_queued`

Why it matters:
- supports performance-model validation (IRQ minimal work + deferred work)

### `DEFERRED_WORK`

Semantic meaning:
- deferred/worker-context processing triggered from IRQ or other fast path

Use when:
- deferred queue push/pop/flush
- backlog thresholds exceeded
- worker starts/completes a unit of work

Recommended `data` keys:
- `queue`
- `action` (`enqueue`, `dequeue`, `flush`, `drop`)
- `depth`
- `work_type`

Why it matters:
- bridges IRQ activity to work completion and latency/pressure analysis

### `DMA`

Semantic meaning:
- DMA-like buffer/ring lifecycle events and ownership changes

Use when:
- RX/TX ring enqueue/dequeue/complete
- buffer ownership transfer (`driver -> kernel`, `kernel -> driver`)
- cache clean/invalidate (logical model)
- pressure/backpressure/drop events

Recommended `data` keys:
- `ring` (`rx`/`tx`)
- `slot`
- `owner_from`
- `owner_to`
- `buf_id`
- `len`
- `cache_op` (`clean`, `invalidate`)

Why it matters:
- core substrate for NIC-style zero-copy/DMA reasoning and leak detection

### `RESOURCE_PRESSURE`

Semantic meaning:
- pressure threshold crossings or saturation signals for a resource

Use when:
- queue/ring high-watermark exceeded
- backlog or memory pool depletion reaches threshold
- recovery from pressure state is detected

Recommended `data` keys:
- `resource_kind`
- `resource_id`
- `level`
- `capacity`
- `threshold`
- `action` (`assert`, `clear`)

Why it matters:
- enables early warning and root-cause correlation before faults occur

### `BRINGUP_STAGE`

Semantic meaning:
- bring-up pipeline progress/failure/rollback events at stage granularity

Use when:
- phase begin/commit/fail/rollback
- injected failures are consumed
- rerun starts from a given stage

Recommended `data` keys:
- `stage`
- `action` (`begin`, `commit`, `fail`, `rollback`, `inject`)
- `error`
- `injected`

Why it matters:
- drives bring-up explainability and restart-path analysis

### `POLICY_CHANGE`

Semantic meaning:
- policy plane mutations or enforcement-mode changes

Use when:
- MIG policy mode changes (`off/monitor/enforce`)
- allow/deny sets change
- config transaction commits/rollbacks/confirmations occur

Recommended `data` keys:
- `policy_domain` (`vm_mig`, `sonic`, `kdi_caps`, ...)
- `action`
- `key`
- `old`
- `new`

Why it matters:
- foundation for policy drift detection and what-if explanations

### `FAULT`

Semantic meaning:
- normalized fault/error events that represent correctness or safety failures

Use when:
- CPU fault handlers emit normalized fault records
- VM faults are normalized into fault pipeline
- KDI/driver containment faults are recorded

Recommended `data` keys:
- `fault_kind` (`CPU`, `VM`, `KDI`, ...)
- `fault_code`
- `containment`
- `domain`
- `pc` / `addr` (if available)

Why it matters:
- canonical anchor for RCA and fingerprinting

### `RESET`

Semantic meaning:
- reset/restart/recovery actions and outcomes

Use when:
- driver reset/reinit/restart
- subsystem reset sequence starts/completes
- board/system reset requested or observed

Recommended `data` keys:
- `target`
- `scope` (`driver`, `subsystem`, `system`, `board`)
- `reason`
- `rc`

Why it matters:
- allows containment/recovery reasoning without confusing reset actions with faults themselves

## Snapshot and Metric Naming Guidance

### `STATE_SNAPSHOT.snapshot_type`

Initial categories in schema:
- `SYSTEM`
- `BRINGUP`
- `DRIVER`
- `FAULT`
- `IRQ`
- `DMA`
- `POLICY`

Guideline:
- snapshots should be coarse enough to inspect quickly, but structured enough for machine diffing
- include stable field names in `state` for host-side graph extraction and drift checks

### `RESOURCE_METRIC.metric_class`

- `COUNT`: monotonically increasing total (e.g. drops, faults)
- `RATE`: per-window throughput/frequency (e.g. IRQ/s, bytes/s)
- `LEVEL`: instantaneous or sampled level/watermark (e.g. queue depth, ring occupancy)

## Interview-Friendly Summary (Short)

"We standardized runtime observations into semantic events, snapshots, and resource metrics with explicit correlation IDs. That gives us a machine-usable event graph substrate for RCA, drift detection, bring-up explainability, and future what-if analysis without changing the kernel/driver control plane semantics."
