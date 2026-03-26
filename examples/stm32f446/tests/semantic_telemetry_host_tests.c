#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "semantic_telemetry.h"

static int failures = 0;

static void check(int cond, const char *msg)
{
  if (!cond) {
    printf("FAIL: %s\n", msg);
    failures++;
  }
}

static SemTelemMeta make_meta(uint64_t ts, uint32_t corr_id, uint8_t src_class, uint16_t src_id)
{
  SemTelemMeta meta;

  memset(&meta, 0, sizeof(meta));
  meta.ts = ts;
  meta.corr_id = corr_id;
  meta.src_class = src_class;
  meta.src_id = src_id;
  meta.entity_kind = SEMTELEM_ENTITY_NONE;
  return meta;
}

static SemTelemEvent make_event(SemTelemEventType type, uint64_t ts, uint32_t corr_id)
{
  SemTelemEvent ev;

  memset(&ev, 0, sizeof(ev));
  ev.meta = make_meta(ts, corr_id, SEMTELEM_SRC_KERNEL, 1U);
  ev.type = type;
  ev.severity = (type == SEMTELEM_EVENT_FAULT) ? SEMTELEM_SEV_ERROR : SEMTELEM_SEV_INFO;
  ev.code = (type == SEMTELEM_EVENT_FAULT) ? -42 : 0;
  return ev;
}

static void test_init_validation(void)
{
  SemTelemRing ring;
  SemTelemRecord storage[8];

  check(sem_telem_ring_init(NULL, storage, 8U) == -1, "init rejects null ring");
  check(sem_telem_ring_init(&ring, NULL, 8U) == -1, "init rejects null storage");
  check(sem_telem_ring_init(&ring, storage, 6U) == -1, "init rejects non-power-of-two capacity");
  check(sem_telem_ring_init(&ring, storage, 8U) == 0, "init accepts power-of-two capacity");
  check(sem_telem_capacity(&ring) == 8U, "capacity visible after init");
}

static void test_invalid_meta_rejected(void)
{
  SemTelemRing ring;
  SemTelemRecord storage[8];
  SemTelemEvent ev;

  check(sem_telem_ring_init(&ring, storage, 8U) == 0, "init for invalid meta test");

  ev = make_event(SEMTELEM_EVENT_FAULT, 10U, 0U);
  check(sem_telem_emit_event(&ring, &ev) == SEMTELEM_EMIT_BAD_ARG, "corr_id=0 rejected");

  ev = make_event(SEMTELEM_EVENT_FAULT, 11U, 1U);
  ev.meta.parent_span_id = 99U;
  ev.meta.span_id = 0U;
  check(sem_telem_emit_event(&ring, &ev) == SEMTELEM_EMIT_BAD_ARG, "parent_span requires span_id");
}

static void test_high_low_frequency_sampling(void)
{
  SemTelemRing ring;
  SemTelemRecord storage[128];
  SemTelemSamplingPolicy policy;
  SemTelemStats stats;
  uint32_t i;
  uint32_t ok_count = 0U;
  uint32_t sampled_count = 0U;

  check(sem_telem_ring_init(&ring, storage, 128U) == 0, "init for sampling test");

  sem_telem_sampling_policy_no_sampling(&policy);
  policy.irq_every = 4U;
  policy.dma_every = 5U;
  policy.deferred_work_every = 3U;
  policy.metric_high_every = 2U;
  sem_telem_ring_set_sampling_policy(&ring, &policy);

  for (i = 0U; i < 20U; ++i) {
    SemTelemEvent ev = make_event(SEMTELEM_EVENT_IRQ, 1000U + i, 7U);
    SemTelemEmitStatus rc = sem_telem_emit_event(&ring, &ev);
    if (rc == SEMTELEM_EMIT_OK) {
      ok_count++;
    } else if (rc == SEMTELEM_EMIT_DROP_SAMPLED) {
      sampled_count++;
    }
  }
  for (i = 0U; i < 20U; ++i) {
    SemTelemEvent ev = make_event(SEMTELEM_EVENT_DMA, 2000U + i, 7U);
    SemTelemEmitStatus rc = sem_telem_emit_event(&ring, &ev);
    if (rc == SEMTELEM_EMIT_OK) {
      ok_count++;
    } else if (rc == SEMTELEM_EMIT_DROP_SAMPLED) {
      sampled_count++;
    }
  }
  for (i = 0U; i < 9U; ++i) {
    SemTelemEvent ev = make_event(SEMTELEM_EVENT_DEFERRED_WORK, 3000U + i, 7U);
    SemTelemEmitStatus rc = sem_telem_emit_event(&ring, &ev);
    if (rc == SEMTELEM_EMIT_OK) {
      ok_count++;
    } else if (rc == SEMTELEM_EMIT_DROP_SAMPLED) {
      sampled_count++;
    }
  }
  for (i = 0U; i < 10U; ++i) {
    SemTelemEvent ev = make_event(SEMTELEM_EVENT_STATE_TRANSITION, 4000U + i, 8U);
    SemTelemEmitStatus rc = sem_telem_emit_event(&ring, &ev);
    if (rc == SEMTELEM_EMIT_OK) {
      ok_count++;
    }
  }
  for (i = 0U; i < 6U; ++i) {
    SemTelemMetric m;
    SemTelemEmitStatus rc;

    memset(&m, 0, sizeof(m));
    m.meta = make_meta(5000U + i, 9U, SEMTELEM_SRC_DRIVER, 2U);
    m.meta.entity_kind = SEMTELEM_ENTITY_DMA_RING;
    m.meta.entity_id = 1U;
    m.resource_kind = 1U;
    m.resource_id = 1U;
    m.metric_id = 1U;
    m.metric_class = SEMTELEM_METRIC_CLASS_LEVEL;
    m.unit = SEMTELEM_METRIC_UNIT_SLOTS;
    m.freq_class = SEMTELEM_FREQ_HIGH;
    m.value = (int32_t)i;
    m.capacity = 32;
    m.high_watermark = (int32_t)i;
    rc = sem_telem_emit_metric(&ring, &m);
    if (rc == SEMTELEM_EMIT_OK) {
      ok_count++;
    } else if (rc == SEMTELEM_EMIT_DROP_SAMPLED) {
      sampled_count++;
    }
  }

  sem_telem_get_stats(&ring, &stats);
  check(ok_count == 25U, "expected kept count after high-frequency sampling");
  check(sampled_count == 40U, "expected sampled-drop count");
  check(sem_telem_depth(&ring) == ok_count, "depth matches kept count");
  check(stats.drop_sampled_total == sampled_count, "sampled drop stats tracked");
  check(stats.drop_ring_full_total == 0U, "no ring-full drops in sampling test");
  check(stats.high_freq_seen_total == 55U, "high frequency seen count");
  check(stats.high_freq_kept_total == 15U, "high frequency kept count");
  check(stats.low_freq_seen_total == 10U, "low frequency event count (state transitions only)");
}

static void test_snapshot_and_metric_low_frequency_full(void)
{
  SemTelemRing ring;
  SemTelemRecord storage[16];
  SemTelemSnapshot snap;
  SemTelemMetric metric;
  SemTelemRecord rec;

  check(sem_telem_ring_init(&ring, storage, 16U) == 0, "init for snapshot/metric test");

  memset(&snap, 0, sizeof(snap));
  snap.meta = make_meta(100U, 2U, SEMTELEM_SRC_KERNEL, 1U);
  snap.meta.entity_kind = SEMTELEM_ENTITY_PHASE;
  snap.meta.entity_id = 3U;
  snap.snapshot_type = SEMTELEM_SNAPSHOT_BRINGUP;
  snap.snapshot_reason = SEMTELEM_SNAPSHOT_REASON_ON_TRANSITION;
  snap.word0 = 123;
  check(sem_telem_emit_snapshot(&ring, &snap) == SEMTELEM_EMIT_OK, "snapshot emit ok");

  memset(&metric, 0, sizeof(metric));
  metric.meta = make_meta(101U, 2U, SEMTELEM_SRC_DRIVER, 4U);
  metric.meta.entity_kind = SEMTELEM_ENTITY_IRQ;
  metric.meta.entity_id = 10U;
  metric.resource_kind = 2U;
  metric.resource_id = 10U;
  metric.metric_id = 7U;
  metric.metric_class = SEMTELEM_METRIC_CLASS_COUNT;
  metric.unit = SEMTELEM_METRIC_UNIT_COUNT;
  metric.freq_class = SEMTELEM_FREQ_LOW;
  metric.value = 55;
  check(sem_telem_emit_metric(&ring, &metric) == SEMTELEM_EMIT_OK, "low frequency metric emit ok");

  check(sem_telem_try_pop(&ring, &rec) == 1, "pop snapshot");
  check(rec.kind == SEMTELEM_KIND_STATE_SNAPSHOT, "first record is snapshot");
  check(rec.payload.snapshot.snapshot_type == SEMTELEM_SNAPSHOT_BRINGUP, "snapshot type preserved");

  check(sem_telem_try_pop(&ring, &rec) == 1, "pop metric");
  check(rec.kind == SEMTELEM_KIND_RESOURCE_METRIC, "second record is metric");
  check(rec.payload.metric.metric_class == SEMTELEM_METRIC_CLASS_COUNT, "metric class preserved");
  check(rec.payload.metric.value == 55, "metric value preserved");
}

static void test_fifo_order_and_seq(void)
{
  SemTelemRing ring;
  SemTelemRecord storage[16];
  SemTelemRecord out;
  uint32_t i;

  check(sem_telem_ring_init(&ring, storage, 16U) == 0, "init for fifo test");

  for (i = 0U; i < 5U; ++i) {
    SemTelemEvent ev = make_event(SEMTELEM_EVENT_STATE_TRANSITION, 100U + i, 11U);
    ev.data0 = i;
    check(sem_telem_emit_event(&ring, &ev) == SEMTELEM_EMIT_OK, "emit low-freq event for fifo");
  }

  for (i = 0U; i < 5U; ++i) {
    check(sem_telem_try_pop(&ring, &out) == 1, "pop fifo item");
    check(out.base.seq == (i + 1U), "record seq monotonic");
    check(out.payload.event.data0 == i, "fifo preserves payload order");
  }
  check(sem_telem_try_pop(&ring, &out) == 0, "ring empty after draining");
}

static void test_pressure_path_drops_without_depth_growth(void)
{
  SemTelemRing ring;
  SemTelemRecord storage[8];
  SemTelemSamplingPolicy no_sampling;
  SemTelemStats stats;
  uint32_t i;
  uint32_t depth_before;

  check(sem_telem_ring_init(&ring, storage, 8U) == 0, "init for pressure test");
  sem_telem_sampling_policy_no_sampling(&no_sampling);
  sem_telem_ring_set_sampling_policy(&ring, &no_sampling);

  for (i = 0U; i < 8U; ++i) {
    SemTelemEvent ev = make_event(SEMTELEM_EVENT_FAULT, 1000U + i, 12U);
    check(sem_telem_emit_event(&ring, &ev) == SEMTELEM_EMIT_OK,
          "fill ring with low-freq events");
  }

  depth_before = sem_telem_depth(&ring);
  check(depth_before == 8U, "ring full before pressure loop");

  for (i = 0U; i < 200000U; ++i) {
    SemTelemEvent ev = make_event(SEMTELEM_EVENT_FAULT, 2000U + i, 13U);
    SemTelemEmitStatus rc = sem_telem_emit_event(&ring, &ev);
    if (rc != SEMTELEM_EMIT_DROP_FULL) {
      check(0, "full ring must drop immediately");
      break;
    }
  }

  sem_telem_get_stats(&ring, &stats);
  check(sem_telem_depth(&ring) == depth_before, "pressure loop does not grow depth");
  check(stats.drop_ring_full_total == 200000U, "ring-full drop counter increments under pressure");
  check(stats.drop_sampled_total == 0U, "no sampled drops in low-frequency pressure test");
  check(stats.max_depth == 8U, "max depth capped at capacity");
}

int main(void)
{
  check(sizeof(SemTelemRecord) <= 160U, "record size remains bounded for no-malloc ring");

  test_init_validation();
  test_invalid_meta_rejected();
  test_high_low_frequency_sampling();
  test_snapshot_and_metric_low_frequency_full();
  test_fifo_order_and_seq();
  test_pressure_path_drops_without_depth_growth();

  if (failures == 0) {
    printf("semantic_telemetry_host_tests: PASS\n");
    return 0;
  }

  printf("semantic_telemetry_host_tests: FAIL (%d)\n", failures);
  return 1;
}
