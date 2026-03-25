#ifndef KDI_H
#define KDI_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
  KDI_DRIVER_KERNEL = 0,
  KDI_DRIVER_UART = 1,
  KDI_DRIVER_SENSOR = 2,
  KDI_DRIVER_VM_RUNTIME = 3,
  KDI_DRIVER_DIAG = 4,
  KDI_DRIVER_COUNT
} KdiDriverId;

typedef enum {
  KDI_REQ_NONE = 0,
  KDI_REQ_MPU = 1,
  KDI_REQ_IRQ = 2,
  KDI_REQ_DMA = 3,
  KDI_REQ_FAULT = 4,
  KDI_REQ_POWER = 5,
  KDI_REQ_RESET = 6
} KdiRequestType;

#define KDI_PROFILE_REQ_COUNT ((uint32_t)KDI_REQ_RESET + 1U)

typedef enum {
  KDI_OK = 0,
  KDI_ERR_BAD_ARG = -1,
  KDI_ERR_DENIED = -2,
  KDI_ERR_LIMIT = -3,
  KDI_ERR_UNSUPPORTED = -4,
  KDI_ERR_AUTH = -5,
  KDI_ERR_STATE = -6
} KdiResult;

typedef uint32_t KdiCapToken;
typedef uint32_t (*KdiNowMsFn)(void);

#define KDI_CAP_INVALID ((KdiCapToken)0U)
#define KDI_CAP_REQ_BIT(req) (1UL << (uint32_t)(req))

typedef struct {
  uint8_t allow_mpu;
  uint8_t allow_irq;
  uint8_t allow_dma;
  uint8_t allow_fault;
  uint8_t allow_power;
  uint8_t allow_reset;
  uint8_t max_mpu_regions;
  int16_t min_irqn;
  int16_t max_irqn;
  uint32_t max_dma_bytes;
} KdiPolicy;

typedef struct {
  uint8_t region_index;
  uint32_t base;
  uint32_t size;
  uint32_t attrs;
} KdiMpuRequest;

typedef struct {
  int16_t irqn;
  uint8_t priority;
} KdiIrqRequest;

typedef struct {
  uintptr_t base;
  uint32_t size;
  uint8_t align;
  uint8_t direction;
} KdiDmaRequest;

typedef enum {
  KDI_DMA_OWNER_DRIVER = 0,
  KDI_DMA_OWNER_KERNEL = 1
} KdiDmaOwner;

typedef struct {
  uint16_t code;
  uint32_t detail;
} KdiFaultReport;

typedef enum {
  KDI_POWER_ONLINE = 1,
  KDI_POWER_SUSPEND = 2,
  KDI_POWER_RESUME = 3,
  KDI_POWER_OFFLINE = 4
} KdiPowerEvent;

typedef enum {
  KDI_RESET_ASSERT = 1,
  KDI_RESET_DEASSERT = 2,
  KDI_RESET_COMPLETE = 3
} KdiResetEvent;

typedef enum {
  KDI_IRQ_UNSAFE_MALLOC = 1,
  KDI_IRQ_UNSAFE_PRINTF = 2,
  KDI_IRQ_UNSAFE_POLICY = 3
} KdiIrqUnsafeOp;

typedef int (*KdiIrqWorkFn)(KdiDriverId driver, uint16_t work_id, uint32_t arg, void *ctx);

typedef enum {
  KDI_STATE_INIT = 0,
  KDI_STATE_PROBE = 1,
  KDI_STATE_READY = 2,
  KDI_STATE_ACTIVE = 3,
  KDI_STATE_ERROR = 4,
  KDI_STATE_RESET = 5,
  KDI_STATE_DEAD = 6
} KdiDriverState;

#define KDI_PROFILE_STATE_COUNT ((uint32_t)KDI_STATE_DEAD + 1U)

typedef struct {
  uint8_t valid;
  uint8_t driver;
  uint8_t req;
  int32_t rc;
  uint32_t arg0;
  uint32_t arg1;
} KdiDecision;

typedef struct {
  uint32_t allow_total;
  uint32_t deny_total;
  uint32_t reject_total;
  uint32_t fault_reports;
  uint32_t auth_fail_total;
  uint32_t token_rotate_total;
  uint32_t token_revoke_total;
  uint32_t token_expire_total;
  uint32_t state_fail_total;
  uint32_t force_reclaim_total;
} KdiStats;

typedef struct {
  uint32_t irq_enter_total;
  uint32_t irq_throttle_total;
  uint32_t irq_cooldown_total;
  uint32_t irq_recover_total;
  uint32_t irq_defer_total;
  uint32_t irq_drop_total;
  uint32_t irq_worker_total;
  uint32_t irq_worker_error_total;
  uint32_t irq_starvation_total;
  uint32_t irq_unsafe_total;
  uint32_t deferred_pending;
  uint32_t starvation_ms;
} KdiIrqStats;

typedef struct {
  uint8_t in_irq;
  uint8_t throttled;
  uint8_t worker_registered;
  uint8_t cooldown_level;
  uint32_t budget_per_sec;
  uint32_t window_count;
  uint32_t cooldown_base_ms;
  uint32_t cooldown_max_ms;
  uint32_t cooldown_until_ms;
  uint32_t deferred_pending;
} KdiIrqDriverStats;

typedef struct {
  uint32_t irq_enter_total;
  uint32_t irq_throttle_total;
  uint32_t irq_defer_total;
  uint32_t irq_drop_total;
  uint32_t irq_worker_total;
  uint32_t irq_worker_error_total;
  uint32_t irq_unsafe_total;
} KdiIrqDriverCounters;

typedef struct {
  uint16_t rx_depth;
  uint16_t tx_depth;
} KdiDmaRingConfig;

typedef struct {
  uint32_t buffers_registered;
  uint32_t buffers_unregistered;
  uint32_t rx_post_total;
  uint32_t rx_complete_total;
  uint32_t rx_poll_total;
  uint32_t tx_submit_total;
  uint32_t tx_complete_total;
  uint32_t tx_poll_total;
  uint32_t cache_clean_total;
  uint32_t cache_invalidate_total;
  uint32_t ring_overflow_total;
  uint32_t leak_check_total;
  uint32_t leak_found_total;
} KdiDmaStats;

typedef struct {
  uint16_t rx_posted;
  uint16_t rx_ready;
  uint16_t tx_pending;
  uint16_t tx_done;
  uint16_t rx_depth;
  uint16_t tx_depth;
  uint16_t total_buffers;
  uint16_t reserved;
} KdiDmaRingOccupancy;

typedef struct {
  uint16_t total_buffers;
  uint16_t driver_owned;
  uint16_t kernel_owned;
  uint16_t rx_posted;
  uint16_t rx_ready;
  uint16_t tx_pending;
  uint16_t tx_done;
  uint8_t leak;
} KdiDmaLeakReport;

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
} KdiFaultDomainStats;

typedef struct {
  uint32_t request_total[KDI_PROFILE_REQ_COUNT];
  uint32_t request_fail[KDI_PROFILE_REQ_COUNT];
  uint32_t state_visit[KDI_PROFILE_STATE_COUNT];
  uint32_t state_transition[KDI_PROFILE_STATE_COUNT][KDI_PROFILE_STATE_COUNT];
} KdiDriverProfileStats;

typedef struct {
  uint32_t declared_mask;
  uint32_t used_mask;
  uint32_t active_mask;
  uint32_t declared_not_used_mask;
  uint32_t observation_start_ms;
  uint32_t observation_end_ms;
  uint32_t observation_window_ms;
  uint32_t request_total[KDI_PROFILE_REQ_COUNT];
  uint32_t request_fail[KDI_PROFILE_REQ_COUNT];
  uint32_t request_ok[KDI_PROFILE_REQ_COUNT];
  uint32_t request_first_ms[KDI_PROFILE_REQ_COUNT];
  uint32_t request_last_ms[KDI_PROFILE_REQ_COUNT];
} KdiCapUsageTrace;

void kdi_init(void);
void kdi_set_now_ms_fn(KdiNowMsFn fn);
void kdi_set_now_ms(uint32_t now_ms);
int kdi_acquire_token(KdiDriverId driver, KdiCapToken *out_token);
int kdi_rotate_token(KdiDriverId driver, KdiCapToken *out_token);
int kdi_revoke_token(KdiDriverId driver);
int kdi_token_is_active(KdiDriverId driver, uint8_t *out_active);
int kdi_set_token_ttl_ms(KdiDriverId driver, uint32_t ttl_ms);
int kdi_get_token_ttl_ms(KdiDriverId driver, uint32_t *out_ttl_ms);
int kdi_token_remaining_ms(KdiDriverId driver, uint32_t *out_remaining_ms);
const char *kdi_driver_state_name(KdiDriverState state);
int kdi_driver_get_state(KdiDriverId driver, KdiDriverState *out_state);
int kdi_driver_probe(KdiDriverId driver, KdiCapToken token);
int kdi_driver_probe_done(KdiDriverId driver, KdiCapToken token, uint8_t ok);
int kdi_driver_activate(KdiDriverId driver, KdiCapToken token);
int kdi_driver_runtime_error(KdiDriverId driver, KdiCapToken token, uint16_t code);
int kdi_driver_reset(KdiDriverId driver, KdiCapToken kernel_token);
int kdi_driver_reinit(KdiDriverId driver, KdiCapToken kernel_token, KdiCapToken *out_token);
int kdi_driver_force_reclaim(KdiDriverId driver, KdiCapToken kernel_token);
int kdi_irq_set_budget_per_sec(KdiDriverId driver, uint32_t budget_per_sec);
int kdi_irq_get_budget_per_sec(KdiDriverId driver, uint32_t *out_budget_per_sec);
int kdi_irq_set_cooldown_ms(KdiDriverId driver, uint32_t base_ms, uint32_t max_ms);
int kdi_irq_set_starvation_ms(uint32_t starvation_ms);
int kdi_irq_set_worker(KdiDriverId driver, KdiCapToken token, KdiIrqWorkFn fn, void *ctx);
int kdi_irq_enter(KdiDriverId driver, KdiCapToken token);
int kdi_irq_defer(KdiDriverId driver, KdiCapToken token, uint16_t work_id, uint32_t arg);
int kdi_irq_exit(KdiDriverId driver, KdiCapToken token);
int kdi_irq_worker_run(uint32_t max_items, uint32_t *out_processed);
int kdi_irq_poll_starvation(uint32_t *out_detected);
int kdi_irq_unsafe_op(KdiDriverId driver, KdiCapToken token, KdiIrqUnsafeOp op);
void kdi_irq_get_stats(KdiIrqStats *out);
int kdi_irq_get_driver_stats(KdiDriverId driver, KdiIrqDriverStats *out);
int kdi_irq_get_driver_counters(KdiDriverId driver, KdiIrqDriverCounters *out);
const KdiPolicy *kdi_get_policy(KdiDriverId driver);
const char *kdi_driver_name(KdiDriverId driver);
const char *kdi_request_name(KdiRequestType req);
const char *kdi_result_name(int rc);

int kdi_request_mpu_region(KdiDriverId driver, KdiCapToken token, const KdiMpuRequest *req);
int kdi_request_irq(KdiDriverId driver, KdiCapToken token, const KdiIrqRequest *req);
int kdi_declare_dma_buffer(KdiDriverId driver, KdiCapToken token, const KdiDmaRequest *req);
int kdi_dma_ring_configure(KdiDriverId driver, KdiCapToken token, const KdiDmaRingConfig *cfg);
int kdi_dma_register_buffer(KdiDriverId driver, KdiCapToken token, const KdiDmaRequest *req, uint16_t *out_buffer_id);
int kdi_dma_unregister_buffer(KdiDriverId driver, KdiCapToken token, uint16_t buffer_id);
int kdi_dma_get_owner(KdiDriverId driver, KdiCapToken token, uint16_t buffer_id, KdiDmaOwner *out_owner);
int kdi_dma_rx_post_buffer(KdiDriverId driver, KdiCapToken token, uint16_t buffer_id);
int kdi_dma_rx_complete_one(KdiDriverId driver, KdiCapToken kernel_token, uint16_t bytes, uint16_t *out_buffer_id);
int kdi_dma_rx_poll(KdiDriverId driver, KdiCapToken token, uint16_t *out_buffer_id, uint16_t *out_bytes);
int kdi_dma_tx_submit(KdiDriverId driver, KdiCapToken token, uint16_t buffer_id, uint16_t bytes);
int kdi_dma_tx_complete_one(KdiDriverId driver, KdiCapToken kernel_token, uint16_t *out_buffer_id);
int kdi_dma_tx_poll_complete(KdiDriverId driver, KdiCapToken token, uint16_t *out_buffer_id);
int kdi_dma_check_leaks(KdiDriverId driver, KdiCapToken token, KdiDmaLeakReport *out_report);
int kdi_dma_get_stats(KdiDriverId driver, KdiDmaStats *out_stats);
int kdi_dma_get_ring_occupancy(KdiDriverId driver, KdiDmaRingOccupancy *out);
int kdi_profile_get_driver(KdiDriverId driver, KdiDriverProfileStats *out_profile);
int kdi_cap_usage_get(KdiDriverId driver, KdiCapUsageTrace *out_trace);
int kdi_fault_domain_get(KdiDriverId driver, KdiFaultDomainStats *out_stats);
int kdi_report_fault(KdiDriverId driver, KdiCapToken token, const KdiFaultReport *report);
int kdi_power_hook(KdiDriverId driver, KdiCapToken token, KdiPowerEvent event);
int kdi_reset_hook(KdiDriverId driver, KdiCapToken token, KdiResetEvent event);

int kdi_last_decision(KdiDecision *out);
void kdi_get_stats(KdiStats *out);
int kdi_last_fault(KdiDriverId *driver, KdiFaultReport *out);

#endif
