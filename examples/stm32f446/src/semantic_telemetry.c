#include "semantic_telemetry.h"

#include <string.h>

static int sem_telem_is_power_of_two(uint32_t v)
{
  return (v != 0U && (v & (v - 1U)) == 0U) ? 1 : 0;
}

static int sem_telem_meta_valid(const SemTelemMeta *meta)
{
  if (meta == NULL) {
    return 0;
  }
  if (meta->corr_id == 0U) {
    return 0;
  }
  if (meta->parent_span_id != 0U && meta->span_id == 0U) {
    return 0;
  }
  if (meta->src_class == 0U) {
    return 0;
  }
  return 1;
}

static uint32_t sem_telem_depth_unlocked(const SemTelemRing *ring)
{
  return ring->head_seq - ring->tail_seq;
}

static void sem_telem_lock(SemTelemRing *ring)
{
  if (ring->lock_fn != NULL) {
    ring->lock_fn(ring->lock_ctx);
  }
}

static void sem_telem_unlock(SemTelemRing *ring)
{
  if (ring->unlock_fn != NULL) {
    ring->unlock_fn(ring->lock_ctx);
  }
}

static uint16_t sem_telem_stride_or_one(uint16_t stride)
{
  return (stride == 0U) ? 1U : stride;
}

static int sem_telem_sample_keep(uint32_t *counter, uint16_t every)
{
  every = sem_telem_stride_or_one(every);
  if (every <= 1U) {
    (*counter)++;
    return 1;
  }
  (*counter)++;
  return (((*counter) % (uint32_t)every) == 0U) ? 1 : 0;
}

static int sem_telem_should_sample_event(SemTelemRing *ring,
                                         SemTelemEventType type,
                                         SemTelemFreqClass freq_class)
{
  if (freq_class != SEMTELEM_FREQ_HIGH) {
    ring->stats.low_freq_seen_total++;
    return 0;
  }

  ring->stats.high_freq_seen_total++;

  switch (type) {
    case SEMTELEM_EVENT_IRQ:
      if (!sem_telem_sample_keep(&ring->stats.irq_sample_seen, ring->sampling.irq_every)) {
        return 1;
      }
      break;
    case SEMTELEM_EVENT_DMA:
      if (!sem_telem_sample_keep(&ring->stats.dma_sample_seen, ring->sampling.dma_every)) {
        return 1;
      }
      break;
    case SEMTELEM_EVENT_DEFERRED_WORK:
      if (!sem_telem_sample_keep(&ring->stats.deferred_sample_seen,
                                 ring->sampling.deferred_work_every)) {
        return 1;
      }
      break;
    default:
      /* Classified as high, but no per-type stride configured: keep by default. */
      break;
  }

  ring->stats.high_freq_kept_total++;
  return 0;
}

static int sem_telem_should_sample_metric(SemTelemRing *ring, SemTelemFreqClass freq_class)
{
  if (freq_class != SEMTELEM_FREQ_HIGH) {
    ring->stats.low_freq_seen_total++;
    return 0;
  }

  ring->stats.high_freq_seen_total++;
  if (!sem_telem_sample_keep(&ring->stats.metric_sample_seen, ring->sampling.metric_high_every)) {
    return 1;
  }
  ring->stats.high_freq_kept_total++;
  return 0;
}

static SemTelemEmitStatus sem_telem_push_unlocked(SemTelemRing *ring, const SemTelemRecord *record)
{
  uint32_t depth = sem_telem_depth_unlocked(ring);
  uint32_t slot_index;

  if (depth >= ring->capacity) {
    ring->stats.drop_ring_full_total++;
    return SEMTELEM_EMIT_DROP_FULL;
  }

  slot_index = ring->head_seq & ring->mask;
  ring->storage[slot_index] = *record;
  ring->head_seq++;
  ring->stats.emit_ok_total++;
  depth++;
  if (depth > ring->stats.max_depth) {
    ring->stats.max_depth = depth;
  }
  return SEMTELEM_EMIT_OK;
}

void sem_telem_sampling_policy_default(SemTelemSamplingPolicy *out)
{
  if (out == NULL) {
    return;
  }
  out->irq_every = 8U;
  out->dma_every = 8U;
  out->deferred_work_every = 4U;
  out->metric_high_every = 4U;
}

void sem_telem_sampling_policy_no_sampling(SemTelemSamplingPolicy *out)
{
  if (out == NULL) {
    return;
  }
  out->irq_every = 1U;
  out->dma_every = 1U;
  out->deferred_work_every = 1U;
  out->metric_high_every = 1U;
}

int sem_telem_ring_init(SemTelemRing *ring, SemTelemRecord *storage, uint32_t capacity)
{
  if (ring == NULL || storage == NULL || !sem_telem_is_power_of_two(capacity)) {
    return -1;
  }

  memset(ring, 0, sizeof(*ring));
  ring->storage = storage;
  ring->capacity = capacity;
  ring->mask = capacity - 1U;
  ring->next_record_seq = 1U;
  sem_telem_sampling_policy_default(&ring->sampling);
  return 0;
}

void sem_telem_ring_reset(SemTelemRing *ring)
{
  if (ring == NULL) {
    return;
  }

  sem_telem_lock(ring);
  ring->head_seq = 0U;
  ring->tail_seq = 0U;
  ring->next_record_seq = 1U;
  memset(&ring->stats, 0, sizeof(ring->stats));
  sem_telem_unlock(ring);
}

void sem_telem_ring_set_lock_hooks(SemTelemRing *ring,
                                   SemTelemLockFn lock_fn,
                                   SemTelemLockFn unlock_fn,
                                   void *ctx)
{
  if (ring == NULL) {
    return;
  }
  ring->lock_fn = lock_fn;
  ring->unlock_fn = unlock_fn;
  ring->lock_ctx = ctx;
}

void sem_telem_ring_set_sampling_policy(SemTelemRing *ring, const SemTelemSamplingPolicy *policy)
{
  if (ring == NULL || policy == NULL) {
    return;
  }

  sem_telem_lock(ring);
  ring->sampling = *policy;
  if (ring->sampling.irq_every == 0U) {
    ring->sampling.irq_every = 1U;
  }
  if (ring->sampling.dma_every == 0U) {
    ring->sampling.dma_every = 1U;
  }
  if (ring->sampling.deferred_work_every == 0U) {
    ring->sampling.deferred_work_every = 1U;
  }
  if (ring->sampling.metric_high_every == 0U) {
    ring->sampling.metric_high_every = 1U;
  }
  sem_telem_unlock(ring);
}

SemTelemFreqClass sem_telem_event_default_freq(SemTelemEventType type)
{
  switch (type) {
    case SEMTELEM_EVENT_IRQ:
    case SEMTELEM_EVENT_DMA:
    case SEMTELEM_EVENT_DEFERRED_WORK:
      return SEMTELEM_FREQ_HIGH;
    default:
      return SEMTELEM_FREQ_LOW;
  }
}

SemTelemEmitStatus sem_telem_emit_event(SemTelemRing *ring, const SemTelemEvent *event)
{
  SemTelemRecord rec;
  SemTelemFreqClass freq;
  SemTelemEmitStatus rc;

  if (ring == NULL || event == NULL || !sem_telem_meta_valid(&event->meta)) {
    return SEMTELEM_EMIT_BAD_ARG;
  }

  freq = sem_telem_event_default_freq(event->type);

  memset(&rec, 0, sizeof(rec));
  rec.kind = SEMTELEM_KIND_EVENT;
  rec.base.meta = event->meta;
  rec.payload.event.type = event->type;
  rec.payload.event.severity = event->severity;
  rec.payload.event.freq_class = freq;
  rec.payload.event.state_from = event->state_from;
  rec.payload.event.state_to = event->state_to;
  rec.payload.event.code = event->code;
  rec.payload.event.data0 = event->data0;
  rec.payload.event.data1 = event->data1;
  rec.payload.event.data2 = event->data2;
  rec.payload.event.data3 = event->data3;

  sem_telem_lock(ring);
  ring->stats.emit_attempt_total++;
  if (sem_telem_should_sample_event(ring, event->type, freq)) {
    ring->stats.drop_sampled_total++;
    sem_telem_unlock(ring);
    return SEMTELEM_EMIT_DROP_SAMPLED;
  }
  rec.base.seq = ring->next_record_seq++;
  rc = sem_telem_push_unlocked(ring, &rec);
  sem_telem_unlock(ring);
  return rc;
}

SemTelemEmitStatus sem_telem_emit_snapshot(SemTelemRing *ring, const SemTelemSnapshot *snapshot)
{
  SemTelemRecord rec;
  SemTelemEmitStatus rc;

  if (ring == NULL || snapshot == NULL || !sem_telem_meta_valid(&snapshot->meta)) {
    return SEMTELEM_EMIT_BAD_ARG;
  }

  memset(&rec, 0, sizeof(rec));
  rec.kind = SEMTELEM_KIND_STATE_SNAPSHOT;
  rec.base.meta = snapshot->meta;
  rec.payload.snapshot.snapshot_type = snapshot->snapshot_type;
  rec.payload.snapshot.snapshot_reason = snapshot->snapshot_reason;
  rec.payload.snapshot.word0 = snapshot->word0;
  rec.payload.snapshot.word1 = snapshot->word1;
  rec.payload.snapshot.word2 = snapshot->word2;
  rec.payload.snapshot.word3 = snapshot->word3;
  rec.payload.snapshot.word4 = snapshot->word4;
  rec.payload.snapshot.word5 = snapshot->word5;

  sem_telem_lock(ring);
  ring->stats.emit_attempt_total++;
  ring->stats.low_freq_seen_total++;
  rec.base.seq = ring->next_record_seq++;
  rc = sem_telem_push_unlocked(ring, &rec);
  sem_telem_unlock(ring);
  return rc;
}

SemTelemEmitStatus sem_telem_emit_metric(SemTelemRing *ring, const SemTelemMetric *metric)
{
  SemTelemRecord rec;
  SemTelemEmitStatus rc;

  if (ring == NULL || metric == NULL || !sem_telem_meta_valid(&metric->meta)) {
    return SEMTELEM_EMIT_BAD_ARG;
  }

  memset(&rec, 0, sizeof(rec));
  rec.kind = SEMTELEM_KIND_RESOURCE_METRIC;
  rec.base.meta = metric->meta;
  rec.payload.metric.resource_kind = metric->resource_kind;
  rec.payload.metric.resource_id = metric->resource_id;
  rec.payload.metric.metric_id = metric->metric_id;
  rec.payload.metric.metric_class = metric->metric_class;
  rec.payload.metric.unit = metric->unit;
  rec.payload.metric.freq_class = metric->freq_class;
  rec.payload.metric.value = metric->value;
  rec.payload.metric.capacity = metric->capacity;
  rec.payload.metric.high_watermark = metric->high_watermark;
  rec.payload.metric.window_us = metric->window_us;
  rec.payload.metric.data0 = metric->data0;
  rec.payload.metric.data1 = metric->data1;

  sem_telem_lock(ring);
  ring->stats.emit_attempt_total++;
  if (sem_telem_should_sample_metric(ring, metric->freq_class)) {
    ring->stats.drop_sampled_total++;
    sem_telem_unlock(ring);
    return SEMTELEM_EMIT_DROP_SAMPLED;
  }
  rec.base.seq = ring->next_record_seq++;
  rc = sem_telem_push_unlocked(ring, &rec);
  sem_telem_unlock(ring);
  return rc;
}

int sem_telem_try_pop(SemTelemRing *ring, SemTelemRecord *out_record)
{
  uint32_t slot_index;

  if (ring == NULL || out_record == NULL) {
    return 0;
  }

  sem_telem_lock(ring);
  if (ring->head_seq == ring->tail_seq) {
    sem_telem_unlock(ring);
    return 0;
  }

  slot_index = ring->tail_seq & ring->mask;
  *out_record = ring->storage[slot_index];
  ring->tail_seq++;
  sem_telem_unlock(ring);
  return 1;
}

uint32_t sem_telem_depth(const SemTelemRing *ring)
{
  if (ring == NULL) {
    return 0U;
  }
  return ring->head_seq - ring->tail_seq;
}

uint32_t sem_telem_capacity(const SemTelemRing *ring)
{
  if (ring == NULL) {
    return 0U;
  }
  return ring->capacity;
}

void sem_telem_get_stats(const SemTelemRing *ring, SemTelemStats *out_stats)
{
  if (ring == NULL || out_stats == NULL) {
    return;
  }
  *out_stats = ring->stats;
}
