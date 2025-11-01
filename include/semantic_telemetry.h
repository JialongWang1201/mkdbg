#ifndef SEMANTIC_TELEMETRY_H
#define SEMANTIC_TELEMETRY_H

#include <stddef.h>
#include <stdint.h>

/* Runtime object class aligned with docs/semantic_telemetry/schema.json */
typedef enum {
  SEMTELEM_KIND_EVENT = 1,
  SEMTELEM_KIND_STATE_SNAPSHOT = 2,
  SEMTELEM_KIND_RESOURCE_METRIC = 3
} SemTelemRecordKind;

typedef enum {
  SEMTELEM_SRC_KERNEL = 1,
  SEMTELEM_SRC_DRIVER = 2,
  SEMTELEM_SRC_SERVICE = 3,
  SEMTELEM_SRC_USER = 4,
  SEMTELEM_SRC_HOST = 5
} SemTelemSourceClass;

typedef enum {
  SEMTELEM_ENTITY_NONE = 0,
  SEMTELEM_ENTITY_SYSTEM = 1,
  SEMTELEM_ENTITY_DRIVER = 2,
  SEMTELEM_ENTITY_IRQ = 3,
  SEMTELEM_ENTITY_DMA_RING = 4,
  SEMTELEM_ENTITY_PHASE = 5,
  SEMTELEM_ENTITY_POLICY = 6,
  SEMTELEM_ENTITY_SERVICE = 7,
  SEMTELEM_ENTITY_BUFFER = 8
} SemTelemEntityKind;

typedef enum {
  SEMTELEM_SEV_INFO = 1,
  SEMTELEM_SEV_WARN = 2,
  SEMTELEM_SEV_ERROR = 3,
  SEMTELEM_SEV_FATAL = 4
} SemTelemSeverity;

typedef enum {
  SEMTELEM_FREQ_LOW = 0,
  SEMTELEM_FREQ_HIGH = 1
} SemTelemFreqClass;

typedef enum {
  SEMTELEM_EVENT_COMMAND = 1,
  SEMTELEM_EVENT_KDI_CALL = 2,
  SEMTELEM_EVENT_CAPABILITY_CHECK = 3,
  SEMTELEM_EVENT_STATE_TRANSITION = 4,
  SEMTELEM_EVENT_IRQ = 5,
  SEMTELEM_EVENT_DEFERRED_WORK = 6,
  SEMTELEM_EVENT_DMA = 7,
  SEMTELEM_EVENT_RESOURCE_PRESSURE = 8,
  SEMTELEM_EVENT_BRINGUP_STAGE = 9,
  SEMTELEM_EVENT_POLICY_CHANGE = 10,
  SEMTELEM_EVENT_FAULT = 11,
  SEMTELEM_EVENT_RESET = 12
} SemTelemEventType;

typedef enum {
  SEMTELEM_SNAPSHOT_SYSTEM = 1,
  SEMTELEM_SNAPSHOT_BRINGUP = 2,
  SEMTELEM_SNAPSHOT_DRIVER = 3,
  SEMTELEM_SNAPSHOT_FAULT = 4,
  SEMTELEM_SNAPSHOT_IRQ = 5,
  SEMTELEM_SNAPSHOT_DMA = 6,
  SEMTELEM_SNAPSHOT_POLICY = 7
} SemTelemSnapshotType;

typedef enum {
  SEMTELEM_SNAPSHOT_REASON_ON_DEMAND = 1,
  SEMTELEM_SNAPSHOT_REASON_PERIODIC = 2,
  SEMTELEM_SNAPSHOT_REASON_ON_FAULT = 3,
  SEMTELEM_SNAPSHOT_REASON_ON_RESET = 4,
  SEMTELEM_SNAPSHOT_REASON_ON_TRANSITION = 5
} SemTelemSnapshotReason;

typedef enum {
  SEMTELEM_METRIC_CLASS_COUNT = 1,
  SEMTELEM_METRIC_CLASS_RATE = 2,
  SEMTELEM_METRIC_CLASS_LEVEL = 3
} SemTelemMetricClass;

typedef enum {
  SEMTELEM_METRIC_UNIT_COUNT = 1,
  SEMTELEM_METRIC_UNIT_BYTES = 2,
  SEMTELEM_METRIC_UNIT_BYTES_PER_S = 3,
  SEMTELEM_METRIC_UNIT_PCT = 4,
  SEMTELEM_METRIC_UNIT_SLOTS = 5,
  SEMTELEM_METRIC_UNIT_US = 6
} SemTelemMetricUnit;

typedef enum {
  SEMTELEM_EMIT_OK = 0,
  SEMTELEM_EMIT_DROP_SAMPLED = 1,
  SEMTELEM_EMIT_DROP_FULL = 2,
  SEMTELEM_EMIT_BAD_ARG = -1
} SemTelemEmitStatus;

typedef struct {
  uint64_t ts;
  uint32_t corr_id;
  uint32_t span_id;
  uint32_t parent_span_id;
  uint16_t src_id;
  uint16_t entity_id;
  uint16_t domain_id;
  uint8_t src_class;
  uint8_t entity_kind;
} SemTelemMeta;

typedef struct {
  SemTelemMeta meta;
  SemTelemEventType type;
  SemTelemSeverity severity;
  uint16_t state_from;
  uint16_t state_to;
  int32_t code;
  uint32_t data0;
  uint32_t data1;
  uint32_t data2;
  uint32_t data3;
} SemTelemEvent;

typedef struct {
  SemTelemMeta meta;
  SemTelemSnapshotType snapshot_type;
  SemTelemSnapshotReason snapshot_reason;
  int32_t word0;
  int32_t word1;
  int32_t word2;
  int32_t word3;
  int32_t word4;
  int32_t word5;
} SemTelemSnapshot;

typedef struct {
  SemTelemMeta meta;
  uint16_t resource_kind;
  uint16_t resource_id;
  uint16_t metric_id;
  SemTelemMetricClass metric_class;
  SemTelemMetricUnit unit;
  SemTelemFreqClass freq_class;
  int32_t value;
  int32_t capacity;
  int32_t high_watermark;
  uint32_t window_us;
  uint32_t data0;
  uint32_t data1;
} SemTelemMetric;

typedef struct {
  uint32_t seq;
  SemTelemMeta meta;
} SemTelemRecordBase;

typedef struct {
  SemTelemEventType type;
  SemTelemSeverity severity;
  SemTelemFreqClass freq_class;
  uint8_t reserved0;
  uint16_t state_from;
  uint16_t state_to;
  int32_t code;
  uint32_t data0;
  uint32_t data1;
  uint32_t data2;
  uint32_t data3;
} SemTelemEventRecord;

typedef struct {
  SemTelemSnapshotType snapshot_type;
  SemTelemSnapshotReason snapshot_reason;
  uint16_t reserved0;
  int32_t word0;
  int32_t word1;
  int32_t word2;
  int32_t word3;
  int32_t word4;
  int32_t word5;
} SemTelemSnapshotRecord;

typedef struct {
  uint16_t resource_kind;
  uint16_t resource_id;
  uint16_t metric_id;
  SemTelemMetricClass metric_class;
  SemTelemMetricUnit unit;
  SemTelemFreqClass freq_class;
  uint16_t reserved0;
  int32_t value;
  int32_t capacity;
  int32_t high_watermark;
  uint32_t window_us;
  uint32_t data0;
  uint32_t data1;
} SemTelemMetricRecord;

typedef struct {
  SemTelemRecordKind kind;
  uint8_t reserved0;
  uint16_t reserved1;
  SemTelemRecordBase base;
  union {
    SemTelemEventRecord event;
    SemTelemSnapshotRecord snapshot;
    SemTelemMetricRecord metric;
  } payload;
} SemTelemRecord;

typedef struct {
  uint16_t irq_every;
  uint16_t dma_every;
  uint16_t deferred_work_every;
  uint16_t metric_high_every;
} SemTelemSamplingPolicy;

typedef struct {
  uint32_t emit_attempt_total;
  uint32_t emit_ok_total;
  uint32_t drop_ring_full_total;
  uint32_t drop_sampled_total;
  uint32_t high_freq_seen_total;
  uint32_t high_freq_kept_total;
  uint32_t low_freq_seen_total;
  uint32_t max_depth;
  uint32_t irq_sample_seen;
  uint32_t dma_sample_seen;
  uint32_t deferred_sample_seen;
  uint32_t metric_sample_seen;
} SemTelemStats;

typedef void (*SemTelemLockFn)(void *ctx);

typedef struct {
  SemTelemRecord *storage;
  uint32_t capacity;
  uint32_t mask;
  uint32_t head_seq;
  uint32_t tail_seq;
  uint32_t next_record_seq;
  SemTelemSamplingPolicy sampling;
  SemTelemStats stats;
  SemTelemLockFn lock_fn;
  SemTelemLockFn unlock_fn;
  void *lock_ctx;
} SemTelemRing;

void sem_telem_sampling_policy_default(SemTelemSamplingPolicy *out);
void sem_telem_sampling_policy_no_sampling(SemTelemSamplingPolicy *out);

int sem_telem_ring_init(SemTelemRing *ring, SemTelemRecord *storage, uint32_t capacity);
void sem_telem_ring_reset(SemTelemRing *ring);
void sem_telem_ring_set_lock_hooks(SemTelemRing *ring,
                                   SemTelemLockFn lock_fn,
                                   SemTelemLockFn unlock_fn,
                                   void *ctx);
void sem_telem_ring_set_sampling_policy(SemTelemRing *ring, const SemTelemSamplingPolicy *policy);

SemTelemFreqClass sem_telem_event_default_freq(SemTelemEventType type);

SemTelemEmitStatus sem_telem_emit_event(SemTelemRing *ring, const SemTelemEvent *event);
SemTelemEmitStatus sem_telem_emit_snapshot(SemTelemRing *ring, const SemTelemSnapshot *snapshot);
SemTelemEmitStatus sem_telem_emit_metric(SemTelemRing *ring, const SemTelemMetric *metric);

int sem_telem_try_pop(SemTelemRing *ring, SemTelemRecord *out_record);
uint32_t sem_telem_depth(const SemTelemRing *ring);
uint32_t sem_telem_capacity(const SemTelemRing *ring);
void sem_telem_get_stats(const SemTelemRing *ring, SemTelemStats *out_stats);

#endif
