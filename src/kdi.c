#include "kdi.h"
#include "seam_agent.h"

#include <string.h>

#define KDI_MPU_MIN_SIZE 32U
#define KDI_HW_MPU_REGION_COUNT 8U
#define KDI_TOKEN_TTL_NEVER 0U
#define KDI_TOKEN_TTL_DEFAULT_MS 120000U
#define KDI_DEFERRED_QUEUE_CAP 32U
#define KDI_IRQ_STARVATION_DEFAULT_MS 250U
#define KDI_IRQ_COOLDOWN_BASE_DEFAULT_MS 100U
#define KDI_IRQ_COOLDOWN_MAX_DEFAULT_MS 1600U
#define KDI_DMA_RING_CAP 16U
#define KDI_DMA_BUFFER_CAP 32U

typedef struct {
  KdiPolicy policy;
  const char *name;
} KdiPolicyEntry;

typedef struct {
  uint8_t driver;
  uint16_t work_id;
  uint32_t arg;
  uint32_t queued_ms;
} KdiDeferredWork;

typedef enum {
  KDI_DMA_BUF_FREE = 0,
  KDI_DMA_BUF_IDLE = 1,
  KDI_DMA_BUF_RX_POSTED = 2,
  KDI_DMA_BUF_RX_READY = 3,
  KDI_DMA_BUF_TX_PENDING = 4,
  KDI_DMA_BUF_TX_DONE = 5
} KdiDmaBufferState;

typedef struct {
  uint8_t used;
  uint8_t owner;
  uint8_t align;
  uint8_t direction;
  uintptr_t base;
  uint32_t size;
  uint16_t last_bytes;
  uint8_t state;
} KdiDmaBuffer;

typedef struct {
  uint16_t slots[KDI_DMA_RING_CAP];
  uint16_t head;
  uint16_t tail;
  uint16_t count;
  uint16_t depth;
} KdiDmaRing;

typedef struct {
  KdiDmaBuffer buffers[KDI_DMA_BUFFER_CAP];
  KdiDmaRing rx_posted;
  KdiDmaRing rx_ready;
  KdiDmaRing tx_pending;
  KdiDmaRing tx_done;
  KdiDmaStats stats;
} KdiDmaDriverState;

typedef struct {
  uint8_t isolated;
  uint8_t active_fault;
  uint16_t last_code;
  uint32_t last_detail;
  uint32_t fault_total;
  uint32_t contain_total;
  uint32_t crash_total;
  uint32_t restart_total;
  uint32_t generation;
  uint32_t last_fault_ms;
} KdiFaultDomainState;

static const KdiPolicyEntry kdi_policy_table[KDI_DRIVER_COUNT] = {
  {
    .policy = {
      .allow_mpu = 1U,
      .allow_irq = 1U,
      .allow_dma = 1U,
      .allow_fault = 1U,
      .allow_power = 1U,
      .allow_reset = 1U,
      .max_mpu_regions = KDI_HW_MPU_REGION_COUNT,
      .min_irqn = -15,
      .max_irqn = 95,
      .max_dma_bytes = 4096U,
    },
    .name = "kernel",
  },
  {
    .policy = {
      .allow_mpu = 0U,
      .allow_irq = 1U,
      .allow_dma = 1U,
      .allow_fault = 1U,
      .allow_power = 1U,
      .allow_reset = 0U,
      .max_mpu_regions = 0U,
      .min_irqn = 0,
      .max_irqn = 95,
      .max_dma_bytes = 1024U,
    },
    .name = "uart",
  },
  {
    .policy = {
      .allow_mpu = 0U,
      .allow_irq = 1U,
      .allow_dma = 1U,
      .allow_fault = 1U,
      .allow_power = 1U,
      .allow_reset = 0U,
      .max_mpu_regions = 0U,
      .min_irqn = 0,
      .max_irqn = 95,
      .max_dma_bytes = 2048U,
    },
    .name = "sensor",
  },
  {
    .policy = {
      .allow_mpu = 1U,
      .allow_irq = 0U,
      .allow_dma = 0U,
      .allow_fault = 1U,
      .allow_power = 0U,
      .allow_reset = 0U,
      .max_mpu_regions = 4U,
      .min_irqn = 0,
      .max_irqn = -1,
      .max_dma_bytes = 0U,
    },
    .name = "vm-runtime",
  },
  {
    .policy = {
      .allow_mpu = 0U,
      .allow_irq = 0U,
      .allow_dma = 0U,
      .allow_fault = 1U,
      .allow_power = 1U,
      .allow_reset = 1U,
      .max_mpu_regions = 0U,
      .min_irqn = 0,
      .max_irqn = -1,
      .max_dma_bytes = 0U,
    },
    .name = "diag",
  },
};

static KdiDecision kdi_last;
static KdiStats kdi_stats;
static uint8_t kdi_fault_valid;
static KdiDriverId kdi_fault_driver;
static KdiFaultReport kdi_fault_last;
static KdiCapToken kdi_tokens[KDI_DRIVER_COUNT];
static uint8_t kdi_token_active[KDI_DRIVER_COUNT];
static uint32_t kdi_token_issued_ms[KDI_DRIVER_COUNT];
static uint32_t kdi_token_ttl_ms[KDI_DRIVER_COUNT];
static KdiDriverState kdi_driver_state[KDI_DRIVER_COUNT];
static uint32_t kdi_profile_req_total[KDI_DRIVER_COUNT][KDI_PROFILE_REQ_COUNT];
static uint32_t kdi_profile_req_fail[KDI_DRIVER_COUNT][KDI_PROFILE_REQ_COUNT];
static uint32_t kdi_profile_req_first_ms[KDI_DRIVER_COUNT][KDI_PROFILE_REQ_COUNT];
static uint32_t kdi_profile_req_last_ms[KDI_DRIVER_COUNT][KDI_PROFILE_REQ_COUNT];
static uint8_t kdi_profile_req_seen[KDI_DRIVER_COUNT][KDI_PROFILE_REQ_COUNT];
static uint32_t kdi_profile_state_visit[KDI_DRIVER_COUNT][KDI_PROFILE_STATE_COUNT];
static uint32_t kdi_profile_state_transition[KDI_DRIVER_COUNT][KDI_PROFILE_STATE_COUNT][KDI_PROFILE_STATE_COUNT];
static uint32_t kdi_profile_window_start_ms;
static uint8_t kdi_irq_in_handler[KDI_DRIVER_COUNT];
static uint8_t kdi_irq_throttled[KDI_DRIVER_COUNT];
static uint32_t kdi_irq_budget_per_sec[KDI_DRIVER_COUNT];
static uint32_t kdi_irq_window_start_ms[KDI_DRIVER_COUNT];
static uint32_t kdi_irq_window_count[KDI_DRIVER_COUNT];
static uint32_t kdi_irq_cooldown_base_ms[KDI_DRIVER_COUNT];
static uint32_t kdi_irq_cooldown_max_ms[KDI_DRIVER_COUNT];
static uint32_t kdi_irq_cooldown_until_ms[KDI_DRIVER_COUNT];
static uint8_t kdi_irq_cooldown_level[KDI_DRIVER_COUNT];
static KdiIrqStats kdi_irq_stats;
static KdiIrqDriverCounters kdi_irq_driver_counters[KDI_DRIVER_COUNT];
static uint32_t kdi_irq_starvation_ms;
static KdiDeferredWork kdi_deferred_queue[KDI_DEFERRED_QUEUE_CAP];
static uint8_t kdi_deferred_head;
static uint8_t kdi_deferred_tail;
static uint8_t kdi_deferred_count;
static KdiIrqWorkFn kdi_irq_worker_fn[KDI_DRIVER_COUNT];
static void *kdi_irq_worker_ctx[KDI_DRIVER_COUNT];
static KdiDmaDriverState kdi_dma_state[KDI_DRIVER_COUNT];
static KdiFaultDomainState kdi_fault_domain[KDI_DRIVER_COUNT];
static KdiNowMsFn kdi_now_ms_fn;
static uint32_t kdi_now_ms_soft;
static uint32_t kdi_token_epoch = 0x7C4A7A99U;

static const uint32_t kdi_token_ttl_default_ms[KDI_DRIVER_COUNT] = {
  KDI_TOKEN_TTL_NEVER,     /* kernel */
  KDI_TOKEN_TTL_DEFAULT_MS,/* uart */
  KDI_TOKEN_TTL_DEFAULT_MS,/* sensor */
  KDI_TOKEN_TTL_DEFAULT_MS,/* vm-runtime */
  KDI_TOKEN_TTL_DEFAULT_MS /* diag */
};

static const uint32_t kdi_irq_budget_default_per_sec[KDI_DRIVER_COUNT] = {
  0U,   /* kernel: unlimited */
  500U, /* uart */
  400U, /* sensor */
  300U, /* vm-runtime */
  200U  /* diag */
};

static const uint32_t kdi_irq_cooldown_base_default_ms[KDI_DRIVER_COUNT] = {
  0U,                                /* kernel */
  KDI_IRQ_COOLDOWN_BASE_DEFAULT_MS,  /* uart */
  KDI_IRQ_COOLDOWN_BASE_DEFAULT_MS,  /* sensor */
  KDI_IRQ_COOLDOWN_BASE_DEFAULT_MS,  /* vm-runtime */
  KDI_IRQ_COOLDOWN_BASE_DEFAULT_MS   /* diag */
};

static const uint32_t kdi_irq_cooldown_max_default_ms[KDI_DRIVER_COUNT] = {
  0U,                               /* kernel */
  KDI_IRQ_COOLDOWN_MAX_DEFAULT_MS,  /* uart */
  KDI_IRQ_COOLDOWN_MAX_DEFAULT_MS,  /* sensor */
  KDI_IRQ_COOLDOWN_MAX_DEFAULT_MS,  /* vm-runtime */
  KDI_IRQ_COOLDOWN_MAX_DEFAULT_MS   /* diag */
};

static void kdi_dma_driver_reset(KdiDriverId driver);

static int kdi_is_pow2(uint32_t value)
{
  if (value == 0U) {
    return 0;
  }
  return ((value & (value - 1U)) == 0U) ? 1 : 0;
}

static int kdi_driver_valid(KdiDriverId driver)
{
  return ((uint32_t)driver < (uint32_t)KDI_DRIVER_COUNT) ? 1 : 0;
}

static int kdi_profile_req_valid(KdiRequestType req)
{
  return (req >= KDI_REQ_MPU && req <= KDI_REQ_RESET) ? 1 : 0;
}

static int kdi_profile_state_valid(KdiDriverState state)
{
  return (state <= KDI_STATE_DEAD) ? 1 : 0;
}

static void kdi_profile_state_visit_record(KdiDriverId driver, KdiDriverState state)
{
  if (!kdi_driver_valid(driver) || !kdi_profile_state_valid(state)) {
    return;
  }
  kdi_profile_state_visit[driver][state]++;
}

static void kdi_driver_set_state(KdiDriverId driver, KdiDriverState next_state)
{
  KdiDriverState prev_state;

  if (!kdi_driver_valid(driver) || !kdi_profile_state_valid(next_state)) {
    return;
  }

  prev_state = kdi_driver_state[driver];
  if (kdi_profile_state_valid(prev_state) &&
      prev_state != next_state) {
    kdi_profile_state_transition[driver][prev_state][next_state]++;
  }
  kdi_driver_state[driver] = next_state;
  kdi_profile_state_visit_record(driver, next_state);
  seam_emit(CFL_LAYER_KDI, CFL_EV_KDI_STATE,
            (uint32_t)driver, (uint32_t)prev_state, (uint32_t)next_state, 0);
}

static int kdi_state_allows_request(KdiDriverId driver)
{
  KdiDriverState state;

  if (!kdi_driver_valid(driver)) {
    return 0;
  }
  state = kdi_driver_state[driver];
  return (state == KDI_STATE_PROBE || state == KDI_STATE_READY || state == KDI_STATE_ACTIVE) ? 1 : 0;
}

static int kdi_state_allows_fault_report(KdiDriverId driver)
{
  KdiDriverState state;

  if (!kdi_driver_valid(driver)) {
    return 0;
  }
  state = kdi_driver_state[driver];
  return (state == KDI_STATE_PROBE ||
          state == KDI_STATE_READY ||
          state == KDI_STATE_ACTIVE ||
          state == KDI_STATE_ERROR) ? 1 : 0;
}

static uint32_t kdi_now_ms(void)
{
  if (kdi_now_ms_fn != NULL) {
    return kdi_now_ms_fn();
  }
  return kdi_now_ms_soft;
}

static uint32_t kdi_mix32(uint32_t value)
{
  value ^= value >> 16;
  value *= 0x7FEB352DU;
  value ^= value >> 15;
  value *= 0x846CA68BU;
  value ^= value >> 16;
  return value;
}

static KdiCapToken kdi_make_token(KdiDriverId driver)
{
  uint32_t v = kdi_token_epoch ^ ((uint32_t)driver + 1U) * 0x9E3779B9U;
  v = kdi_mix32(v);
  if (v == (uint32_t)KDI_CAP_INVALID) {
    v = 0xA5A5A5A5U ^ ((uint32_t)driver << 24);
  }
  return (KdiCapToken)v;
}

static int kdi_expire_if_needed(KdiDriverId driver)
{
  uint32_t ttl;
  uint32_t elapsed;

  if (!kdi_driver_valid(driver) || kdi_token_active[driver] == 0U) {
    return 0;
  }

  ttl = kdi_token_ttl_ms[driver];
  if (ttl == KDI_TOKEN_TTL_NEVER) {
    return 0;
  }

  elapsed = kdi_now_ms() - kdi_token_issued_ms[driver];
  if (elapsed < ttl) {
    return 0;
  }

  kdi_token_active[driver] = 0U;
  kdi_tokens[driver] = KDI_CAP_INVALID;
  kdi_stats.token_expire_total++;
  seam_emit(CFL_LAYER_KDI, CFL_EV_KDI_TOKEN_EXP,
            (uint32_t)driver, elapsed, 0, 0);
  return 1;
}

static int kdi_token_valid(KdiDriverId driver, KdiCapToken token)
{
  if (!kdi_driver_valid(driver) || token == KDI_CAP_INVALID) {
    return 0;
  }
  (void)kdi_expire_if_needed(driver);
  if (kdi_token_active[driver] == 0U) {
    return 0;
  }
  return (token == kdi_tokens[driver]) ? 1 : 0;
}

static int kdi_ms_reached(uint32_t now, uint32_t deadline)
{
  return ((int32_t)(now - deadline) >= 0) ? 1 : 0;
}

static uint32_t kdi_irq_cooldown_duration_ms(KdiDriverId driver)
{
  uint32_t base;
  uint32_t max_ms;
  uint32_t dur;

  if (!kdi_driver_valid(driver)) {
    return 0U;
  }

  base = kdi_irq_cooldown_base_ms[driver];
  if (base == 0U) {
    return 0U;
  }
  max_ms = kdi_irq_cooldown_max_ms[driver];
  if (max_ms == 0U || max_ms < base) {
    max_ms = base;
  }

  dur = base;
  for (uint8_t i = 0U; i < kdi_irq_cooldown_level[driver]; ++i) {
    if (dur >= (max_ms / 2U)) {
      dur = max_ms;
      break;
    }
    dur *= 2U;
  }
  if (dur > max_ms) {
    dur = max_ms;
  }
  return dur;
}

static void kdi_irq_clear_throttle(KdiDriverId driver, uint8_t count_recover)
{
  if (!kdi_driver_valid(driver)) {
    return;
  }
  if (kdi_irq_throttled[driver] != 0U && count_recover != 0U) {
    kdi_irq_stats.irq_recover_total++;
  }
  kdi_irq_throttled[driver] = 0U;
  kdi_irq_cooldown_until_ms[driver] = 0U;
  if (kdi_irq_cooldown_level[driver] > 0U) {
    kdi_irq_cooldown_level[driver]--;
  }
  kdi_irq_window_start_ms[driver] = kdi_now_ms();
  kdi_irq_window_count[driver] = 0U;
}

static void kdi_irq_recover_if_needed(KdiDriverId driver)
{
  uint32_t now;

  if (!kdi_driver_valid(driver) || kdi_irq_throttled[driver] == 0U) {
    return;
  }
  if (kdi_irq_cooldown_until_ms[driver] == 0U) {
    return;
  }
  now = kdi_now_ms();
  if (kdi_ms_reached(now, kdi_irq_cooldown_until_ms[driver])) {
    kdi_irq_clear_throttle(driver, 1U);
  }
}

static void kdi_irq_reset_window_if_needed(KdiDriverId driver)
{
  uint32_t now;

  if (!kdi_driver_valid(driver)) {
    return;
  }
  kdi_irq_recover_if_needed(driver);
  now = kdi_now_ms();
  if ((now - kdi_irq_window_start_ms[driver]) < 1000U) {
    return;
  }
  kdi_irq_window_start_ms[driver] = now;
  kdi_irq_window_count[driver] = 0U;
  if (kdi_irq_throttled[driver] != 0U && kdi_irq_cooldown_until_ms[driver] == 0U) {
    kdi_irq_clear_throttle(driver, 1U);
  } else if (kdi_irq_throttled[driver] == 0U && kdi_irq_cooldown_level[driver] > 0U) {
    kdi_irq_cooldown_level[driver]--;
  }
}

static uint32_t kdi_deferred_pending_for_driver(KdiDriverId driver)
{
  uint32_t pending = 0U;

  for (uint32_t i = 0U; i < kdi_deferred_count; ++i) {
    uint32_t idx = ((uint32_t)kdi_deferred_tail + i) % KDI_DEFERRED_QUEUE_CAP;
    if ((KdiDriverId)kdi_deferred_queue[idx].driver == driver) {
      pending++;
    }
  }
  return pending;
}

static void kdi_drop_deferred_for_driver(KdiDriverId driver)
{
  KdiDeferredWork kept[KDI_DEFERRED_QUEUE_CAP];
  uint8_t kept_count = 0U;

  if (!kdi_driver_valid(driver) || kdi_deferred_count == 0U) {
    return;
  }

  memset(kept, 0, sizeof(kept));
  for (uint32_t i = 0U; i < kdi_deferred_count; ++i) {
    uint32_t idx = ((uint32_t)kdi_deferred_tail + i) % KDI_DEFERRED_QUEUE_CAP;
    KdiDeferredWork work = kdi_deferred_queue[idx];
    if ((KdiDriverId)work.driver == driver) {
      kdi_irq_stats.irq_drop_total++;
      kdi_irq_driver_counters[driver].irq_drop_total++;
      continue;
    }
    if (kept_count < KDI_DEFERRED_QUEUE_CAP) {
      kept[kept_count++] = work;
    }
  }

  memset(kdi_deferred_queue, 0, sizeof(kdi_deferred_queue));
  for (uint8_t i = 0U; i < kept_count; ++i) {
    kdi_deferred_queue[i] = kept[i];
  }
  kdi_deferred_tail = 0U;
  kdi_deferred_head = kept_count % KDI_DEFERRED_QUEUE_CAP;
  kdi_deferred_count = kept_count;
}

static void kdi_fault_domain_record(KdiDriverId driver, uint16_t code, uint32_t detail)
{
  KdiFaultDomainState *d;

  if (!kdi_driver_valid(driver) || code == 0U) {
    return;
  }
  d = &kdi_fault_domain[driver];
  d->active_fault = 1U;
  d->last_code = code;
  d->last_detail = detail;
  d->last_fault_ms = kdi_now_ms();
  d->fault_total++;
}

static void kdi_contain_driver_fault(KdiDriverId driver)
{
  KdiFaultDomainState *domain;

  if (!kdi_driver_valid(driver) || driver == KDI_DRIVER_KERNEL) {
    return;
  }

  domain = &kdi_fault_domain[driver];
  if (domain->isolated == 0U) {
    domain->contain_total++;
  }
  domain->isolated = 1U;

  kdi_irq_in_handler[driver] = 0U;
  kdi_irq_throttled[driver] = 0U;
  kdi_irq_cooldown_until_ms[driver] = 0U;
  kdi_irq_cooldown_level[driver] = 0U;
  kdi_irq_window_count[driver] = 0U;
  kdi_irq_window_start_ms[driver] = kdi_now_ms();
  kdi_irq_worker_fn[driver] = NULL;
  kdi_irq_worker_ctx[driver] = NULL;
  kdi_drop_deferred_for_driver(driver);
  kdi_dma_driver_reset(driver);
}

static void kdi_dma_ring_reset(KdiDmaRing *ring, uint16_t depth)
{
  if (ring == NULL) {
    return;
  }
  memset(ring->slots, 0, sizeof(ring->slots));
  ring->head = 0U;
  ring->tail = 0U;
  ring->count = 0U;
  ring->depth = (depth == 0U || depth > KDI_DMA_RING_CAP) ? KDI_DMA_RING_CAP : depth;
}

static int kdi_dma_ring_push(KdiDmaRing *ring, uint16_t buffer_id)
{
  if (ring == NULL || ring->depth == 0U) {
    return KDI_ERR_BAD_ARG;
  }
  if (ring->count >= ring->depth) {
    return KDI_ERR_LIMIT;
  }
  ring->slots[ring->head] = buffer_id;
  ring->head = (uint16_t)((ring->head + 1U) % ring->depth);
  ring->count++;
  return KDI_OK;
}

static int kdi_dma_ring_pop(KdiDmaRing *ring, uint16_t *out_buffer_id)
{
  if (ring == NULL || ring->depth == 0U || out_buffer_id == NULL) {
    return KDI_ERR_BAD_ARG;
  }
  if (ring->count == 0U) {
    return KDI_ERR_STATE;
  }
  *out_buffer_id = ring->slots[ring->tail];
  ring->tail = (uint16_t)((ring->tail + 1U) % ring->depth);
  ring->count--;
  return KDI_OK;
}

static void kdi_dma_driver_reset(KdiDriverId driver)
{
  KdiDmaDriverState *d;

  if (!kdi_driver_valid(driver)) {
    return;
  }
  d = &kdi_dma_state[driver];
  memset(d, 0, sizeof(*d));
  kdi_dma_ring_reset(&d->rx_posted, KDI_DMA_RING_CAP);
  kdi_dma_ring_reset(&d->rx_ready, KDI_DMA_RING_CAP);
  kdi_dma_ring_reset(&d->tx_pending, KDI_DMA_RING_CAP);
  kdi_dma_ring_reset(&d->tx_done, KDI_DMA_RING_CAP);
}

static int kdi_dma_runtime_allowed(KdiDriverId driver)
{
  if (!kdi_driver_valid(driver)) {
    return 0;
  }
  return (kdi_driver_state[driver] == KDI_STATE_ACTIVE) ? 1 : 0;
}

static int kdi_dma_policy_allowed(KdiDriverId driver)
{
  if (!kdi_driver_valid(driver)) {
    return 0;
  }
  return (kdi_policy_table[driver].policy.allow_dma != 0U) ? 1 : 0;
}

static int kdi_dma_validate_declare(KdiDriverId driver,
                                    KdiCapToken token,
                                    const KdiDmaRequest *req,
                                    const KdiPolicy **out_policy,
                                    uint8_t *out_align)
{
  const KdiPolicy *policy;
  uint8_t align;

  if (!kdi_driver_valid(driver) || req == NULL) {
    return KDI_ERR_BAD_ARG;
  }
  if (!kdi_token_valid(driver, token)) {
    return KDI_ERR_AUTH;
  }
  if (!kdi_state_allows_request(driver)) {
    return KDI_ERR_STATE;
  }
  policy = &kdi_policy_table[driver].policy;
  if (policy->allow_dma == 0U) {
    return KDI_ERR_DENIED;
  }
  if (req->size == 0U || req->size > policy->max_dma_bytes) {
    return KDI_ERR_LIMIT;
  }
  align = (req->align == 0U) ? 4U : req->align;
  if (!kdi_is_pow2(align) || align > 64U) {
    return KDI_ERR_BAD_ARG;
  }
  if (((uint32_t)req->base & (align - 1U)) != 0U) {
    return KDI_ERR_BAD_ARG;
  }
  if (out_policy != NULL) {
    *out_policy = policy;
  }
  if (out_align != NULL) {
    *out_align = align;
  }
  return KDI_OK;
}

static KdiDmaBuffer *kdi_dma_buffer_get(KdiDriverId driver, uint16_t buffer_id)
{
  if (!kdi_driver_valid(driver) || buffer_id >= KDI_DMA_BUFFER_CAP) {
    return NULL;
  }
  if (kdi_dma_state[driver].buffers[buffer_id].used == 0U) {
    return NULL;
  }
  return &kdi_dma_state[driver].buffers[buffer_id];
}

static int kdi_dma_token_or_kernel_valid(KdiDriverId driver, KdiCapToken token)
{
  if (kdi_token_valid(driver, token)) {
    return 1;
  }
  return kdi_token_valid(KDI_DRIVER_KERNEL, token);
}

static int kdi_dma_driver_busy(KdiDriverId driver)
{
  KdiDmaDriverState *d;

  if (!kdi_driver_valid(driver)) {
    return 1;
  }
  d = &kdi_dma_state[driver];
  if (d->rx_posted.count != 0U || d->rx_ready.count != 0U ||
      d->tx_pending.count != 0U || d->tx_done.count != 0U) {
    return 1;
  }
  for (uint16_t i = 0U; i < KDI_DMA_BUFFER_CAP; ++i) {
    if (d->buffers[i].used == 0U) {
      continue;
    }
    if (d->buffers[i].owner != KDI_DMA_OWNER_DRIVER || d->buffers[i].state != KDI_DMA_BUF_IDLE) {
      return 1;
    }
  }
  return 0;
}

static void kdi_dma_fill_leak_report(KdiDriverId driver, KdiDmaLeakReport *out_report)
{
  const KdiDmaDriverState *d;

  if (out_report == NULL) {
    return;
  }
  memset(out_report, 0, sizeof(*out_report));
  if (!kdi_driver_valid(driver)) {
    return;
  }

  d = &kdi_dma_state[driver];
  for (uint16_t i = 0U; i < KDI_DMA_BUFFER_CAP; ++i) {
    const KdiDmaBuffer *b = &d->buffers[i];
    if (b->used == 0U) {
      continue;
    }
    out_report->total_buffers++;
    if (b->owner == KDI_DMA_OWNER_DRIVER) {
      out_report->driver_owned++;
    } else {
      out_report->kernel_owned++;
    }
    switch ((KdiDmaBufferState)b->state) {
      case KDI_DMA_BUF_RX_POSTED:
        out_report->rx_posted++;
        break;
      case KDI_DMA_BUF_RX_READY:
        out_report->rx_ready++;
        break;
      case KDI_DMA_BUF_TX_PENDING:
        out_report->tx_pending++;
        break;
      case KDI_DMA_BUF_TX_DONE:
        out_report->tx_done++;
        break;
      default:
        break;
    }
  }

  out_report->leak = (out_report->kernel_owned != 0U ||
                      out_report->rx_posted != 0U ||
                      out_report->rx_ready != 0U ||
                      out_report->tx_pending != 0U ||
                      out_report->tx_done != 0U) ? 1U : 0U;
}

static int kdi_finish(KdiDriverId driver, KdiRequestType req, int rc, uint32_t arg0, uint32_t arg1)
{
  kdi_last.valid = 1U;
  kdi_last.driver = (uint8_t)driver;
  kdi_last.req = (uint8_t)req;
  kdi_last.rc = rc;
  kdi_last.arg0 = arg0;
  kdi_last.arg1 = arg1;

  if (kdi_driver_valid(driver) &&
      kdi_profile_req_valid(req)) {
    uint32_t req_idx = (uint32_t)req;
    uint32_t now_ms = kdi_now_ms();

    kdi_profile_req_total[driver][req_idx]++;
    if (kdi_profile_req_seen[driver][req_idx] == 0U) {
      kdi_profile_req_seen[driver][req_idx] = 1U;
      kdi_profile_req_first_ms[driver][req_idx] = now_ms;
    }
    kdi_profile_req_last_ms[driver][req_idx] = now_ms;
    if (rc != KDI_OK) {
      kdi_profile_req_fail[driver][req_idx]++;
    }
  }

  if (rc == KDI_OK) {
    kdi_stats.allow_total++;
  } else if (rc == KDI_ERR_DENIED || rc == KDI_ERR_AUTH) {
    kdi_stats.deny_total++;
    if (rc == KDI_ERR_AUTH) {
      kdi_stats.auth_fail_total++;
    }
  } else {
    kdi_stats.reject_total++;
    if (rc == KDI_ERR_STATE) {
      kdi_stats.state_fail_total++;
    }
  }

  return rc;
}

void kdi_init(void)
{
  memset(&kdi_last, 0, sizeof(kdi_last));
  memset(&kdi_stats, 0, sizeof(kdi_stats));
  memset(&kdi_fault_last, 0, sizeof(kdi_fault_last));
  memset(&kdi_tokens, 0, sizeof(kdi_tokens));
  memset(&kdi_token_active, 0, sizeof(kdi_token_active));
  memset(&kdi_token_issued_ms, 0, sizeof(kdi_token_issued_ms));
  memset(&kdi_token_ttl_ms, 0, sizeof(kdi_token_ttl_ms));
  memset(&kdi_driver_state, 0, sizeof(kdi_driver_state));
  memset(&kdi_profile_req_total, 0, sizeof(kdi_profile_req_total));
  memset(&kdi_profile_req_fail, 0, sizeof(kdi_profile_req_fail));
  memset(&kdi_profile_req_first_ms, 0, sizeof(kdi_profile_req_first_ms));
  memset(&kdi_profile_req_last_ms, 0, sizeof(kdi_profile_req_last_ms));
  memset(&kdi_profile_req_seen, 0, sizeof(kdi_profile_req_seen));
  memset(&kdi_profile_state_visit, 0, sizeof(kdi_profile_state_visit));
  memset(&kdi_profile_state_transition, 0, sizeof(kdi_profile_state_transition));
  memset(&kdi_irq_in_handler, 0, sizeof(kdi_irq_in_handler));
  memset(&kdi_irq_throttled, 0, sizeof(kdi_irq_throttled));
  memset(&kdi_irq_budget_per_sec, 0, sizeof(kdi_irq_budget_per_sec));
  memset(&kdi_irq_window_start_ms, 0, sizeof(kdi_irq_window_start_ms));
  memset(&kdi_irq_window_count, 0, sizeof(kdi_irq_window_count));
  memset(&kdi_irq_cooldown_base_ms, 0, sizeof(kdi_irq_cooldown_base_ms));
  memset(&kdi_irq_cooldown_max_ms, 0, sizeof(kdi_irq_cooldown_max_ms));
  memset(&kdi_irq_cooldown_until_ms, 0, sizeof(kdi_irq_cooldown_until_ms));
  memset(&kdi_irq_cooldown_level, 0, sizeof(kdi_irq_cooldown_level));
  memset(&kdi_irq_stats, 0, sizeof(kdi_irq_stats));
  memset(&kdi_irq_driver_counters, 0, sizeof(kdi_irq_driver_counters));
  memset(&kdi_deferred_queue, 0, sizeof(kdi_deferred_queue));
  memset(&kdi_irq_worker_fn, 0, sizeof(kdi_irq_worker_fn));
  memset(&kdi_irq_worker_ctx, 0, sizeof(kdi_irq_worker_ctx));
  memset(&kdi_dma_state, 0, sizeof(kdi_dma_state));
  memset(&kdi_fault_domain, 0, sizeof(kdi_fault_domain));
  kdi_deferred_head = 0U;
  kdi_deferred_tail = 0U;
  kdi_deferred_count = 0U;
  kdi_irq_starvation_ms = KDI_IRQ_STARVATION_DEFAULT_MS;
  kdi_fault_valid = 0U;
  kdi_fault_driver = KDI_DRIVER_KERNEL;
  kdi_now_ms_fn = NULL;
  kdi_now_ms_soft = 0U;
  kdi_profile_window_start_ms = kdi_now_ms();
  kdi_token_epoch = kdi_mix32(kdi_token_epoch + 0x6D2B79F5U);
  for (uint32_t i = 0U; i < (uint32_t)KDI_DRIVER_COUNT; ++i) {
    uint32_t now = kdi_now_ms();
    kdi_tokens[i] = kdi_make_token((KdiDriverId)i);
    kdi_token_active[i] = 1U;
    kdi_token_issued_ms[i] = now;
    kdi_token_ttl_ms[i] = kdi_token_ttl_default_ms[i];
    kdi_driver_state[i] = KDI_STATE_INIT;
    kdi_profile_state_visit_record((KdiDriverId)i, KDI_STATE_INIT);
    kdi_irq_budget_per_sec[i] = kdi_irq_budget_default_per_sec[i];
    kdi_irq_cooldown_base_ms[i] = kdi_irq_cooldown_base_default_ms[i];
    kdi_irq_cooldown_max_ms[i] = kdi_irq_cooldown_max_default_ms[i];
    kdi_irq_window_start_ms[i] = now;
    kdi_dma_driver_reset((KdiDriverId)i);
    kdi_fault_domain[i].generation = 1U;
  }
  kdi_driver_set_state(KDI_DRIVER_KERNEL, KDI_STATE_ACTIVE);
}

void kdi_set_now_ms_fn(KdiNowMsFn fn)
{
  kdi_now_ms_fn = fn;
}

void kdi_set_now_ms(uint32_t now_ms)
{
  kdi_now_ms_soft = now_ms;
}

int kdi_acquire_token(KdiDriverId driver, KdiCapToken *out_token)
{
  if (!kdi_driver_valid(driver) || out_token == NULL) {
    return KDI_ERR_BAD_ARG;
  }
  (void)kdi_expire_if_needed(driver);
  if (kdi_token_active[driver] == 0U) {
    return KDI_ERR_DENIED;
  }
  *out_token = kdi_tokens[driver];
  return KDI_OK;
}

int kdi_rotate_token(KdiDriverId driver, KdiCapToken *out_token)
{
  if (!kdi_driver_valid(driver) || out_token == NULL) {
    return KDI_ERR_BAD_ARG;
  }
  if (kdi_irq_in_handler[driver] != 0U) {
    return kdi_finish(driver, KDI_REQ_NONE, KDI_ERR_STATE, 0U, 0U);
  }

  kdi_token_epoch = kdi_mix32(kdi_token_epoch + 0x9E3779B9U + ((uint32_t)driver << 3));
  kdi_tokens[driver] = kdi_make_token(driver);
  kdi_token_active[driver] = 1U;
  kdi_token_issued_ms[driver] = kdi_now_ms();
  kdi_stats.token_rotate_total++;
  *out_token = kdi_tokens[driver];
  return KDI_OK;
}

int kdi_revoke_token(KdiDriverId driver)
{
  if (!kdi_driver_valid(driver)) {
    return KDI_ERR_BAD_ARG;
  }
  if (kdi_irq_in_handler[driver] != 0U) {
    return kdi_finish(driver, KDI_REQ_NONE, KDI_ERR_STATE, 0U, 0U);
  }
  if (kdi_token_active[driver] == 0U) {
    return KDI_OK;
  }
  kdi_token_active[driver] = 0U;
  kdi_tokens[driver] = KDI_CAP_INVALID;
  kdi_stats.token_revoke_total++;
  return KDI_OK;
}

int kdi_token_is_active(KdiDriverId driver, uint8_t *out_active)
{
  if (!kdi_driver_valid(driver) || out_active == NULL) {
    return KDI_ERR_BAD_ARG;
  }
  (void)kdi_expire_if_needed(driver);
  *out_active = kdi_token_active[driver];
  return KDI_OK;
}

int kdi_set_token_ttl_ms(KdiDriverId driver, uint32_t ttl_ms)
{
  if (!kdi_driver_valid(driver)) {
    return KDI_ERR_BAD_ARG;
  }
  if (kdi_irq_in_handler[driver] != 0U) {
    return kdi_finish(driver, KDI_REQ_NONE, KDI_ERR_STATE, ttl_ms, 0U);
  }
  kdi_token_ttl_ms[driver] = ttl_ms;
  (void)kdi_expire_if_needed(driver);
  return KDI_OK;
}

int kdi_get_token_ttl_ms(KdiDriverId driver, uint32_t *out_ttl_ms)
{
  if (!kdi_driver_valid(driver) || out_ttl_ms == NULL) {
    return KDI_ERR_BAD_ARG;
  }
  *out_ttl_ms = kdi_token_ttl_ms[driver];
  return KDI_OK;
}

int kdi_token_remaining_ms(KdiDriverId driver, uint32_t *out_remaining_ms)
{
  uint32_t ttl;
  uint32_t elapsed;

  if (!kdi_driver_valid(driver) || out_remaining_ms == NULL) {
    return KDI_ERR_BAD_ARG;
  }
  (void)kdi_expire_if_needed(driver);
  if (kdi_token_active[driver] == 0U) {
    return KDI_ERR_DENIED;
  }

  ttl = kdi_token_ttl_ms[driver];
  if (ttl == KDI_TOKEN_TTL_NEVER) {
    *out_remaining_ms = 0xFFFFFFFFU;
    return KDI_OK;
  }

  elapsed = kdi_now_ms() - kdi_token_issued_ms[driver];
  if (elapsed >= ttl) {
    *out_remaining_ms = 0U;
    return KDI_OK;
  }
  *out_remaining_ms = ttl - elapsed;
  return KDI_OK;
}

const char *kdi_driver_state_name(KdiDriverState state)
{
  switch (state) {
    case KDI_STATE_INIT:
      return "init";
    case KDI_STATE_PROBE:
      return "probe";
    case KDI_STATE_READY:
      return "ready";
    case KDI_STATE_ACTIVE:
      return "active";
    case KDI_STATE_ERROR:
      return "error";
    case KDI_STATE_RESET:
      return "reset";
    case KDI_STATE_DEAD:
      return "dead";
    default:
      return "unknown";
  }
}

int kdi_driver_get_state(KdiDriverId driver, KdiDriverState *out_state)
{
  if (!kdi_driver_valid(driver) || out_state == NULL) {
    return KDI_ERR_BAD_ARG;
  }
  *out_state = kdi_driver_state[driver];
  return KDI_OK;
}

int kdi_driver_probe(KdiDriverId driver, KdiCapToken token)
{
  KdiDriverState curr;

  if (!kdi_driver_valid(driver)) {
    return kdi_finish(driver, KDI_REQ_NONE, KDI_ERR_BAD_ARG, 0U, 0U);
  }
  if (driver == KDI_DRIVER_KERNEL) {
    return kdi_finish(driver, KDI_REQ_NONE, KDI_ERR_DENIED, KDI_STATE_ACTIVE, KDI_STATE_PROBE);
  }
  if (!kdi_token_valid(driver, token)) {
    return kdi_finish(driver, KDI_REQ_NONE, KDI_ERR_AUTH, (uint32_t)token, KDI_STATE_PROBE);
  }
  curr = kdi_driver_state[driver];
  if (curr != KDI_STATE_INIT) {
    return kdi_finish(driver, KDI_REQ_NONE, KDI_ERR_STATE, (uint32_t)curr, KDI_STATE_PROBE);
  }
  kdi_driver_set_state(driver, KDI_STATE_PROBE);
  return kdi_finish(driver, KDI_REQ_NONE, KDI_OK, KDI_STATE_INIT, KDI_STATE_PROBE);
}

int kdi_driver_probe_done(KdiDriverId driver, KdiCapToken token, uint8_t ok)
{
  KdiDriverState next;

  if (!kdi_driver_valid(driver)) {
    return kdi_finish(driver, KDI_REQ_NONE, KDI_ERR_BAD_ARG, 0U, 0U);
  }
  if (driver == KDI_DRIVER_KERNEL) {
    return kdi_finish(driver, KDI_REQ_NONE, KDI_ERR_DENIED, KDI_STATE_ACTIVE, KDI_STATE_READY);
  }
  if (!kdi_token_valid(driver, token)) {
    return kdi_finish(driver, KDI_REQ_NONE, KDI_ERR_AUTH, (uint32_t)token, (uint32_t)ok);
  }
  if (kdi_driver_state[driver] != KDI_STATE_PROBE) {
    return kdi_finish(driver, KDI_REQ_NONE, KDI_ERR_STATE, (uint32_t)kdi_driver_state[driver], KDI_STATE_PROBE);
  }
  next = (ok != 0U) ? KDI_STATE_READY : KDI_STATE_ERROR;
  kdi_driver_set_state(driver, next);
  return kdi_finish(driver, KDI_REQ_NONE, KDI_OK, KDI_STATE_PROBE, (uint32_t)next);
}

int kdi_driver_activate(KdiDriverId driver, KdiCapToken token)
{
  if (!kdi_driver_valid(driver)) {
    return kdi_finish(driver, KDI_REQ_NONE, KDI_ERR_BAD_ARG, 0U, 0U);
  }
  if (driver == KDI_DRIVER_KERNEL) {
    return kdi_finish(driver, KDI_REQ_NONE, KDI_ERR_DENIED, KDI_STATE_ACTIVE, KDI_STATE_ACTIVE);
  }
  if (!kdi_token_valid(driver, token)) {
    return kdi_finish(driver, KDI_REQ_NONE, KDI_ERR_AUTH, (uint32_t)token, KDI_STATE_ACTIVE);
  }
  if (kdi_driver_state[driver] != KDI_STATE_READY) {
    return kdi_finish(driver, KDI_REQ_NONE, KDI_ERR_STATE, (uint32_t)kdi_driver_state[driver], KDI_STATE_READY);
  }
  kdi_driver_set_state(driver, KDI_STATE_ACTIVE);
  return kdi_finish(driver, KDI_REQ_NONE, KDI_OK, KDI_STATE_READY, KDI_STATE_ACTIVE);
}

int kdi_driver_runtime_error(KdiDriverId driver, KdiCapToken token, uint16_t code)
{
  KdiDriverState curr;

  if (!kdi_driver_valid(driver) || code == 0U) {
    return kdi_finish(driver, KDI_REQ_NONE, KDI_ERR_BAD_ARG, (uint32_t)code, 0U);
  }
  if (driver == KDI_DRIVER_KERNEL) {
    return kdi_finish(driver, KDI_REQ_NONE, KDI_ERR_DENIED, KDI_STATE_ACTIVE, KDI_STATE_ERROR);
  }
  if (!kdi_token_valid(driver, token)) {
    return kdi_finish(driver, KDI_REQ_NONE, KDI_ERR_AUTH, (uint32_t)token, (uint32_t)code);
  }
  curr = kdi_driver_state[driver];
  if (curr != KDI_STATE_PROBE && curr != KDI_STATE_READY && curr != KDI_STATE_ACTIVE) {
    return kdi_finish(driver, KDI_REQ_NONE, KDI_ERR_STATE, (uint32_t)curr, KDI_STATE_ERROR);
  }

  kdi_fault_domain_record(driver, code, 0U);
  kdi_fault_domain[driver].crash_total++;
  kdi_contain_driver_fault(driver);
  kdi_driver_set_state(driver, KDI_STATE_ERROR);
  return kdi_finish(driver, KDI_REQ_NONE, KDI_OK, (uint32_t)curr, (uint32_t)code);
}

int kdi_driver_reset(KdiDriverId driver, KdiCapToken kernel_token)
{
  if (!kdi_driver_valid(driver)) {
    return kdi_finish(driver, KDI_REQ_NONE, KDI_ERR_BAD_ARG, 0U, 0U);
  }
  if (driver == KDI_DRIVER_KERNEL) {
    return kdi_finish(driver, KDI_REQ_NONE, KDI_ERR_DENIED, KDI_STATE_ACTIVE, KDI_STATE_RESET);
  }
  if (!kdi_token_valid(KDI_DRIVER_KERNEL, kernel_token)) {
    return kdi_finish(driver, KDI_REQ_NONE, KDI_ERR_AUTH, (uint32_t)kernel_token, KDI_STATE_RESET);
  }
  if (kdi_driver_state[driver] != KDI_STATE_ERROR) {
    return kdi_finish(driver, KDI_REQ_NONE, KDI_ERR_STATE, (uint32_t)kdi_driver_state[driver], KDI_STATE_ERROR);
  }
  kdi_driver_set_state(driver, KDI_STATE_RESET);
  return kdi_finish(driver, KDI_REQ_NONE, KDI_OK, KDI_STATE_ERROR, KDI_STATE_RESET);
}

int kdi_driver_reinit(KdiDriverId driver, KdiCapToken kernel_token, KdiCapToken *out_token)
{
  int rc;
  KdiCapToken rotated = KDI_CAP_INVALID;
  KdiDriverState curr;

  if (!kdi_driver_valid(driver)) {
    return kdi_finish(driver, KDI_REQ_NONE, KDI_ERR_BAD_ARG, 0U, 0U);
  }
  if (driver == KDI_DRIVER_KERNEL) {
    return kdi_finish(driver, KDI_REQ_NONE, KDI_ERR_DENIED, KDI_STATE_ACTIVE, KDI_STATE_INIT);
  }
  if (!kdi_token_valid(KDI_DRIVER_KERNEL, kernel_token)) {
    return kdi_finish(driver, KDI_REQ_NONE, KDI_ERR_AUTH, (uint32_t)kernel_token, KDI_STATE_INIT);
  }
  curr = kdi_driver_state[driver];
  if (curr != KDI_STATE_RESET && curr != KDI_STATE_DEAD) {
    return kdi_finish(driver, KDI_REQ_NONE, KDI_ERR_STATE, (uint32_t)curr, KDI_STATE_RESET);
  }

  rc = kdi_rotate_token(driver, &rotated);
  if (rc != KDI_OK) {
    return kdi_finish(driver, KDI_REQ_NONE, rc, (uint32_t)curr, 0U);
  }
  kdi_dma_driver_reset(driver);
  kdi_fault_domain[driver].isolated = 0U;
  kdi_fault_domain[driver].active_fault = 0U;
  kdi_fault_domain[driver].restart_total++;
  kdi_fault_domain[driver].generation++;
  kdi_driver_set_state(driver, KDI_STATE_INIT);
  if (out_token != NULL) {
    *out_token = rotated;
  }
  return kdi_finish(driver, KDI_REQ_NONE, KDI_OK, (uint32_t)curr, KDI_STATE_INIT);
}

int kdi_driver_force_reclaim(KdiDriverId driver, KdiCapToken kernel_token)
{
  KdiDriverState curr;

  if (!kdi_driver_valid(driver)) {
    return kdi_finish(driver, KDI_REQ_NONE, KDI_ERR_BAD_ARG, 0U, 0U);
  }
  if (driver == KDI_DRIVER_KERNEL) {
    return kdi_finish(driver, KDI_REQ_NONE, KDI_ERR_DENIED, KDI_STATE_ACTIVE, KDI_STATE_DEAD);
  }
  if (!kdi_token_valid(KDI_DRIVER_KERNEL, kernel_token)) {
    return kdi_finish(driver, KDI_REQ_NONE, KDI_ERR_AUTH, (uint32_t)kernel_token, KDI_STATE_DEAD);
  }
  curr = kdi_driver_state[driver];
  if (curr == KDI_STATE_DEAD) {
    return kdi_finish(driver, KDI_REQ_NONE, KDI_OK, KDI_STATE_DEAD, KDI_STATE_DEAD);
  }
  kdi_contain_driver_fault(driver);
  kdi_driver_set_state(driver, KDI_STATE_DEAD);
  (void)kdi_revoke_token(driver);
  kdi_stats.force_reclaim_total++;
  return kdi_finish(driver, KDI_REQ_NONE, KDI_OK, (uint32_t)curr, KDI_STATE_DEAD);
}

int kdi_irq_set_budget_per_sec(KdiDriverId driver, uint32_t budget_per_sec)
{
  uint32_t now;

  if (!kdi_driver_valid(driver)) {
    return kdi_finish(driver, KDI_REQ_IRQ, KDI_ERR_BAD_ARG, budget_per_sec, 0U);
  }
  now = kdi_now_ms();
  kdi_irq_budget_per_sec[driver] = budget_per_sec;
  kdi_irq_window_count[driver] = 0U;
  kdi_irq_window_start_ms[driver] = now;
  kdi_irq_throttled[driver] = 0U;
  kdi_irq_cooldown_until_ms[driver] = 0U;
  kdi_irq_cooldown_level[driver] = 0U;
  return kdi_finish(driver, KDI_REQ_IRQ, KDI_OK, budget_per_sec, 0U);
}

int kdi_irq_get_budget_per_sec(KdiDriverId driver, uint32_t *out_budget_per_sec)
{
  if (!kdi_driver_valid(driver) || out_budget_per_sec == NULL) {
    return KDI_ERR_BAD_ARG;
  }
  *out_budget_per_sec = kdi_irq_budget_per_sec[driver];
  return KDI_OK;
}

int kdi_irq_set_cooldown_ms(KdiDriverId driver, uint32_t base_ms, uint32_t max_ms)
{
  if (!kdi_driver_valid(driver)) {
    return kdi_finish(driver, KDI_REQ_IRQ, KDI_ERR_BAD_ARG, base_ms, max_ms);
  }
  if (max_ms != 0U && base_ms > max_ms) {
    return kdi_finish(driver, KDI_REQ_IRQ, KDI_ERR_BAD_ARG, base_ms, max_ms);
  }
  if (max_ms == 0U) {
    max_ms = base_ms;
  }
  if (kdi_irq_in_handler[driver] != 0U) {
    return kdi_finish(driver, KDI_REQ_IRQ, KDI_ERR_STATE, base_ms, max_ms);
  }

  kdi_irq_cooldown_base_ms[driver] = base_ms;
  kdi_irq_cooldown_max_ms[driver] = max_ms;
  kdi_irq_cooldown_until_ms[driver] = 0U;
  kdi_irq_cooldown_level[driver] = 0U;
  kdi_irq_throttled[driver] = 0U;
  kdi_irq_window_count[driver] = 0U;
  kdi_irq_window_start_ms[driver] = kdi_now_ms();
  return kdi_finish(driver, KDI_REQ_IRQ, KDI_OK, base_ms, max_ms);
}

int kdi_irq_set_starvation_ms(uint32_t starvation_ms)
{
  kdi_irq_starvation_ms = starvation_ms;
  return kdi_finish(KDI_DRIVER_KERNEL, KDI_REQ_NONE, KDI_OK, starvation_ms, 0U);
}

int kdi_irq_set_worker(KdiDriverId driver, KdiCapToken token, KdiIrqWorkFn fn, void *ctx)
{
  if (!kdi_driver_valid(driver)) {
    return kdi_finish(driver, KDI_REQ_IRQ, KDI_ERR_BAD_ARG, 0U, 0U);
  }
  if (!kdi_token_valid(driver, token)) {
    return kdi_finish(driver, KDI_REQ_IRQ, KDI_ERR_AUTH, (uint32_t)token, 0U);
  }
  if (kdi_irq_in_handler[driver] != 0U) {
    return kdi_finish(driver, KDI_REQ_IRQ, KDI_ERR_STATE, 1U, 0U);
  }
  if (kdi_driver_state[driver] == KDI_STATE_DEAD) {
    return kdi_finish(driver, KDI_REQ_IRQ, KDI_ERR_STATE, KDI_STATE_DEAD, 0U);
  }

  kdi_irq_worker_fn[driver] = fn;
  kdi_irq_worker_ctx[driver] = ctx;
  return kdi_finish(driver, KDI_REQ_IRQ, KDI_OK, (fn != NULL) ? 1U : 0U, 0U);
}

int kdi_irq_enter(KdiDriverId driver, KdiCapToken token)
{
  uint32_t budget;
  uint32_t count;
  uint32_t now;
  uint32_t cooldown_ms;
  uint32_t rem_ms;

  if (!kdi_driver_valid(driver)) {
    return kdi_finish(driver, KDI_REQ_IRQ, KDI_ERR_BAD_ARG, 0U, 0U);
  }
  if (!kdi_token_valid(driver, token)) {
    return kdi_finish(driver, KDI_REQ_IRQ, KDI_ERR_AUTH, (uint32_t)token, 0U);
  }
  if (kdi_driver_state[driver] != KDI_STATE_ACTIVE) {
    return kdi_finish(driver, KDI_REQ_IRQ, KDI_ERR_STATE, (uint32_t)kdi_driver_state[driver], 0U);
  }
  if (kdi_irq_in_handler[driver] != 0U) {
    return kdi_finish(driver, KDI_REQ_IRQ, KDI_ERR_STATE, 1U, 0U);
  }

  kdi_irq_reset_window_if_needed(driver);
  now = kdi_now_ms();
  if (kdi_irq_throttled[driver] != 0U) {
    if (kdi_irq_cooldown_until_ms[driver] != 0U && !kdi_ms_reached(now, kdi_irq_cooldown_until_ms[driver])) {
      rem_ms = kdi_irq_cooldown_until_ms[driver] - now;
      return kdi_finish(driver, KDI_REQ_IRQ, KDI_ERR_LIMIT, rem_ms, kdi_irq_cooldown_level[driver]);
    }
    if (kdi_irq_cooldown_until_ms[driver] != 0U) {
      kdi_irq_clear_throttle(driver, 1U);
    }
  }

  budget = kdi_irq_budget_per_sec[driver];
  count = kdi_irq_window_count[driver];
  if (budget != 0U && count >= budget) {
    cooldown_ms = kdi_irq_cooldown_duration_ms(driver);
    if (cooldown_ms != 0U) {
      if (kdi_irq_cooldown_level[driver] < 7U) {
        kdi_irq_cooldown_level[driver]++;
      }
      kdi_irq_cooldown_until_ms[driver] = now + cooldown_ms;
      kdi_irq_stats.irq_cooldown_total++;
    } else {
      kdi_irq_cooldown_until_ms[driver] = 0U;
    }
    kdi_irq_throttled[driver] = 1U;
    kdi_irq_stats.irq_throttle_total++;
    kdi_irq_driver_counters[driver].irq_throttle_total++;
    /* seam: IRQ throttle hit — record driver_id and remaining budget */
    seam_emit(CFL_LAYER_KDI, CFL_EV_KDI_THROTTLE,
              (uint32_t)driver, kdi_irq_budget_per_sec[driver], 0U, 0U);
    return kdi_finish(driver, KDI_REQ_IRQ, KDI_ERR_LIMIT, count, cooldown_ms);
  }

  kdi_irq_window_count[driver] = count + 1U;
  kdi_irq_in_handler[driver] = 1U;
  kdi_irq_stats.irq_enter_total++;
  kdi_irq_driver_counters[driver].irq_enter_total++;
  return kdi_finish(driver, KDI_REQ_IRQ, KDI_OK, kdi_irq_window_count[driver], budget);
}

int kdi_irq_defer(KdiDriverId driver, KdiCapToken token, uint16_t work_id, uint32_t arg)
{
  KdiDeferredWork *slot;

  if (!kdi_driver_valid(driver)) {
    return kdi_finish(driver, KDI_REQ_IRQ, KDI_ERR_BAD_ARG, work_id, arg);
  }
  if (!kdi_token_valid(driver, token)) {
    return kdi_finish(driver, KDI_REQ_IRQ, KDI_ERR_AUTH, (uint32_t)token, work_id);
  }
  if (kdi_irq_in_handler[driver] == 0U) {
    return kdi_finish(driver, KDI_REQ_IRQ, KDI_ERR_STATE, 0U, work_id);
  }
  if (kdi_deferred_count >= KDI_DEFERRED_QUEUE_CAP) {
    kdi_irq_stats.irq_drop_total++;
    kdi_irq_driver_counters[driver].irq_drop_total++;
    return kdi_finish(driver, KDI_REQ_IRQ, KDI_ERR_LIMIT, kdi_deferred_count, KDI_DEFERRED_QUEUE_CAP);
  }

  slot = &kdi_deferred_queue[kdi_deferred_head];
  slot->driver = (uint8_t)driver;
  slot->work_id = work_id;
  slot->arg = arg;
  slot->queued_ms = kdi_now_ms();
  kdi_deferred_head = (uint8_t)((kdi_deferred_head + 1U) % KDI_DEFERRED_QUEUE_CAP);
  kdi_deferred_count++;
  kdi_irq_stats.irq_defer_total++;
  kdi_irq_driver_counters[driver].irq_defer_total++;
  return kdi_finish(driver, KDI_REQ_IRQ, KDI_OK, work_id, arg);
}

int kdi_irq_exit(KdiDriverId driver, KdiCapToken token)
{
  if (!kdi_driver_valid(driver)) {
    return kdi_finish(driver, KDI_REQ_IRQ, KDI_ERR_BAD_ARG, 0U, 0U);
  }
  if (!kdi_token_valid(driver, token)) {
    return kdi_finish(driver, KDI_REQ_IRQ, KDI_ERR_AUTH, (uint32_t)token, 0U);
  }
  if (kdi_irq_in_handler[driver] == 0U) {
    return kdi_finish(driver, KDI_REQ_IRQ, KDI_ERR_STATE, 0U, 0U);
  }
  kdi_irq_in_handler[driver] = 0U;
  return kdi_finish(driver, KDI_REQ_IRQ, KDI_OK, 0U, 0U);
}

int kdi_irq_worker_run(uint32_t max_items, uint32_t *out_processed)
{
  uint32_t processed = 0U;
  uint32_t worker_errors = 0U;

  if (max_items == 0U) {
    return kdi_finish(KDI_DRIVER_KERNEL, KDI_REQ_NONE, KDI_ERR_BAD_ARG, 0U, 0U);
  }

  while (processed < max_items && kdi_deferred_count > 0U) {
    KdiDeferredWork work = kdi_deferred_queue[kdi_deferred_tail];
    KdiDeferredWork *slot = &kdi_deferred_queue[kdi_deferred_tail];
    KdiIrqWorkFn fn = NULL;
    void *ctx = NULL;
    uint8_t worker_error = 0U;

    if (kdi_driver_valid((KdiDriverId)work.driver)) {
      fn = kdi_irq_worker_fn[work.driver];
      ctx = kdi_irq_worker_ctx[work.driver];
      if (kdi_driver_state[work.driver] != KDI_STATE_ACTIVE) {
        worker_errors++;
        worker_error = 1U;
      } else if (fn != NULL) {
        if (fn((KdiDriverId)work.driver, work.work_id, work.arg, ctx) != KDI_OK) {
          worker_errors++;
          worker_error = 1U;
        }
      } else {
        worker_errors++;
        worker_error = 1U;
      }
      kdi_irq_driver_counters[work.driver].irq_worker_total++;
      if (worker_error != 0U) {
        kdi_irq_driver_counters[work.driver].irq_worker_error_total++;
      }
    } else {
      worker_errors++;
    }

    memset(slot, 0, sizeof(*slot));
    kdi_deferred_tail = (uint8_t)((kdi_deferred_tail + 1U) % KDI_DEFERRED_QUEUE_CAP);
    kdi_deferred_count--;
    processed++;
  }

  kdi_irq_stats.irq_worker_total += processed;
  kdi_irq_stats.irq_worker_error_total += worker_errors;
  if (out_processed != NULL) {
    *out_processed = processed;
  }
  return kdi_finish(KDI_DRIVER_KERNEL, KDI_REQ_NONE, KDI_OK, processed, worker_errors);
}

int kdi_irq_poll_starvation(uint32_t *out_detected)
{
  uint32_t detected = 0U;

  if (out_detected == NULL) {
    return kdi_finish(KDI_DRIVER_KERNEL, KDI_REQ_NONE, KDI_ERR_BAD_ARG, 0U, 0U);
  }
  if (kdi_deferred_count > 0U && kdi_irq_starvation_ms > 0U) {
    const KdiDeferredWork *oldest = &kdi_deferred_queue[kdi_deferred_tail];
    uint32_t age = kdi_now_ms() - oldest->queued_ms;
    if (age >= kdi_irq_starvation_ms) {
      detected = 1U;
      kdi_irq_stats.irq_starvation_total++;
    }
  }
  *out_detected = detected;
  return kdi_finish(KDI_DRIVER_KERNEL, KDI_REQ_NONE, KDI_OK, detected, kdi_deferred_count);
}

int kdi_irq_unsafe_op(KdiDriverId driver, KdiCapToken token, KdiIrqUnsafeOp op)
{
  if (!kdi_driver_valid(driver)) {
    return kdi_finish(driver, KDI_REQ_NONE, KDI_ERR_BAD_ARG, (uint32_t)op, 0U);
  }
  if (!kdi_token_valid(driver, token)) {
    return kdi_finish(driver, KDI_REQ_NONE, KDI_ERR_AUTH, (uint32_t)token, (uint32_t)op);
  }
  if (op < KDI_IRQ_UNSAFE_MALLOC || op > KDI_IRQ_UNSAFE_POLICY) {
    return kdi_finish(driver, KDI_REQ_NONE, KDI_ERR_BAD_ARG, (uint32_t)op, 0U);
  }
  if (kdi_irq_in_handler[driver] != 0U) {
    kdi_irq_stats.irq_unsafe_total++;
    kdi_irq_driver_counters[driver].irq_unsafe_total++;
    return kdi_finish(driver, KDI_REQ_NONE, KDI_ERR_STATE, (uint32_t)op, 1U);
  }
  return kdi_finish(driver, KDI_REQ_NONE, KDI_OK, (uint32_t)op, 0U);
}

void kdi_irq_get_stats(KdiIrqStats *out)
{
  if (out == NULL) {
    return;
  }
  *out = kdi_irq_stats;
  out->deferred_pending = kdi_deferred_count;
  out->starvation_ms = kdi_irq_starvation_ms;
}

int kdi_irq_get_driver_stats(KdiDriverId driver, KdiIrqDriverStats *out)
{
  if (!kdi_driver_valid(driver) || out == NULL) {
    return KDI_ERR_BAD_ARG;
  }
  kdi_irq_reset_window_if_needed(driver);
  out->in_irq = kdi_irq_in_handler[driver];
  out->throttled = kdi_irq_throttled[driver];
  out->worker_registered = (kdi_irq_worker_fn[driver] != NULL) ? 1U : 0U;
  out->cooldown_level = kdi_irq_cooldown_level[driver];
  out->budget_per_sec = kdi_irq_budget_per_sec[driver];
  out->window_count = kdi_irq_window_count[driver];
  out->cooldown_base_ms = kdi_irq_cooldown_base_ms[driver];
  out->cooldown_max_ms = kdi_irq_cooldown_max_ms[driver];
  out->cooldown_until_ms = kdi_irq_cooldown_until_ms[driver];
  out->deferred_pending = kdi_deferred_pending_for_driver(driver);
  return KDI_OK;
}

int kdi_irq_get_driver_counters(KdiDriverId driver, KdiIrqDriverCounters *out)
{
  if (!kdi_driver_valid(driver) || out == NULL) {
    return KDI_ERR_BAD_ARG;
  }
  *out = kdi_irq_driver_counters[driver];
  return KDI_OK;
}

const KdiPolicy *kdi_get_policy(KdiDriverId driver)
{
  if (!kdi_driver_valid(driver)) {
    return NULL;
  }
  return &kdi_policy_table[driver].policy;
}

static uint32_t kdi_declared_cap_mask(KdiDriverId driver)
{
  const KdiPolicy *p = kdi_get_policy(driver);
  uint32_t mask = 0U;

  if (p == NULL) {
    return 0U;
  }
  if (p->allow_mpu != 0U) {
    mask |= KDI_CAP_REQ_BIT(KDI_REQ_MPU);
  }
  if (p->allow_irq != 0U) {
    mask |= KDI_CAP_REQ_BIT(KDI_REQ_IRQ);
  }
  if (p->allow_dma != 0U) {
    mask |= KDI_CAP_REQ_BIT(KDI_REQ_DMA);
  }
  if (p->allow_fault != 0U) {
    mask |= KDI_CAP_REQ_BIT(KDI_REQ_FAULT);
  }
  if (p->allow_power != 0U) {
    mask |= KDI_CAP_REQ_BIT(KDI_REQ_POWER);
  }
  if (p->allow_reset != 0U) {
    mask |= KDI_CAP_REQ_BIT(KDI_REQ_RESET);
  }
  return mask;
}

const char *kdi_driver_name(KdiDriverId driver)
{
  if (!kdi_driver_valid(driver)) {
    return "unknown";
  }
  return kdi_policy_table[driver].name;
}

const char *kdi_request_name(KdiRequestType req)
{
  switch (req) {
    case KDI_REQ_MPU:
      return "mpu";
    case KDI_REQ_IRQ:
      return "irq";
    case KDI_REQ_DMA:
      return "dma";
    case KDI_REQ_FAULT:
      return "fault";
    case KDI_REQ_POWER:
      return "power";
    case KDI_REQ_RESET:
      return "reset";
    default:
      return "none";
  }
}

const char *kdi_result_name(int rc)
{
  switch (rc) {
    case KDI_OK:
      return "ok";
    case KDI_ERR_BAD_ARG:
      return "bad_arg";
    case KDI_ERR_DENIED:
      return "denied";
    case KDI_ERR_LIMIT:
      return "limit";
    case KDI_ERR_UNSUPPORTED:
      return "unsupported";
    case KDI_ERR_AUTH:
      return "auth";
    case KDI_ERR_STATE:
      return "state";
    default:
      return "unknown";
  }
}

int kdi_request_mpu_region(KdiDriverId driver, KdiCapToken token, const KdiMpuRequest *req)
{
  const KdiPolicy *policy;

  if (!kdi_driver_valid(driver) || req == NULL) {
    return kdi_finish(driver, KDI_REQ_MPU, KDI_ERR_BAD_ARG, 0U, 0U);
  }
  if (!kdi_token_valid(driver, token)) {
    return kdi_finish(driver, KDI_REQ_MPU, KDI_ERR_AUTH, (uint32_t)token, req->region_index);
  }
  if (!kdi_state_allows_request(driver)) {
    return kdi_finish(driver, KDI_REQ_MPU, KDI_ERR_STATE, (uint32_t)kdi_driver_state[driver], req->region_index);
  }
  policy = &kdi_policy_table[driver].policy;
  if (policy->allow_mpu == 0U) {
    return kdi_finish(driver, KDI_REQ_MPU, KDI_ERR_DENIED, req->region_index, req->size);
  }
  if (req->region_index >= policy->max_mpu_regions || req->region_index >= KDI_HW_MPU_REGION_COUNT) {
    return kdi_finish(driver, KDI_REQ_MPU, KDI_ERR_LIMIT, req->region_index, req->size);
  }
  if (!kdi_is_pow2(req->size) || req->size < KDI_MPU_MIN_SIZE) {
    return kdi_finish(driver, KDI_REQ_MPU, KDI_ERR_BAD_ARG, req->region_index, req->size);
  }
  if ((req->base & (req->size - 1U)) != 0U) {
    return kdi_finish(driver, KDI_REQ_MPU, KDI_ERR_BAD_ARG, req->base, req->size);
  }
  return kdi_finish(driver, KDI_REQ_MPU, KDI_OK, req->base, req->size);
}

int kdi_request_irq(KdiDriverId driver, KdiCapToken token, const KdiIrqRequest *req)
{
  const KdiPolicy *policy;

  if (!kdi_driver_valid(driver) || req == NULL) {
    return kdi_finish(driver, KDI_REQ_IRQ, KDI_ERR_BAD_ARG, 0U, 0U);
  }
  if (!kdi_token_valid(driver, token)) {
    return kdi_finish(driver, KDI_REQ_IRQ, KDI_ERR_AUTH, (uint32_t)token, (uint32_t)req->irqn);
  }
  if (!kdi_state_allows_request(driver)) {
    return kdi_finish(driver, KDI_REQ_IRQ, KDI_ERR_STATE, (uint32_t)kdi_driver_state[driver], (uint32_t)req->irqn);
  }
  policy = &kdi_policy_table[driver].policy;
  if (policy->allow_irq == 0U) {
    return kdi_finish(driver, KDI_REQ_IRQ, KDI_ERR_DENIED, (uint32_t)req->irqn, req->priority);
  }
  if (req->irqn < policy->min_irqn || req->irqn > policy->max_irqn) {
    return kdi_finish(driver, KDI_REQ_IRQ, KDI_ERR_LIMIT, (uint32_t)req->irqn, req->priority);
  }
  if (req->priority > 15U) {
    return kdi_finish(driver, KDI_REQ_IRQ, KDI_ERR_BAD_ARG, (uint32_t)req->irqn, req->priority);
  }
  return kdi_finish(driver, KDI_REQ_IRQ, KDI_OK, (uint32_t)req->irqn, req->priority);
}

int kdi_declare_dma_buffer(KdiDriverId driver, KdiCapToken token, const KdiDmaRequest *req)
{
  int rc;
  uint8_t align;

  if (req == NULL || !kdi_driver_valid(driver)) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_BAD_ARG, 0U, 0U);
  }

  rc = kdi_dma_validate_declare(driver, token, req, NULL, &align);
  if (rc == KDI_OK) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_OK, (uint32_t)req->base, req->size);
  }

  switch (rc) {
    case KDI_ERR_AUTH:
      return kdi_finish(driver, KDI_REQ_DMA, rc, (uint32_t)token, req->size);
    case KDI_ERR_STATE:
      return kdi_finish(driver, KDI_REQ_DMA, rc, (uint32_t)kdi_driver_state[driver], req->size);
    case KDI_ERR_DENIED:
      return kdi_finish(driver, KDI_REQ_DMA, rc, (uint32_t)req->base, req->size);
    case KDI_ERR_LIMIT:
      return kdi_finish(driver, KDI_REQ_DMA, rc, (uint32_t)req->base, req->size);
    case KDI_ERR_BAD_ARG:
      return kdi_finish(driver, KDI_REQ_DMA, rc, (uint32_t)req->base, align);
    default:
      return kdi_finish(driver, KDI_REQ_DMA, rc, (uint32_t)req->base, req->size);
  }
}

int kdi_dma_ring_configure(KdiDriverId driver, KdiCapToken token, const KdiDmaRingConfig *cfg)
{
  KdiDmaDriverState *d;
  uint16_t rx_depth;
  uint16_t tx_depth;

  if (!kdi_driver_valid(driver) || cfg == NULL) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_BAD_ARG, 0U, 0U);
  }
  if (!kdi_token_valid(driver, token)) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_AUTH, (uint32_t)token, 0U);
  }
  if (!kdi_state_allows_request(driver)) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_STATE, (uint32_t)kdi_driver_state[driver], 0U);
  }
  if (!kdi_dma_policy_allowed(driver)) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_DENIED, 0U, 0U);
  }

  rx_depth = (cfg->rx_depth == 0U) ? KDI_DMA_RING_CAP : cfg->rx_depth;
  tx_depth = (cfg->tx_depth == 0U) ? KDI_DMA_RING_CAP : cfg->tx_depth;
  if (rx_depth > KDI_DMA_RING_CAP || tx_depth > KDI_DMA_RING_CAP) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_BAD_ARG, rx_depth, tx_depth);
  }
  if (kdi_dma_driver_busy(driver)) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_STATE, 1U, 0U);
  }

  d = &kdi_dma_state[driver];
  kdi_dma_ring_reset(&d->rx_posted, rx_depth);
  kdi_dma_ring_reset(&d->rx_ready, rx_depth);
  kdi_dma_ring_reset(&d->tx_pending, tx_depth);
  kdi_dma_ring_reset(&d->tx_done, tx_depth);
  return kdi_finish(driver, KDI_REQ_DMA, KDI_OK, rx_depth, tx_depth);
}

int kdi_dma_register_buffer(KdiDriverId driver, KdiCapToken token, const KdiDmaRequest *req, uint16_t *out_buffer_id)
{
  KdiDmaDriverState *d;
  uint8_t align;
  int rc;

  if (out_buffer_id == NULL) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_BAD_ARG, 0U, 0U);
  }
  if (!kdi_driver_valid(driver) || req == NULL) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_BAD_ARG, 0U, 0U);
  }

  rc = kdi_dma_validate_declare(driver, token, req, NULL, &align);
  if (rc != KDI_OK) {
    switch (rc) {
      case KDI_ERR_AUTH:
        return kdi_finish(driver, KDI_REQ_DMA, rc, (uint32_t)token, req->size);
      case KDI_ERR_STATE:
        return kdi_finish(driver, KDI_REQ_DMA, rc, (uint32_t)kdi_driver_state[driver], req->size);
      case KDI_ERR_DENIED:
      case KDI_ERR_LIMIT:
        return kdi_finish(driver, KDI_REQ_DMA, rc, (uint32_t)req->base, req->size);
      default:
        return kdi_finish(driver, KDI_REQ_DMA, rc, (uint32_t)req->base, align);
    }
  }

  d = &kdi_dma_state[driver];
  for (uint16_t i = 0U; i < KDI_DMA_BUFFER_CAP; ++i) {
    if (d->buffers[i].used != 0U && d->buffers[i].base == req->base) {
      return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_STATE, (uint32_t)req->base, i);
    }
  }

  for (uint16_t i = 0U; i < KDI_DMA_BUFFER_CAP; ++i) {
    KdiDmaBuffer *b = &d->buffers[i];
    if (b->used != 0U) {
      continue;
    }
    memset(b, 0, sizeof(*b));
    b->used = 1U;
    b->owner = KDI_DMA_OWNER_DRIVER;
    b->align = align;
    b->direction = req->direction;
    b->base = req->base;
    b->size = req->size;
    b->state = KDI_DMA_BUF_IDLE;
    d->stats.buffers_registered++;
    *out_buffer_id = i;
    return kdi_finish(driver, KDI_REQ_DMA, KDI_OK, i, req->size);
  }

  return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_LIMIT, KDI_DMA_BUFFER_CAP, req->size);
}

int kdi_dma_unregister_buffer(KdiDriverId driver, KdiCapToken token, uint16_t buffer_id)
{
  KdiDmaDriverState *d;
  KdiDmaBuffer *b;

  if (!kdi_driver_valid(driver) || buffer_id >= KDI_DMA_BUFFER_CAP) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_BAD_ARG, buffer_id, 0U);
  }
  if (!kdi_token_valid(driver, token)) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_AUTH, (uint32_t)token, buffer_id);
  }
  if (!kdi_state_allows_request(driver)) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_STATE, (uint32_t)kdi_driver_state[driver], buffer_id);
  }

  d = &kdi_dma_state[driver];
  b = kdi_dma_buffer_get(driver, buffer_id);
  if (b == NULL) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_BAD_ARG, buffer_id, 0U);
  }
  if (b->owner != KDI_DMA_OWNER_DRIVER || b->state != KDI_DMA_BUF_IDLE) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_STATE, buffer_id, b->state);
  }

  memset(b, 0, sizeof(*b));
  d->stats.buffers_unregistered++;
  return kdi_finish(driver, KDI_REQ_DMA, KDI_OK, buffer_id, 0U);
}

int kdi_dma_get_owner(KdiDriverId driver, KdiCapToken token, uint16_t buffer_id, KdiDmaOwner *out_owner)
{
  KdiDmaBuffer *b;

  if (!kdi_driver_valid(driver) || out_owner == NULL || buffer_id >= KDI_DMA_BUFFER_CAP) {
    return KDI_ERR_BAD_ARG;
  }
  if (!kdi_token_valid(driver, token)) {
    return KDI_ERR_AUTH;
  }
  b = kdi_dma_buffer_get(driver, buffer_id);
  if (b == NULL) {
    return KDI_ERR_BAD_ARG;
  }

  *out_owner = (KdiDmaOwner)b->owner;
  return KDI_OK;
}

int kdi_dma_rx_post_buffer(KdiDriverId driver, KdiCapToken token, uint16_t buffer_id)
{
  KdiDmaDriverState *d;
  KdiDmaBuffer *b;
  int rc;

  if (!kdi_driver_valid(driver) || buffer_id >= KDI_DMA_BUFFER_CAP) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_BAD_ARG, buffer_id, 0U);
  }
  if (!kdi_token_valid(driver, token)) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_AUTH, (uint32_t)token, buffer_id);
  }
  if (!kdi_dma_runtime_allowed(driver)) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_STATE, (uint32_t)kdi_driver_state[driver], buffer_id);
  }
  if (!kdi_dma_policy_allowed(driver)) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_DENIED, buffer_id, 0U);
  }

  d = &kdi_dma_state[driver];
  b = kdi_dma_buffer_get(driver, buffer_id);
  if (b == NULL) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_BAD_ARG, buffer_id, 0U);
  }
  if (b->owner != KDI_DMA_OWNER_DRIVER || b->state != KDI_DMA_BUF_IDLE) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_STATE, buffer_id, b->state);
  }

  rc = kdi_dma_ring_push(&d->rx_posted, buffer_id);
  if (rc != KDI_OK) {
    d->stats.ring_overflow_total++;
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_LIMIT, buffer_id, d->rx_posted.depth);
  }

  b->owner = KDI_DMA_OWNER_KERNEL;
  b->state = KDI_DMA_BUF_RX_POSTED;
  b->last_bytes = 0U;
  d->stats.rx_post_total++;
  return kdi_finish(driver, KDI_REQ_DMA, KDI_OK, buffer_id, d->rx_posted.count);
}

int kdi_dma_rx_complete_one(KdiDriverId driver, KdiCapToken kernel_token, uint16_t bytes, uint16_t *out_buffer_id)
{
  KdiDmaDriverState *d;
  KdiDmaBuffer *b;
  uint16_t buffer_id;
  int rc;

  if (!kdi_driver_valid(driver)) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_BAD_ARG, 0U, 0U);
  }
  if (!kdi_token_valid(KDI_DRIVER_KERNEL, kernel_token)) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_AUTH, (uint32_t)kernel_token, driver);
  }
  if (!kdi_dma_runtime_allowed(driver)) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_STATE, (uint32_t)kdi_driver_state[driver], 0U);
  }
  if (!kdi_dma_policy_allowed(driver)) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_DENIED, 0U, 0U);
  }
  if (bytes == 0U) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_BAD_ARG, 0U, bytes);
  }

  d = &kdi_dma_state[driver];
  if (d->rx_posted.count == 0U) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_STATE, 0U, 0U);
  }
  if (d->rx_ready.count >= d->rx_ready.depth) {
    d->stats.ring_overflow_total++;
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_LIMIT, d->rx_ready.count, d->rx_ready.depth);
  }

  buffer_id = d->rx_posted.slots[d->rx_posted.tail];
  b = kdi_dma_buffer_get(driver, buffer_id);
  if (b == NULL || b->state != KDI_DMA_BUF_RX_POSTED || b->owner != KDI_DMA_OWNER_KERNEL) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_STATE, buffer_id, (b == NULL) ? 0U : b->state);
  }
  if (bytes > b->size) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_BAD_ARG, buffer_id, bytes);
  }

  rc = kdi_dma_ring_pop(&d->rx_posted, &buffer_id);
  if (rc != KDI_OK) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_STATE, 0U, 0U);
  }
  b = kdi_dma_buffer_get(driver, buffer_id);
  if (b == NULL) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_STATE, buffer_id, 0U);
  }
  b->last_bytes = bytes;
  b->state = KDI_DMA_BUF_RX_READY;
  rc = kdi_dma_ring_push(&d->rx_ready, buffer_id);
  if (rc != KDI_OK) {
    b->state = KDI_DMA_BUF_RX_POSTED;
    b->last_bytes = 0U;
    d->stats.ring_overflow_total++;
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_LIMIT, d->rx_ready.count, d->rx_ready.depth);
  }

  d->stats.rx_complete_total++;
  if (out_buffer_id != NULL) {
    *out_buffer_id = buffer_id;
  }
  return kdi_finish(driver, KDI_REQ_DMA, KDI_OK, buffer_id, bytes);
}

int kdi_dma_rx_poll(KdiDriverId driver, KdiCapToken token, uint16_t *out_buffer_id, uint16_t *out_bytes)
{
  KdiDmaDriverState *d;
  KdiDmaBuffer *b;
  uint16_t buffer_id;
  int rc;

  if (!kdi_driver_valid(driver) || out_buffer_id == NULL || out_bytes == NULL) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_BAD_ARG, 0U, 0U);
  }
  if (!kdi_token_valid(driver, token)) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_AUTH, (uint32_t)token, 0U);
  }
  if (!kdi_dma_runtime_allowed(driver)) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_STATE, (uint32_t)kdi_driver_state[driver], 0U);
  }

  d = &kdi_dma_state[driver];
  rc = kdi_dma_ring_pop(&d->rx_ready, &buffer_id);
  if (rc != KDI_OK) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_STATE, 0U, 0U);
  }

  b = kdi_dma_buffer_get(driver, buffer_id);
  if (b == NULL || b->state != KDI_DMA_BUF_RX_READY || b->owner != KDI_DMA_OWNER_KERNEL) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_STATE, buffer_id, (b == NULL) ? 0U : b->state);
  }

  *out_buffer_id = buffer_id;
  *out_bytes = b->last_bytes;
  b->last_bytes = 0U;
  b->owner = KDI_DMA_OWNER_DRIVER;
  b->state = KDI_DMA_BUF_IDLE;
  d->stats.cache_invalidate_total++;
  d->stats.rx_poll_total++;
  return kdi_finish(driver, KDI_REQ_DMA, KDI_OK, buffer_id, *out_bytes);
}

int kdi_dma_tx_submit(KdiDriverId driver, KdiCapToken token, uint16_t buffer_id, uint16_t bytes)
{
  KdiDmaDriverState *d;
  KdiDmaBuffer *b;
  int rc;

  if (!kdi_driver_valid(driver) || buffer_id >= KDI_DMA_BUFFER_CAP) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_BAD_ARG, buffer_id, bytes);
  }
  if (!kdi_token_valid(driver, token)) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_AUTH, (uint32_t)token, buffer_id);
  }
  if (!kdi_dma_runtime_allowed(driver)) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_STATE, (uint32_t)kdi_driver_state[driver], buffer_id);
  }
  if (!kdi_dma_policy_allowed(driver)) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_DENIED, buffer_id, bytes);
  }
  if (bytes == 0U) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_BAD_ARG, buffer_id, bytes);
  }

  d = &kdi_dma_state[driver];
  b = kdi_dma_buffer_get(driver, buffer_id);
  if (b == NULL) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_BAD_ARG, buffer_id, bytes);
  }
  if (bytes > b->size) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_BAD_ARG, buffer_id, bytes);
  }
  if (b->owner != KDI_DMA_OWNER_DRIVER || b->state != KDI_DMA_BUF_IDLE) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_STATE, buffer_id, b->state);
  }

  rc = kdi_dma_ring_push(&d->tx_pending, buffer_id);
  if (rc != KDI_OK) {
    d->stats.ring_overflow_total++;
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_LIMIT, buffer_id, d->tx_pending.depth);
  }

  b->owner = KDI_DMA_OWNER_KERNEL;
  b->state = KDI_DMA_BUF_TX_PENDING;
  b->last_bytes = bytes;
  d->stats.cache_clean_total++;
  d->stats.tx_submit_total++;
  return kdi_finish(driver, KDI_REQ_DMA, KDI_OK, buffer_id, bytes);
}

int kdi_dma_tx_complete_one(KdiDriverId driver, KdiCapToken kernel_token, uint16_t *out_buffer_id)
{
  KdiDmaDriverState *d;
  KdiDmaBuffer *b;
  uint16_t buffer_id;
  int rc;

  if (!kdi_driver_valid(driver)) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_BAD_ARG, 0U, 0U);
  }
  if (!kdi_token_valid(KDI_DRIVER_KERNEL, kernel_token)) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_AUTH, (uint32_t)kernel_token, driver);
  }
  if (!kdi_dma_runtime_allowed(driver)) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_STATE, (uint32_t)kdi_driver_state[driver], 0U);
  }
  if (!kdi_dma_policy_allowed(driver)) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_DENIED, 0U, 0U);
  }

  d = &kdi_dma_state[driver];
  if (d->tx_pending.count == 0U) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_STATE, 0U, 0U);
  }
  if (d->tx_done.count >= d->tx_done.depth) {
    d->stats.ring_overflow_total++;
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_LIMIT, d->tx_done.count, d->tx_done.depth);
  }

  buffer_id = d->tx_pending.slots[d->tx_pending.tail];
  b = kdi_dma_buffer_get(driver, buffer_id);
  if (b == NULL || b->state != KDI_DMA_BUF_TX_PENDING || b->owner != KDI_DMA_OWNER_KERNEL) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_STATE, buffer_id, (b == NULL) ? 0U : b->state);
  }

  rc = kdi_dma_ring_pop(&d->tx_pending, &buffer_id);
  if (rc != KDI_OK) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_STATE, 0U, 0U);
  }
  b = kdi_dma_buffer_get(driver, buffer_id);
  if (b == NULL) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_STATE, buffer_id, 0U);
  }
  b->state = KDI_DMA_BUF_TX_DONE;
  rc = kdi_dma_ring_push(&d->tx_done, buffer_id);
  if (rc != KDI_OK) {
    b->state = KDI_DMA_BUF_TX_PENDING;
    d->stats.ring_overflow_total++;
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_LIMIT, d->tx_done.count, d->tx_done.depth);
  }

  d->stats.tx_complete_total++;
  if (out_buffer_id != NULL) {
    *out_buffer_id = buffer_id;
  }
  return kdi_finish(driver, KDI_REQ_DMA, KDI_OK, buffer_id, b->last_bytes);
}

int kdi_dma_tx_poll_complete(KdiDriverId driver, KdiCapToken token, uint16_t *out_buffer_id)
{
  KdiDmaDriverState *d;
  KdiDmaBuffer *b;
  uint16_t buffer_id;
  int rc;

  if (!kdi_driver_valid(driver) || out_buffer_id == NULL) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_BAD_ARG, 0U, 0U);
  }
  if (!kdi_token_valid(driver, token)) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_AUTH, (uint32_t)token, 0U);
  }
  if (!kdi_dma_runtime_allowed(driver)) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_STATE, (uint32_t)kdi_driver_state[driver], 0U);
  }

  d = &kdi_dma_state[driver];
  rc = kdi_dma_ring_pop(&d->tx_done, &buffer_id);
  if (rc != KDI_OK) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_STATE, 0U, 0U);
  }

  b = kdi_dma_buffer_get(driver, buffer_id);
  if (b == NULL || b->state != KDI_DMA_BUF_TX_DONE || b->owner != KDI_DMA_OWNER_KERNEL) {
    return kdi_finish(driver, KDI_REQ_DMA, KDI_ERR_STATE, buffer_id, (b == NULL) ? 0U : b->state);
  }

  b->owner = KDI_DMA_OWNER_DRIVER;
  b->state = KDI_DMA_BUF_IDLE;
  *out_buffer_id = buffer_id;
  d->stats.tx_poll_total++;
  return kdi_finish(driver, KDI_REQ_DMA, KDI_OK, buffer_id, b->last_bytes);
}

int kdi_dma_check_leaks(KdiDriverId driver, KdiCapToken token, KdiDmaLeakReport *out_report)
{
  KdiDmaDriverState *d;

  if (!kdi_driver_valid(driver) || out_report == NULL) {
    return KDI_ERR_BAD_ARG;
  }
  if (!kdi_dma_token_or_kernel_valid(driver, token)) {
    return KDI_ERR_AUTH;
  }

  d = &kdi_dma_state[driver];
  kdi_dma_fill_leak_report(driver, out_report);
  d->stats.leak_check_total++;
  if (out_report->leak != 0U) {
    d->stats.leak_found_total++;
  }
  return KDI_OK;
}

int kdi_dma_get_stats(KdiDriverId driver, KdiDmaStats *out_stats)
{
  if (!kdi_driver_valid(driver) || out_stats == NULL) {
    return KDI_ERR_BAD_ARG;
  }
  *out_stats = kdi_dma_state[driver].stats;
  return KDI_OK;
}

int kdi_dma_get_ring_occupancy(KdiDriverId driver, KdiDmaRingOccupancy *out)
{
  const KdiDmaDriverState *d;
  uint16_t total = 0U;

  if (!kdi_driver_valid(driver) || out == NULL) {
    return KDI_ERR_BAD_ARG;
  }

  d = &kdi_dma_state[driver];
  for (uint16_t i = 0U; i < KDI_DMA_BUFFER_CAP; ++i) {
    if (d->buffers[i].used != 0U) {
      total++;
    }
  }

  memset(out, 0, sizeof(*out));
  out->rx_posted = d->rx_posted.count;
  out->rx_ready = d->rx_ready.count;
  out->tx_pending = d->tx_pending.count;
  out->tx_done = d->tx_done.count;
  out->rx_depth = d->rx_posted.depth;
  out->tx_depth = d->tx_pending.depth;
  out->total_buffers = total;
  return KDI_OK;
}

int kdi_profile_get_driver(KdiDriverId driver, KdiDriverProfileStats *out_profile)
{
  if (!kdi_driver_valid(driver) || out_profile == NULL) {
    return KDI_ERR_BAD_ARG;
  }
  memset(out_profile, 0, sizeof(*out_profile));
  memcpy(out_profile->request_total,
         kdi_profile_req_total[driver],
         sizeof(out_profile->request_total));
  memcpy(out_profile->request_fail,
         kdi_profile_req_fail[driver],
         sizeof(out_profile->request_fail));
  memcpy(out_profile->state_visit,
         kdi_profile_state_visit[driver],
         sizeof(out_profile->state_visit));
  memcpy(out_profile->state_transition,
         kdi_profile_state_transition[driver],
         sizeof(out_profile->state_transition));
  return KDI_OK;
}

int kdi_cap_usage_get(KdiDriverId driver, KdiCapUsageTrace *out_trace)
{
  uint32_t now_ms;

  if (!kdi_driver_valid(driver) || out_trace == NULL) {
    return KDI_ERR_BAD_ARG;
  }

  memset(out_trace, 0, sizeof(*out_trace));
  now_ms = kdi_now_ms();
  out_trace->declared_mask = kdi_declared_cap_mask(driver);
  out_trace->observation_start_ms = kdi_profile_window_start_ms;
  out_trace->observation_end_ms = now_ms;
  out_trace->observation_window_ms = now_ms - kdi_profile_window_start_ms;

  for (uint32_t req = (uint32_t)KDI_REQ_MPU; req <= (uint32_t)KDI_REQ_RESET; ++req) {
    uint32_t total = kdi_profile_req_total[driver][req];
    uint32_t fail = kdi_profile_req_fail[driver][req];
    uint32_t ok = (total >= fail) ? (total - fail) : 0U;

    out_trace->request_total[req] = total;
    out_trace->request_fail[req] = fail;
    out_trace->request_ok[req] = ok;
    if (kdi_profile_req_seen[driver][req] != 0U) {
      out_trace->request_first_ms[req] = kdi_profile_req_first_ms[driver][req];
      out_trace->request_last_ms[req] = kdi_profile_req_last_ms[driver][req];
    }
    if (total != 0U) {
      out_trace->used_mask |= KDI_CAP_REQ_BIT(req);
    }
    if (ok != 0U) {
      out_trace->active_mask |= KDI_CAP_REQ_BIT(req);
    }
  }

  out_trace->declared_not_used_mask = out_trace->declared_mask & (~out_trace->used_mask);
  return KDI_OK;
}

int kdi_fault_domain_get(KdiDriverId driver, KdiFaultDomainStats *out_stats)
{
  KdiFaultDomainState *d;

  if (!kdi_driver_valid(driver) || out_stats == NULL) {
    return KDI_ERR_BAD_ARG;
  }
  d = &kdi_fault_domain[driver];
  out_stats->isolated = d->isolated;
  out_stats->active_fault = d->active_fault;
  out_stats->last_code = d->last_code;
  out_stats->last_detail = d->last_detail;
  out_stats->fault_total = d->fault_total;
  out_stats->contain_total = d->contain_total;
  out_stats->crash_total = d->crash_total;
  out_stats->restart_total = d->restart_total;
  out_stats->generation = d->generation;
  out_stats->last_fault_ms = d->last_fault_ms;
  return KDI_OK;
}

int kdi_report_fault(KdiDriverId driver, KdiCapToken token, const KdiFaultReport *report)
{
  const KdiPolicy *policy;

  if (!kdi_driver_valid(driver) || report == NULL) {
    return kdi_finish(driver, KDI_REQ_FAULT, KDI_ERR_BAD_ARG, 0U, 0U);
  }
  if (!kdi_token_valid(driver, token)) {
    return kdi_finish(driver, KDI_REQ_FAULT, KDI_ERR_AUTH, (uint32_t)token, report->code);
  }
  if (!kdi_state_allows_fault_report(driver)) {
    return kdi_finish(driver, KDI_REQ_FAULT, KDI_ERR_STATE, (uint32_t)kdi_driver_state[driver], report->code);
  }
  policy = &kdi_policy_table[driver].policy;
  if (policy->allow_fault == 0U) {
    return kdi_finish(driver, KDI_REQ_FAULT, KDI_ERR_DENIED, report->code, report->detail);
  }
  if (report->code == 0U) {
    return kdi_finish(driver, KDI_REQ_FAULT, KDI_ERR_BAD_ARG, report->code, report->detail);
  }

  kdi_fault_domain_record(driver, report->code, report->detail);
  kdi_fault_last = *report;
  kdi_fault_driver = driver;
  kdi_fault_valid = 1U;
  kdi_stats.fault_reports++;
  return kdi_finish(driver, KDI_REQ_FAULT, KDI_OK, report->code, report->detail);
}

int kdi_power_hook(KdiDriverId driver, KdiCapToken token, KdiPowerEvent event)
{
  const KdiPolicy *policy;

  if (!kdi_driver_valid(driver)) {
    return kdi_finish(driver, KDI_REQ_POWER, KDI_ERR_BAD_ARG, (uint32_t)event, 0U);
  }
  if (!kdi_token_valid(driver, token)) {
    return kdi_finish(driver, KDI_REQ_POWER, KDI_ERR_AUTH, (uint32_t)token, (uint32_t)event);
  }
  if (!kdi_state_allows_request(driver)) {
    return kdi_finish(driver, KDI_REQ_POWER, KDI_ERR_STATE, (uint32_t)kdi_driver_state[driver], (uint32_t)event);
  }
  policy = &kdi_policy_table[driver].policy;
  if (policy->allow_power == 0U) {
    return kdi_finish(driver, KDI_REQ_POWER, KDI_ERR_DENIED, (uint32_t)event, 0U);
  }
  if (event < KDI_POWER_ONLINE || event > KDI_POWER_OFFLINE) {
    return kdi_finish(driver, KDI_REQ_POWER, KDI_ERR_BAD_ARG, (uint32_t)event, 0U);
  }
  return kdi_finish(driver, KDI_REQ_POWER, KDI_OK, (uint32_t)event, 0U);
}

int kdi_reset_hook(KdiDriverId driver, KdiCapToken token, KdiResetEvent event)
{
  const KdiPolicy *policy;

  if (!kdi_driver_valid(driver)) {
    return kdi_finish(driver, KDI_REQ_RESET, KDI_ERR_BAD_ARG, (uint32_t)event, 0U);
  }
  if (!kdi_token_valid(driver, token)) {
    return kdi_finish(driver, KDI_REQ_RESET, KDI_ERR_AUTH, (uint32_t)token, (uint32_t)event);
  }
  if (!kdi_state_allows_request(driver)) {
    return kdi_finish(driver, KDI_REQ_RESET, KDI_ERR_STATE, (uint32_t)kdi_driver_state[driver], (uint32_t)event);
  }
  policy = &kdi_policy_table[driver].policy;
  if (policy->allow_reset == 0U) {
    return kdi_finish(driver, KDI_REQ_RESET, KDI_ERR_DENIED, (uint32_t)event, 0U);
  }
  if (event < KDI_RESET_ASSERT || event > KDI_RESET_COMPLETE) {
    return kdi_finish(driver, KDI_REQ_RESET, KDI_ERR_BAD_ARG, (uint32_t)event, 0U);
  }
  return kdi_finish(driver, KDI_REQ_RESET, KDI_OK, (uint32_t)event, 0U);
}

int kdi_last_decision(KdiDecision *out)
{
  if (out == NULL) {
    return KDI_ERR_BAD_ARG;
  }
  if (kdi_last.valid == 0U) {
    return KDI_ERR_UNSUPPORTED;
  }
  *out = kdi_last;
  return KDI_OK;
}

void kdi_get_stats(KdiStats *out)
{
  if (out == NULL) {
    return;
  }
  *out = kdi_stats;
}

int kdi_last_fault(KdiDriverId *driver, KdiFaultReport *out)
{
  if (driver == NULL || out == NULL) {
    return KDI_ERR_BAD_ARG;
  }
  if (kdi_fault_valid == 0U) {
    return KDI_ERR_UNSUPPORTED;
  }
  *driver = kdi_fault_driver;
  *out = kdi_fault_last;
  return KDI_OK;
}
