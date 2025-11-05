#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include "board.h"
#include "mpu_demo.h"
#include "static_assert.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "mpu_wrappers.h"
#include "stm32f4xx.h"
#include "vm32.h"
#include "fault.h"
#include "scenario.h"
#include "sonic_lite.h"
#include "kdi.h"
#include "dependency_graph.h"
#include "bringup_phase.h"
#include "analysis_engine.h"

#define LOG_QUEUE_LEN 8
#define USER_STACK_WORDS 512
#define CLI_STACK_WORDS 1024
#define USER_STACK_ALIGN_BYTES (USER_STACK_WORDS * sizeof(StackType_t))
#define USER_STACK_WORDS_IS_POW2 ((USER_STACK_WORDS & (USER_STACK_WORDS - 1)) == 0)
#define USER_STACK_ALIGN_IS_POW2 ((USER_STACK_ALIGN_BYTES & (USER_STACK_ALIGN_BYTES - 1)) == 0)
#define USER_STACK_ALIGN_MIN_OK (USER_STACK_ALIGN_BYTES >= 128U)
#define CLI_LINE_MAX 192U
#define BRINGUP_CLK_MIN_HZ 76000000UL
#define BRINGUP_CLK_MAX_HZ 90000000UL
#define SONIC_AUDIT_CAP 12U
#define SONIC_AUDIT_MSG_MAX 64U
#define SNAPSHOT_EVENT_CAP 64U
#define SNAPSHOT_EVENT_MSG_MAX 80U
#define SNAPSHOT_EVENT_WINDOW_MS 5000U
#define SNAPSHOT_EVT_BOUNDARY_FAULT 0x0001U
#define SNAPSHOT_EVT_BOUNDARY_RESET 0x0002U
#define SNAPSHOT_EVT_BOUNDARY_STAGE 0x0004U
#define SNAPSHOT_SLICE_START_FIRST 0x0001U
#define SNAPSHOT_SLICE_START_CORR 0x0002U
#define SNAPSHOT_SLICE_START_RESET 0x0004U
#define SNAPSHOT_SLICE_START_FAULT 0x0008U
#define SNAPSHOT_SLICE_START_STAGE 0x0010U
#define SNAPSHOT_FEATURE_LOOKBACK_MS SNAPSHOT_EVENT_WINDOW_MS
#define SNAPSHOT_FEATURE_IRQ_ABN_PER_SEC 8U
#define SNAPSHOT_FEATURE_KDI_FAIL_FREQ_THRESHOLD 2U
#define SNAPSHOT_HYP_EVIDENCE_CAP 6U
#define DRIVER_PROFILE_IRQ_BIN_COUNT 6U
#define DRIVER_PROFILE_DMA_BIN_COUNT 6U

#if BOARD_UART_PORT == 1
#define KDI_BOOT_UART_IRQn USART1_IRQn
#elif BOARD_UART_PORT == 2
#define KDI_BOOT_UART_IRQn USART2_IRQn
#elif BOARD_UART_PORT == 3
#define KDI_BOOT_UART_IRQn USART3_IRQn
#else
#error "BOARD_UART_PORT must be 1, 2, or 3"
#endif

STATIC_ASSERT(USER_STACK_WORDS_IS_POW2, user_stack_words_pow2);
STATIC_ASSERT(USER_STACK_ALIGN_IS_POW2, user_stack_align_pow2);
STATIC_ASSERT(USER_STACK_ALIGN_MIN_OK, user_stack_align_min);

static QueueHandle_t log_queue;
static QueueHandle_t ipc_cmd_queue;
static QueueHandle_t ipc_resp_queue;
static BringupPhaseModel bringup_phase_model;
static uint8_t bringup_user_task_created;
static Vm32 vm32;
static SonicLiteState sonic_lite;
static SonicLitePlane sonic_rollback_snapshot;
static SonicLitePlane sonic_preset_stage_snapshot;
static uint8_t sonic_preset_stage_dirty_snapshot;
static uint8_t sonic_confirm_pending;
static TickType_t sonic_confirm_deadline;
static uint32_t sonic_confirm_window_ms;
typedef struct {
  KdiCapToken kernel;
  KdiCapToken diag;
  KdiCapToken uart;
  KdiCapToken sensor;
  KdiCapToken vm_runtime;
} KdiBootCaps;
static KdiBootCaps kdi_caps;
typedef struct {
  TickType_t tick;
  char msg[SONIC_AUDIT_MSG_MAX];
} SonicAuditEntry;
typedef struct {
  uint32_t ts_ms;
  uint32_t corr_id;
  uint16_t boundary_flags;
  uint8_t stage_valid;
  uint8_t stage_id;
  char msg[SNAPSHOT_EVENT_MSG_MAX];
} SnapshotEvent;
typedef struct {
  uint8_t begin;
  uint8_t end;
  uint8_t stage_valid;
  uint8_t stage_id;
  uint8_t fault_events;
  uint8_t reset_events;
  uint16_t start_reason;
  uint32_t corr_id;
} SnapshotEventSlice;
typedef AnalysisFaultFeatureVector SnapshotFaultFeatureVector;
typedef struct {
  uint8_t irq_prev_valid;
  uint32_t irq_prev_enter_total;
  uint32_t irq_sample_count;
  uint32_t dma_sample_count;
  uint32_t irq_rate_bins[DRIVER_PROFILE_IRQ_BIN_COUNT];
  uint32_t dma_rx_occ_bins[DRIVER_PROFILE_DMA_BIN_COUNT];
  uint32_t dma_tx_occ_bins[DRIVER_PROFILE_DMA_BIN_COUNT];
} DriverProfileRuntime;
static SonicAuditEntry sonic_audit[SONIC_AUDIT_CAP];
static uint8_t sonic_audit_head;
static uint8_t sonic_audit_count;
static SnapshotEvent snapshot_events[SNAPSHOT_EVENT_CAP];
static uint8_t snapshot_event_head;
static uint8_t snapshot_event_count;
static SnapshotEvent snapshot_work_events[SNAPSHOT_EVENT_CAP];
static SnapshotEventSlice snapshot_work_slices[SNAPSHOT_EVENT_CAP];
static uint8_t snapshot_work_indices[SNAPSHOT_EVENT_CAP];
static AnalysisEngine snapshot_analysis_engine;
static DriverProfileRuntime driver_profile_runtime[KDI_DRIVER_COUNT];
static uint32_t driver_profile_start_ms;
static uint32_t driver_profile_last_sample_ms;
static void log_status(void);
static void log_send_blocking(const char *msg);
static void sonic_clear_confirm_window(void);
static void driver_profile_sample_now(void);
static void snapshot_hyp_evidence_text(const uint8_t *ids,
                                       uint8_t count,
                                       char *buf,
                                       size_t buf_size);
/* MPU region base must be aligned to its size. */
static StackType_t user_stack[USER_STACK_WORDS]
  __attribute__((aligned(USER_STACK_ALIGN_BYTES)));

volatile SharedAdcData g_shared_adc
  __attribute__((section(".shared_data"), aligned(SHARED_ADC_REGION_SIZE)));
volatile SharedControl g_shared_ctrl
  __attribute__((section(".shared_ctrl"), aligned(SHARED_CTRL_REGION_SIZE)));
volatile uint8_t g_shared_stats[STATS_BUF_REGION_SIZE]
  __attribute__((section(".shared_stats"), aligned(STATS_BUF_REGION_SIZE)));

extern void vMPUClearKernelObjectPool(void);

static void enable_fpu(void)
{
  SCB->CPACR |= (0xFU << 20);
  __DSB();
  __ISB();
}

static uint32_t kdi_now_ms_from_rtos(void)
{
  return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

#if (configSUPPORT_STATIC_ALLOCATION == 1)
static PRIVILEGED_DATA StaticTask_t idle_tcb;
static PRIVILEGED_DATA StackType_t idle_stack[configMINIMAL_STACK_SIZE];

void vApplicationGetIdleTaskMemory(StaticTask_t **ppxIdleTaskTCBBuffer,
                                   StackType_t **ppxIdleTaskStackBuffer,
                                   configSTACK_DEPTH_TYPE *puxIdleTaskStackSize)
{
  *ppxIdleTaskTCBBuffer = &idle_tcb;
  *ppxIdleTaskStackBuffer = idle_stack;
  *puxIdleTaskStackSize = configMINIMAL_STACK_SIZE;
}
#endif

static void sensor_task(void *arg)
{
  (void)arg;
  uint32_t idx = 0;

  for (;;) {
    uint16_t raw = board_adc_read_temp_raw();
    g_shared_adc.samples[idx] = raw;
    idx = (idx + 1U) % SHARED_ADC_SAMPLES;
    g_shared_adc.seq++;
    vTaskDelay(pdMS_TO_TICKS(g_sample_period_ms));
  }
}

static int snapshot_msg_token_char(char c)
{
  return (isalnum((unsigned char)c) || c == '-' || c == '_') ? 1 : 0;
}

static uint8_t snapshot_msg_parse_u32_field(const char *msg, const char *key, uint32_t *out)
{
  const char *p;
  uint32_t v = 0U;
  uint8_t saw_digit = 0U;

  if (msg == NULL || key == NULL || key[0] == '\0' || out == NULL) {
    return 0U;
  }

  p = strstr(msg, key);
  if (p == NULL) {
    return 0U;
  }
  p += strlen(key);
  while (*p != '\0' && isdigit((unsigned char)*p)) {
    saw_digit = 1U;
    v = (v * 10U) + (uint32_t)(*p - '0');
    p++;
  }
  if (saw_digit == 0U) {
    return 0U;
  }
  *out = v;
  return 1U;
}

static int snapshot_msg_contains_token(const char *msg, const char *token)
{
  const char *p;
  size_t token_len;

  if (msg == NULL || token == NULL || token[0] == '\0') {
    return 0;
  }

  token_len = strlen(token);
  p = msg;
  while ((p = strstr(p, token)) != NULL) {
    char left = (p > msg) ? p[-1] : '\0';
    char right = p[token_len];
    if (!snapshot_msg_token_char(left) && !snapshot_msg_token_char(right)) {
      return 1;
    }
    p += token_len;
  }

  return 0;
}

static uint8_t snapshot_event_find_driver(const char *msg, KdiDriverId *out_driver)
{
  if (msg == NULL || out_driver == NULL) {
    return 0U;
  }
  if (snapshot_msg_contains_token(msg, "vm-runtime") ||
      snapshot_msg_contains_token(msg, "vm")) {
    *out_driver = KDI_DRIVER_VM_RUNTIME;
    return 1U;
  }
  if (snapshot_msg_contains_token(msg, "kernel")) {
    *out_driver = KDI_DRIVER_KERNEL;
    return 1U;
  }
  if (snapshot_msg_contains_token(msg, "uart")) {
    *out_driver = KDI_DRIVER_UART;
    return 1U;
  }
  if (snapshot_msg_contains_token(msg, "sensor")) {
    *out_driver = KDI_DRIVER_SENSOR;
    return 1U;
  }
  if (snapshot_msg_contains_token(msg, "diag")) {
    *out_driver = KDI_DRIVER_DIAG;
    return 1U;
  }
  return 0U;
}

static uint8_t snapshot_event_find_stage(const char *msg, BringupPhaseId *out_stage)
{
  if (msg == NULL || out_stage == NULL || strstr(msg, "bringup") == NULL) {
    return 0U;
  }

  for (BringupPhaseId phase = BRINGUP_PHASE_ROM_EARLY_INIT;
       phase < BRINGUP_PHASE_COUNT;
       ++phase) {
    const char *name = bringup_phase_name(phase);
    if (name != NULL && strstr(msg, name) != NULL) {
      *out_stage = phase;
      return 1U;
    }
  }
  return 0U;
}

static uint8_t snapshot_event_is_fault_msg(const char *msg)
{
  if (msg == NULL) {
    return 0U;
  }
  if (strstr(msg, "\"type\":\"fault\"") != NULL) {
    return 1U;
  }
  if (strncmp(msg, "fault ", 6) == 0 || strcmp(msg, "fault") == 0) {
    return 1U;
  }
  if (strncmp(msg, "kdi fault ", 10) == 0) {
    return (strstr(msg, "last: none") == NULL) ? 1U : 0U;
  }
  if (strncmp(msg, "Fault:", 6) == 0 ||
      strncmp(msg, "HardFault", 9) == 0 ||
      strncmp(msg, "MemManage", 9) == 0 ||
      strncmp(msg, "BusFault", 8) == 0 ||
      strncmp(msg, "UsageFault", 10) == 0) {
    return 1U;
  }
  return 0U;
}

static uint8_t snapshot_event_is_reset_msg(const char *msg)
{
  if (msg == NULL) {
    return 0U;
  }
  if (strstr(msg, "kdi driver reset ") != NULL) {
    return 1U;
  }
  if (strstr(msg, "bringup phase reset") != NULL) {
    return 1U;
  }
  return 0U;
}

static uint32_t snapshot_event_hash_corr_id(const char *msg)
{
  uint32_t hash = 2166136261UL;

  if (msg == NULL) {
    return 1U;
  }

  while (*msg != '\0') {
    hash ^= (uint32_t)(uint8_t)(*msg);
    hash *= 16777619UL;
    msg++;
  }
  if (hash == 0U) {
    hash = 1U;
  }
  return hash;
}

static uint32_t snapshot_event_corr_id(const char *msg)
{
  KdiDriverId driver = KDI_DRIVER_KERNEL;
  BringupPhaseId stage = BRINGUP_PHASE_ROM_EARLY_INIT;

  if (snapshot_event_find_stage(msg, &stage)) {
    return 0x10000000UL | (uint32_t)stage;
  }
  if (snapshot_event_is_fault_msg(msg)) {
    if (snapshot_event_find_driver(msg, &driver)) {
      return 0xF0000000UL | (uint32_t)driver;
    }
    return 0xF0000001UL;
  }
  if (snapshot_event_is_reset_msg(msg)) {
    if (snapshot_event_find_driver(msg, &driver)) {
      return 0xE0000000UL | (uint32_t)driver;
    }
    return 0xE0000001UL;
  }
  if (msg != NULL && strncmp(msg, "kdi ", 4) == 0) {
    if (snapshot_event_find_driver(msg, &driver)) {
      return 0x20000000UL | (uint32_t)driver;
    }
    return 0x20FFFF00UL;
  }
  if (msg != NULL && strncmp(msg, "dep ", 4) == 0) {
    if (snapshot_event_find_driver(msg, &driver)) {
      return 0x21000000UL | (uint32_t)driver;
    }
    return 0x21FFFF00UL;
  }
  if (msg != NULL &&
      (strncmp(msg, "vm ", 3) == 0 || strncmp(msg, "scenario ", 9) == 0)) {
    return 0x30000000UL;
  }
  if (msg != NULL && strncmp(msg, "bringup ", 8) == 0) {
    return 0x11000000UL;
  }
  if (msg != NULL && strncmp(msg, "ipc ", 4) == 0) {
    return 0x40000000UL;
  }
  return snapshot_event_hash_corr_id(msg);
}

static uint16_t snapshot_event_boundary_flags(const char *msg,
                                              uint8_t *out_stage_valid,
                                              uint8_t *out_stage_id)
{
  uint16_t flags = 0U;
  BringupPhaseId stage = BRINGUP_PHASE_ROM_EARLY_INIT;

  if (out_stage_valid != NULL) {
    *out_stage_valid = 0U;
  }
  if (out_stage_id != NULL) {
    *out_stage_id = 0U;
  }

  if (snapshot_event_is_fault_msg(msg)) {
    flags |= SNAPSHOT_EVT_BOUNDARY_FAULT;
  }
  if (snapshot_event_is_reset_msg(msg)) {
    flags |= SNAPSHOT_EVT_BOUNDARY_RESET;
  }
  if (snapshot_event_find_stage(msg, &stage)) {
    if (out_stage_valid != NULL) {
      *out_stage_valid = 1U;
    }
    if (out_stage_id != NULL) {
      *out_stage_id = (uint8_t)stage;
    }
    if (msg != NULL && strstr(msg, "bringup phase") != NULL) {
      flags |= SNAPSHOT_EVT_BOUNDARY_STAGE;
    }
  }

  return flags;
}

static uint8_t snapshot_corr_id_to_driver(uint32_t corr_id, KdiDriverId *out_driver)
{
  uint32_t prefix = corr_id & 0xFF000000UL;
  uint32_t id = corr_id & 0x000000FFUL;

  if (out_driver == NULL || id >= (uint32_t)KDI_DRIVER_COUNT) {
    return 0U;
  }

  if (prefix == 0xF0000000UL ||
      prefix == 0xE0000000UL ||
      prefix == 0x20000000UL ||
      prefix == 0x21000000UL) {
    *out_driver = (KdiDriverId)id;
    return 1U;
  }
  return 0U;
}

static uint8_t snapshot_event_is_irq_related(const char *msg)
{
  if (msg == NULL) {
    return 0U;
  }
  if (strncmp(msg, "kdi irq ", 8) == 0) {
    return 1U;
  }
  return snapshot_msg_contains_token(msg, "irq") ? 1U : 0U;
}

static uint8_t snapshot_event_is_irq_anomaly_hint(const char *msg)
{
  if (msg == NULL) {
    return 0U;
  }
  if ((strstr(msg, "irq storm") != NULL) ||
      (strstr(msg, "starve") != NULL) ||
      (strstr(msg, "throttle") != NULL) ||
      (strstr(msg, "irq unsafe") != NULL)) {
    return 1U;
  }
  if (snapshot_event_is_irq_related(msg) &&
      (strstr(msg, "rc=limit") != NULL || strstr(msg, ": limit") != NULL)) {
    return 1U;
  }
  return 0U;
}

static uint8_t snapshot_event_is_kdi_failure(const char *msg)
{
  uint32_t fail_count = 0U;

  if (msg == NULL || strncmp(msg, "kdi ", 4) != 0) {
    return 0U;
  }
  if (snapshot_msg_parse_u32_field(msg, " fail=", &fail_count) != 0U && fail_count != 0U) {
    return 1U;
  }
  if (strstr(msg, "rc=auth") != NULL ||
      strstr(msg, "rc=denied") != NULL ||
      strstr(msg, "rc=state") != NULL ||
      strstr(msg, "rc=limit") != NULL ||
      strstr(msg, "rc=bad_arg") != NULL ||
      strstr(msg, "rc=unsupported") != NULL ||
      strstr(msg, "rc=unknown") != NULL) {
    return 1U;
  }
  if (strstr(msg, ": auth") != NULL ||
      strstr(msg, ": denied") != NULL ||
      strstr(msg, ": state") != NULL ||
      strstr(msg, ": limit") != NULL ||
      strstr(msg, ": bad_arg") != NULL ||
      strstr(msg, ": unsupported") != NULL) {
    return 1U;
  }
  return 0U;
}

static uint8_t snapshot_event_is_state_error(const char *msg)
{
  if (msg == NULL) {
    return 0U;
  }
  if (strstr(msg, "kdi driver error ") != NULL) {
    return 1U;
  }
  if (strstr(msg, " st=error") != NULL || strstr(msg, " state=error") != NULL) {
    return 1U;
  }
  return 0U;
}

static uint8_t snapshot_event_is_state_reset(const char *msg)
{
  if (msg == NULL) {
    return 0U;
  }
  if (strstr(msg, "kdi driver reset ") != NULL) {
    return 1U;
  }
  if (strstr(msg, " st=reset") != NULL || strstr(msg, " state=reset") != NULL) {
    return 1U;
  }
  return 0U;
}

static uint8_t snapshot_event_has_cap_eperm(const char *msg)
{
  if (msg == NULL) {
    return 0U;
  }
  if (strstr(msg, "EPERM") != NULL || strstr(msg, "eperm") != NULL) {
    return 1U;
  }
  if (strncmp(msg, "kdi ", 4) == 0 &&
      (strstr(msg, ": auth") != NULL ||
       strstr(msg, ": denied") != NULL ||
       strstr(msg, "last=auth") != NULL ||
       strstr(msg, "rc=auth") != NULL ||
       strstr(msg, "rc=denied") != NULL)) {
    return 1U;
  }
  return 0U;
}

static uint8_t snapshot_event_has_mpu_violation(const char *msg)
{
  if (msg == NULL) {
    return 0U;
  }
  if (strstr(msg, "Fault(cpu:MemManage") != NULL) {
    return 1U;
  }
  if (strstr(msg, "\"type\":\"fault\"") != NULL &&
      (strstr(msg, "\"name\":\"MemManage\"") != NULL ||
       strstr(msg, "\"mpu_region\"") != NULL)) {
    return 1U;
  }
  if (strstr(msg, "mpu_region=") != NULL && strstr(msg, "Fault(cpu") != NULL) {
    return 1U;
  }
  if (strstr(msg, "MPU violation") != NULL &&
      (strstr(msg, "fault") != NULL || strstr(msg, "Fault") != NULL)) {
    return 1U;
  }
  return 0U;
}

static uint32_t snapshot_slice_fault_ts_ms(const SnapshotEventSlice *slice,
                                           const SnapshotEvent *events)
{
  if (slice == NULL || events == NULL) {
    return 0U;
  }
  for (uint8_t i = slice->begin; i <= slice->end; ++i) {
    const SnapshotEvent *event = &events[i];
    if ((event->boundary_flags & SNAPSHOT_EVT_BOUNDARY_FAULT) != 0U) {
      return event->ts_ms;
    }
  }
  return events[slice->end].ts_ms;
}

static uint8_t snapshot_slice_infer_driver(const SnapshotEventSlice *slice,
                                           const SnapshotEvent *events,
                                           KdiDriverId *out_driver)
{
  if (slice == NULL || events == NULL || out_driver == NULL) {
    return 0U;
  }
  if (snapshot_corr_id_to_driver(slice->corr_id, out_driver) != 0U) {
    return 1U;
  }
  for (uint8_t i = slice->begin; i <= slice->end; ++i) {
    if (snapshot_event_find_driver(events[i].msg, out_driver) != 0U) {
      return 1U;
    }
  }
  return 0U;
}

static void snapshot_collect_fault_features(const SnapshotEvent *events,
                                            uint8_t event_count,
                                            const SnapshotEventSlice *slice,
                                            SnapshotFaultFeatureVector *out)
{
  uint16_t error_hits = 0U;
  uint16_t reset_hits = 0U;
  uint8_t prev_state = 0U;
  uint8_t irq_anomaly_hint = 0U;
  KdiDriverId driver = KDI_DRIVER_KERNEL;

  if (events == NULL || slice == NULL || out == NULL) {
    return;
  }

  memset(out, 0, sizeof(*out));
  out->lookback_ms = SNAPSHOT_FEATURE_LOOKBACK_MS;
  out->fault_ts_ms = snapshot_slice_fault_ts_ms(slice, events);
  out->fault_found = (slice->fault_events != 0U) ? 1U : 0U;

  if (snapshot_slice_infer_driver(slice, events, &driver) != 0U) {
    out->driver_valid = 1U;
    out->driver_id = (uint8_t)driver;
  }

  {
    uint32_t fault_ts = out->fault_ts_ms;
    uint32_t lookback_begin = (fault_ts > out->lookback_ms) ? (fault_ts - out->lookback_ms) : 0U;
    for (uint8_t i = 0U; i < event_count; ++i) {
      const SnapshotEvent *event = &events[i];
      uint8_t state_signal = 0U;

      if (event->ts_ms > fault_ts || event->ts_ms < lookback_begin) {
        continue;
      }
      if (out->lookback_event_count < 0xFFFFU) {
        out->lookback_event_count++;
      }

      if (snapshot_event_is_irq_related(event->msg) != 0U && out->irq_event_count < 0xFFFFU) {
        out->irq_event_count++;
      }
      if (snapshot_event_is_irq_anomaly_hint(event->msg) != 0U) {
        irq_anomaly_hint = 1U;
      }
      if (snapshot_event_is_kdi_failure(event->msg) != 0U && out->kdi_fail_count < 0xFFFFU) {
        uint32_t fail_burst = 1U;
        if (snapshot_msg_parse_u32_field(event->msg, " fail=", &fail_burst) == 0U ||
            fail_burst == 0U) {
          fail_burst = 1U;
        }
        if (fail_burst > (uint32_t)(0xFFFFU - out->kdi_fail_count)) {
          out->kdi_fail_count = 0xFFFFU;
        } else {
          out->kdi_fail_count = (uint16_t)(out->kdi_fail_count + fail_burst);
        }
      }
      if (snapshot_event_has_cap_eperm(event->msg) != 0U) {
        out->cap_eperm = 1U;
      }
      if (snapshot_event_has_mpu_violation(event->msg) != 0U) {
        out->cap_mpu_violation = 1U;
      }

      if (snapshot_event_is_state_error(event->msg) != 0U) {
        state_signal = 1U;
        if (error_hits < 0xFFFFU) {
          error_hits++;
        }
      } else if (snapshot_event_is_state_reset(event->msg) != 0U) {
        state_signal = 2U;
        if (reset_hits < 0xFFFFU) {
          reset_hits++;
        }
      }

      if (state_signal != 0U) {
        if (prev_state != 0U &&
            prev_state != state_signal &&
            out->state_loop_transitions < 0xFFFFU) {
          out->state_loop_transitions++;
        }
        prev_state = state_signal;
      }
    }
  }

  {
    uint32_t rate = ((uint32_t)out->irq_event_count * 1000U) / ((out->lookback_ms != 0U) ? out->lookback_ms : 1U);
    if (rate > 0xFFFFU) {
      rate = 0xFFFFU;
    }
    out->irq_rate_per_sec = (uint16_t)rate;
  }
  out->irq_rate_abnormal =
    (out->irq_rate_per_sec >= SNAPSHOT_FEATURE_IRQ_ABN_PER_SEC || irq_anomaly_hint != 0U) ? 1U : 0U;
  out->kdi_fail_frequent =
    (out->kdi_fail_count >= SNAPSHOT_FEATURE_KDI_FAIL_FREQ_THRESHOLD) ? 1U : 0U;
  out->state_error_reset_loop =
    ((error_hits != 0U && reset_hits != 0U) && out->state_loop_transitions != 0U) ? 1U : 0U;

  {
    KdiIrqStats irq_stats = {0};
    kdi_irq_get_stats(&irq_stats);
    out->irq_deferred_pending =
      (irq_stats.deferred_pending > 0xFFFFU) ? 0xFFFFU : (uint16_t)irq_stats.deferred_pending;
  }

  if (out->driver_valid != 0U) {
    KdiDmaRingOccupancy dma = {0};
    int dma_rc = kdi_dma_get_ring_occupancy((KdiDriverId)out->driver_id, &dma);
    if (dma_rc == KDI_OK) {
      uint32_t ring_sum = (uint32_t)dma.rx_posted + (uint32_t)dma.rx_ready +
                          (uint32_t)dma.tx_pending + (uint32_t)dma.tx_done;
      if ((dma.rx_depth != 0U && dma.rx_ready >= dma.rx_depth) ||
          (dma.tx_depth != 0U && dma.tx_pending >= dma.tx_depth) ||
          (dma.total_buffers != 0U &&
           ((uint32_t)dma.rx_ready + (uint32_t)dma.tx_pending) >= (uint32_t)dma.total_buffers)) {
        out->dma_full = 1U;
      }
      if (ring_sum == 0U) {
        out->dma_idle_spin = 1U;
      }
    }
  }
}

static void snapshot_log_fault_features(uint8_t slice_id,
                                        const SnapshotEventSlice *slice,
                                        const SnapshotFaultFeatureVector *vec)
{
  char buf[160];
  const char *driver_name = "unknown";

  if (slice == NULL || vec == NULL) {
    return;
  }
  if (vec->driver_valid != 0U &&
      vec->driver_id < (uint8_t)KDI_DRIVER_COUNT) {
    driver_name = kdi_driver_name((KdiDriverId)vec->driver_id);
  }

  snprintf(buf, sizeof(buf),
           "snapshot feature slice=%u drv=%s irq_abn=%u dma_full=%u dma_idle=%u kdi_fail_freq=%u state_loop=%u eperm=%u mpu_vio=%u\r\n",
           (unsigned)slice_id,
           driver_name,
           (unsigned)vec->irq_rate_abnormal,
           (unsigned)vec->dma_full,
           (unsigned)vec->dma_idle_spin,
           (unsigned)vec->kdi_fail_frequent,
           (unsigned)vec->state_error_reset_loop,
           (unsigned)vec->cap_eperm,
           (unsigned)vec->cap_mpu_violation);
  log_send_blocking(buf);

  snprintf(buf, sizeof(buf),
           "snapshot feature detail slice=%u irq_evt=%u irq_rate=%u kdi_fail=%u lookback_ms=%lu corr=0x%08lX\r\n",
           (unsigned)slice_id,
           (unsigned)vec->irq_event_count,
           (unsigned)vec->irq_rate_per_sec,
           (unsigned)vec->kdi_fail_count,
           (unsigned long)vec->lookback_ms,
           (unsigned long)slice->corr_id);
  log_send_blocking(buf);
}

static uint8_t snapshot_event_is_dma_related(const char *msg)
{
  if (msg == NULL) {
    return 0U;
  }
  if (strstr(msg, "dma") != NULL || strstr(msg, "ring") != NULL) {
    return 1U;
  }
  if (strstr(msg, "rx_") != NULL || strstr(msg, "tx_") != NULL) {
    return 1U;
  }
  return 0U;
}

static uint8_t snapshot_event_is_limit_msg(const char *msg)
{
  if (msg == NULL) {
    return 0U;
  }
  if (strstr(msg, "rc=limit") != NULL || strstr(msg, ": limit") != NULL) {
    return 1U;
  }
  if (strstr(msg, "overflow") != NULL || strstr(msg, "exhaust") != NULL) {
    return 1U;
  }
  return 0U;
}

static uint8_t snapshot_event_is_dma_pressure_hint(const char *msg)
{
  if (msg == NULL) {
    return 0U;
  }
  if (snapshot_event_is_dma_related(msg) == 0U) {
    return 0U;
  }
  if (snapshot_event_is_limit_msg(msg) != 0U ||
      strstr(msg, "backpressure") != NULL ||
      strstr(msg, "ring_overflow") != NULL) {
    return 1U;
  }
  return 0U;
}

static uint8_t snapshot_event_is_timeout_msg(const char *msg)
{
  if (msg == NULL) {
    return 0U;
  }
  if (strstr(msg, "timeout") != NULL ||
      strstr(msg, "timed out") != NULL ||
      strstr(msg, "deadline") != NULL ||
      strstr(msg, "stuck") != NULL) {
    return 1U;
  }
  return 0U;
}

static uint8_t snapshot_event_is_resource_fault_hint(const char *msg)
{
  if (msg == NULL) {
    return 0U;
  }
  if (snapshot_event_is_limit_msg(msg) != 0U ||
      snapshot_event_is_dma_pressure_hint(msg) != 0U ||
      snapshot_event_is_irq_anomaly_hint(msg) != 0U) {
    return 1U;
  }
  if (strstr(msg, "overflow") != NULL ||
      strstr(msg, "exhaust") != NULL ||
      strstr(msg, "resource fault") != NULL) {
    return 1U;
  }
  return 0U;
}

static uint8_t snapshot_event_is_failure_msg(const char *msg)
{
  if (msg == NULL) {
    return 0U;
  }
  if (snapshot_event_is_fault_msg(msg) != 0U ||
      snapshot_event_is_kdi_failure(msg) != 0U ||
      snapshot_event_has_cap_eperm(msg) != 0U ||
      snapshot_event_has_mpu_violation(msg) != 0U ||
      snapshot_event_is_timeout_msg(msg) != 0U ||
      snapshot_event_is_resource_fault_hint(msg) != 0U ||
      snapshot_event_is_state_error(msg) != 0U ||
      snapshot_event_is_state_reset(msg) != 0U) {
    return 1U;
  }
  if (strstr(msg, " run fail ") != NULL ||
      strstr(msg, ": fail") != NULL ||
      strstr(msg, " failed") != NULL ||
      strstr(msg, "rollback applied") != NULL ||
      strstr(msg, "rolled_back") != NULL) {
    return 1U;
  }
  return 0U;
}

static uint8_t snapshot_slice_has_failure_event(const SnapshotEventSlice *slice,
                                                const SnapshotEvent *events,
                                                uint8_t event_count)
{
  if (slice == NULL || events == NULL || event_count == 0U) {
    return 0U;
  }
  for (uint8_t i = slice->begin; i <= slice->end && i < event_count; ++i) {
    if (snapshot_event_is_failure_msg(events[i].msg) != 0U) {
      return 1U;
    }
  }
  return 0U;
}

static const char *snapshot_failure_category_name(AnalysisFailureCategory category)
{
  return analysis_failure_category_name(category);
}

static void snapshot_log_ai_explanation(uint8_t slice_id,
                                        AnalysisFailureCategory category,
                                        const SnapshotEventSlice *slice,
                                        const SnapshotEvent *events,
                                        uint8_t event_count,
                                        const SnapshotFaultFeatureVector *vec,
                                        const uint8_t *evidence_ids,
                                        uint8_t evidence_count,
                                        const char *stage_name)
{
  char buf[176];
  char evidence_buf[32];
  const char *driver_name = "unknown";
  uint8_t support_emitted = 0U;

  if (slice == NULL || events == NULL || vec == NULL || evidence_ids == NULL || stage_name == NULL) {
    return;
  }

  if (vec->driver_valid != 0U && vec->driver_id < (uint8_t)KDI_DRIVER_COUNT) {
    driver_name = kdi_driver_name((KdiDriverId)vec->driver_id);
  }

  snapshot_hyp_evidence_text(evidence_ids, evidence_count, evidence_buf, sizeof(evidence_buf));
  snprintf(buf, sizeof(buf),
           "snapshot ai failure slice=%u class=%s stage=%s driver=%s evidence=%s\r\n",
           (unsigned)slice_id,
           snapshot_failure_category_name(category),
           stage_name,
           driver_name,
           evidence_buf);
  log_send_blocking(buf);

  for (uint8_t i = 0U; i < evidence_count && i < SNAPSHOT_HYP_EVIDENCE_CAP; ++i) {
    uint8_t event_id = evidence_ids[i];
    const SnapshotEvent *event;
    const char *event_stage = "none";

    if (event_id >= event_count) {
      continue;
    }
    event = &events[event_id];
    if (event->stage_valid != 0U && event->stage_id < (uint8_t)BRINGUP_PHASE_COUNT) {
      event_stage = bringup_phase_name((BringupPhaseId)event->stage_id);
    }
    snprintf(buf, sizeof(buf),
             "snapshot ai support slice=%u event=%u stage=%s msg=%s\r\n",
             (unsigned)slice_id,
             (unsigned)event_id,
             event_stage,
             event->msg);
    log_send_blocking(buf);
    support_emitted++;
    if (support_emitted >= 3U) {
      break;
    }
  }

  switch (category) {
    case ANALYSIS_FAILURE_ORDERING_VIOLATION:
      snprintf(buf, sizeof(buf),
               "snapshot ai checkpoint slice=%u step=1 cmd=bringup phase show focus=error/reset/rolled_back sequence\r\n",
               (unsigned)slice_id);
      log_send_blocking(buf);
      snprintf(buf, sizeof(buf),
               "snapshot ai checkpoint slice=%u step=2 cmd=bringup stage show focus=current/blocked stage transition\r\n",
               (unsigned)slice_id);
      log_send_blocking(buf);
      snprintf(buf, sizeof(buf),
               "snapshot ai checkpoint slice=%u step=3 cmd=kdi driver show focus=state transitions for %s\r\n",
               (unsigned)slice_id,
               driver_name);
      log_send_blocking(buf);
      break;
    case ANALYSIS_FAILURE_MISSING_DEPENDENCY:
      snprintf(buf, sizeof(buf),
               "snapshot ai checkpoint slice=%u step=1 cmd=bringup stage wait focus=which driver/resource is waiting\r\n",
               (unsigned)slice_id);
      log_send_blocking(buf);
      snprintf(buf, sizeof(buf),
               "snapshot ai checkpoint slice=%u step=2 cmd=dep show focus=stage->driver and driver->resource edges\r\n",
               (unsigned)slice_id);
      log_send_blocking(buf);
      snprintf(buf, sizeof(buf),
               "snapshot ai checkpoint slice=%u step=3 cmd=dep impact %s focus=blast radius before reset\r\n",
               (unsigned)slice_id,
               driver_name);
      log_send_blocking(buf);
      break;
    case ANALYSIS_FAILURE_TIMEOUT:
      snprintf(buf, sizeof(buf),
               "snapshot ai checkpoint slice=%u step=1 cmd=bringup stage wait focus=stuck stage and pending waiters\r\n",
               (unsigned)slice_id);
      log_send_blocking(buf);
      snprintf(buf, sizeof(buf),
               "snapshot ai checkpoint slice=%u step=2 cmd=kdi irq show focus=pending/throttle/starve counters\r\n",
               (unsigned)slice_id);
      log_send_blocking(buf);
      snprintf(buf, sizeof(buf),
               "snapshot ai checkpoint slice=%u step=3 cmd=snapshot focus=confirm timeout events repeat\r\n",
               (unsigned)slice_id);
      log_send_blocking(buf);
      break;
    case ANALYSIS_FAILURE_PERMISSION_VIOLATION:
      snprintf(buf, sizeof(buf),
               "snapshot ai checkpoint slice=%u step=1 cmd=kdi fault domain %s focus=fault containment and last code\r\n",
               (unsigned)slice_id,
               driver_name);
      log_send_blocking(buf);
      snprintf(buf, sizeof(buf),
               "snapshot ai checkpoint slice=%u step=2 cmd=kdi show focus=auth/denied counters and rc fields\r\n",
               (unsigned)slice_id);
      log_send_blocking(buf);
      snprintf(buf, sizeof(buf),
               "snapshot ai checkpoint slice=%u step=3 cmd=snapshot focus=EPERM or MPU evidence events\r\n",
               (unsigned)slice_id);
      log_send_blocking(buf);
      break;
    case ANALYSIS_FAILURE_EARLY_RESOURCE_FAULT:
      snprintf(buf, sizeof(buf),
               "snapshot ai checkpoint slice=%u step=1 cmd=snapshot focus=dma_full/irq_abn/kdi_fail features\r\n",
               (unsigned)slice_id);
      log_send_blocking(buf);
      snprintf(buf, sizeof(buf),
               "snapshot ai checkpoint slice=%u step=2 cmd=kdi irq show focus=pending/defer/drop/worker errors\r\n",
               (unsigned)slice_id);
      log_send_blocking(buf);
      snprintf(buf, sizeof(buf),
               "snapshot ai checkpoint slice=%u step=3 cmd=bringup stage wait focus=resource wait chain\r\n",
               (unsigned)slice_id);
      log_send_blocking(buf);
      break;
    default:
      break;
  }
}

static int snapshot_collect_failure_result(const SnapshotEvent *events,
                                           uint8_t event_count,
                                           const SnapshotEventSlice *slice,
                                           const SnapshotFaultFeatureVector *vec,
                                           AnalysisFailureResult *out)
{
  AnalysisEvent analysis_events[SNAPSHOT_EVENT_CAP];
  AnalysisEventSlice analysis_slice = {0};
  AnalysisInput input = {0};
  AnalysisResult result = {0};

  if (events == NULL || slice == NULL || vec == NULL || out == NULL) {
    return -1;
  }
  if (event_count > SNAPSHOT_EVENT_CAP) {
    return -1;
  }

  for (uint8_t i = 0U; i < event_count; ++i) {
    analysis_events[i].ts_ms = events[i].ts_ms;
    analysis_events[i].boundary_flags = events[i].boundary_flags;
    analysis_events[i].stage_valid = events[i].stage_valid;
    analysis_events[i].stage_id = events[i].stage_id;
    analysis_events[i].msg = events[i].msg;
  }

  analysis_slice.begin = slice->begin;
  analysis_slice.end = slice->end;
  analysis_slice.stage_valid = slice->stage_valid;
  analysis_slice.stage_id = slice->stage_id;
  analysis_slice.fault_events = slice->fault_events;
  analysis_slice.reset_events = slice->reset_events;
  analysis_slice.start_reason = slice->start_reason;
  analysis_slice.corr_id = slice->corr_id;

  input.events = analysis_events;
  input.event_count = event_count;
  input.slice = &analysis_slice;
  input.features = vec;
  input.boot_complete = bringup_phase_model.boot_complete;

  if (analysis_engine_analyze_fault_slice(&snapshot_analysis_engine, &input, &result) != 0) {
    return -1;
  }

  *out = result.failure;
  return 0;
}

static void snapshot_log_failure_classification(uint8_t slice_id,
                                                const SnapshotEvent *events,
                                                uint8_t event_count,
                                                const SnapshotEventSlice *slice,
                                                const SnapshotFaultFeatureVector *vec)
{
  AnalysisFailureResult failure = {0};
  char evidence_buf[32];
  char buf[176];
  const char *stage_name = "none";

  if (events == NULL || slice == NULL || vec == NULL) {
    return;
  }
  if (event_count > SNAPSHOT_EVENT_CAP) {
    return;
  }
  if (slice->stage_valid != 0U && slice->stage_id < (uint8_t)BRINGUP_PHASE_COUNT) {
    BringupStageId stage = bringup_stage_from_phase((BringupPhaseId)slice->stage_id);
    stage_name = bringup_stage_name(stage);
  }
  if (snapshot_collect_failure_result(events, event_count, slice, vec, &failure) != 0) {
    return;
  }

  snapshot_hyp_evidence_text(failure.evidence_ids,
                             failure.evidence_count,
                             evidence_buf,
                             sizeof(evidence_buf));
  snprintf(buf, sizeof(buf),
           "snapshot failure slice=%u category=%s conf=%u evidence=%s stage=%s\r\n",
           (unsigned)slice_id,
           snapshot_failure_category_name(failure.category),
           (unsigned)failure.confidence,
           evidence_buf,
           stage_name);
  log_send_blocking(buf);

  if (failure.explain_p1[0] != '\0') {
    snprintf(buf, sizeof(buf),
             "snapshot failure explain slice=%u category=%s p1=%s\r\n",
             (unsigned)slice_id,
             snapshot_failure_category_name(failure.category),
             failure.explain_p1);
    log_send_blocking(buf);
  }
  if (failure.explain_p2[0] != '\0') {
    snprintf(buf, sizeof(buf),
             "snapshot failure explain slice=%u category=%s p2=%s\r\n",
             (unsigned)slice_id,
             snapshot_failure_category_name(failure.category),
             failure.explain_p2);
    log_send_blocking(buf);
  }

  snapshot_log_ai_explanation(slice_id,
                              failure.category,
                              slice,
                              events,
                              event_count,
                              vec,
                              failure.evidence_ids,
                              failure.evidence_count,
                              stage_name);
}

static void snapshot_hyp_evidence_text(const uint8_t *ids,
                                       uint8_t count,
                                       char *buf,
                                       size_t buf_size)
{
  size_t used = 0U;

  if (buf == NULL || buf_size == 0U) {
    return;
  }
  if (ids == NULL || count == 0U) {
    (void)snprintf(buf, buf_size, "none");
    return;
  }

  buf[0] = '\0';
  for (uint8_t i = 0U; i < count && used < buf_size; ++i) {
    used += (size_t)snprintf(buf + used, buf_size - used,
                             "%s%u", (i == 0U) ? "" : ",", (unsigned)ids[i]);
  }
}

static void snapshot_log_hypothesis(uint8_t slice_id,
                                    const char *name,
                                    uint8_t confidence,
                                    const uint8_t *evidence_ids,
                                    uint8_t evidence_count,
                                    const char *explain_p1,
                                    const char *explain_p2)
{
  char buf[176];
  char evidence_buf[32];

  if (name == NULL) {
    return;
  }

  snapshot_hyp_evidence_text(evidence_ids, evidence_count, evidence_buf, sizeof(evidence_buf));
  snprintf(buf, sizeof(buf),
           "snapshot hypothesis slice=%u name=%s conf=%u evidence=%s\r\n",
           (unsigned)slice_id,
           name,
           (unsigned)confidence,
           evidence_buf);
  log_send_blocking(buf);

  if (explain_p1 != NULL && explain_p1[0] != '\0') {
    snprintf(buf, sizeof(buf),
             "snapshot hypothesis explain slice=%u name=%s p1=%s\r\n",
             (unsigned)slice_id,
             name,
             explain_p1);
    log_send_blocking(buf);
  }
  if (explain_p2 != NULL && explain_p2[0] != '\0') {
    snprintf(buf, sizeof(buf),
             "snapshot hypothesis explain slice=%u name=%s p2=%s\r\n",
             (unsigned)slice_id,
             name,
             explain_p2);
    log_send_blocking(buf);
  }
}

static void snapshot_log_fault_hypotheses(uint8_t slice_id,
                                          const SnapshotEvent *events,
                                          uint8_t event_count,
                                          const SnapshotEventSlice *slice,
                                          const SnapshotFaultFeatureVector *vec)
{
  AnalysisEvent analysis_events[SNAPSHOT_EVENT_CAP];
  AnalysisEventSlice analysis_slice = {0};
  AnalysisInput input = {0};
  AnalysisResult result = {0};

  if (events == NULL || slice == NULL || vec == NULL) {
    return;
  }
  if (event_count > SNAPSHOT_EVENT_CAP) {
    return;
  }

  for (uint8_t i = 0U; i < event_count; ++i) {
    analysis_events[i].ts_ms = events[i].ts_ms;
    analysis_events[i].boundary_flags = events[i].boundary_flags;
    analysis_events[i].stage_valid = events[i].stage_valid;
    analysis_events[i].stage_id = events[i].stage_id;
    analysis_events[i].msg = events[i].msg;
  }

  analysis_slice.begin = slice->begin;
  analysis_slice.end = slice->end;
  analysis_slice.stage_valid = slice->stage_valid;
  analysis_slice.stage_id = slice->stage_id;
  analysis_slice.fault_events = slice->fault_events;
  analysis_slice.reset_events = slice->reset_events;
  analysis_slice.start_reason = slice->start_reason;
  analysis_slice.corr_id = slice->corr_id;

  input.events = analysis_events;
  input.event_count = event_count;
  input.slice = &analysis_slice;
  input.features = vec;
  input.boot_complete = bringup_phase_model.boot_complete;

  if (analysis_engine_analyze_fault_slice(&snapshot_analysis_engine, &input, &result) != 0) {
    return;
  }

  for (uint8_t i = 0U; i < result.hypothesis_count; ++i) {
    const AnalysisHypothesisResult *hyp = &result.hypotheses[i];
    snapshot_log_hypothesis(slice_id,
                            hyp->name,
                            hyp->confidence,
                            hyp->evidence_ids,
                            hyp->evidence_count,
                            hyp->explain_p1,
                            hyp->explain_p2);
  }
}

static void log_send_blocking(const char *msg)
{
  LogMsg out = {0};
  size_t i = 0;

  /* Task-level logs should go through the log queue to serialize UART access. */
  if (log_queue == NULL || msg == NULL) {
    return;
  }

  do {
    memset(&out, 0, sizeof(out));
    i = 0U;
    for (; i < (LOG_MSG_LEN - 1U) && msg[i] != '\0'; ++i) {
      out.text[i] = msg[i];
    }
    out.text[i] = '\0';
    (void)xQueueSend(log_queue, &out, portMAX_DELAY);
    msg += i;
  } while (i == (LOG_MSG_LEN - 1U) && msg[0] != '\0');
}

static void log_task(void *arg)
{
  (void)arg;
  LogMsg msg;

  for (;;) {
    if (xQueueReceive(log_queue, &msg, portMAX_DELAY) == pdPASS) {
      size_t len = 0U;
      SnapshotEvent event = {0};

      if (strncmp(msg.text, "snapshot ", 9) != 0 &&
          strcmp(msg.text, "HB\r\n") != 0 &&
          strncmp(msg.text, "ADC ", 4) != 0) {
        event.ts_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        while (len + 1U < sizeof(event.msg) &&
               msg.text[len] != '\0' &&
               msg.text[len] != '\r' &&
               msg.text[len] != '\n') {
          event.msg[len] = msg.text[len];
          len++;
        }
        event.msg[len] = '\0';
        if (len != 0U) {
          event.corr_id = snapshot_event_corr_id(event.msg);
          event.boundary_flags = snapshot_event_boundary_flags(event.msg,
                                                               &event.stage_valid,
                                                               &event.stage_id);
          taskENTER_CRITICAL();
          snapshot_events[snapshot_event_head] = event;
          snapshot_event_head = (uint8_t)((snapshot_event_head + 1U) % SNAPSHOT_EVENT_CAP);
          if (snapshot_event_count < SNAPSHOT_EVENT_CAP) {
            snapshot_event_count++;
          }
          taskEXIT_CRITICAL();
        }
      }
      board_uart_write(msg.text);
    }
  }
}

static char *next_token(char **input)
{
  char *s = *input;
  while (*s == ' ' || *s == '\t') {
    s++;
  }
  if (*s == '\0') {
    *input = s;
    return NULL;
  }
  char *start = s;
  while (*s && *s != ' ' && *s != '\t') {
    s++;
  }
  if (*s) {
    *s = '\0';
    s++;
  }
  *input = s;
  return start;
}

static int parse_u32(const char *s, uint32_t *out)
{
  if (s == NULL || out == NULL) {
    return 0;
  }
  char *end = NULL;
  unsigned long v = strtoul(s, &end, 0);
  if (end == s || *end != '\0') {
    return 0;
  }
  *out = (uint32_t)v;
  return 1;
}

static void vm_log_result(const char *label, Vm32Result res)
{
  char buf[64];
  snprintf(buf, sizeof(buf), "%s=%d\r\n", label, (int)res);
  log_send_blocking(buf);
}

static void vm_log_cfg_report(const char *label, Vm32Result res, const Vm32CfgReport *cfg)
{
  char buf[192];

  if (cfg == NULL) {
    return;
  }

  if (res == VM32_OK) {
    snprintf(buf, sizeof(buf),
             "%s: ok %s max=%lu nodes=%lu\r\n",
             label,
             vm32_cfg_reason_name(cfg->reason),
             (unsigned long)cfg->max_steps,
             (unsigned long)cfg->reachable);
  } else {
    snprintf(buf, sizeof(buf),
             "%s: reject reason=%s pc=0x%04lX op=0x%02X\r\n",
             label,
             vm32_cfg_reason_name(cfg->reason),
             (unsigned long)cfg->reject_pc,
             (unsigned)cfg->reject_op);
  }
  log_send_blocking(buf);
}

static void scenario_log_result(const ScenarioResult *result)
{
  char buf[160];

  if (result == NULL) {
    return;
  }

  snprintf(buf, sizeof(buf),
           "scenario result: name=%s status=%s vm=%d steps=%lu fault=%u\r\n",
           (result->name != NULL) ? result->name : "-",
           scenario_status_name(result->status),
           (int)result->vm_result,
           (unsigned long)result->steps,
           (unsigned)result->vm_fault);
  log_send_blocking(buf);
}

static Vm32Result vm_run_loop(uint32_t max, uint8_t allow_yield, uint8_t allow_break, uint8_t *bp_hit)
{
  Vm32Result res = VM32_OK;
  if (bp_hit != NULL) {
    *bp_hit = 0U;
  }
  for (uint32_t i = 0; i < max; ++i) {
    if (allow_break && vm32.bp_valid && vm32.pc == vm32.bp_addr) {
      if (bp_hit != NULL) {
        *bp_hit = 1U;
      }
      res = VM32_OK;
      break;
    }
    res = vm32_step(&vm32);
    if (res != VM32_OK) {
      break;
    }
    if (allow_yield && (i & 0x3FFU) == 0x3FFU) {
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }
  return res;
}

static void vm_report_fault_if_needed(Vm32Result res)
{
  if (res == VM32_ERR_STACK || res == VM32_ERR_MEM || res == VM32_ERR_POLICY) {
    char buf[112];
    KdiFaultReport report = {
      .code = (uint16_t)res,
      .detail = vm32.pc,
    };
    (void)kdi_report_fault(KDI_DRIVER_VM_RUNTIME, kdi_caps.vm_runtime, &report);
    fault_report_vm((int)res, vm32.pc, vm32.ic, vm32.last_op,
                    vm32.dtop, vm32.rtop, vm32.last_out);
    snprintf(buf, sizeof(buf),
             "fault event src=vm err=%d pc=0x%04lX ic=%lu op=0x%02X\r\n",
             (int)res,
             (unsigned long)(vm32.pc & 0xFFFFU),
             (unsigned long)vm32.ic,
             (unsigned)vm32.last_op);
    log_send_blocking(buf);
  }
}

static int vm_mig_parse_mask(const char *name, uint32_t *out_mask)
{
  if (name == NULL || out_mask == NULL) {
    return 0;
  }
  if (strcmp(name, "all") == 0) {
    *out_mask = VM32_MIG_RES_ALL;
    return 1;
  }
  if (strcmp(name, "uart_tx") == 0) {
    *out_mask = VM32_MIG_RES_UART_TX;
    return 1;
  }
  if (strcmp(name, "uart_rx") == 0) {
    *out_mask = VM32_MIG_RES_UART_RX;
    return 1;
  }
  if (strcmp(name, "led") == 0) {
    *out_mask = VM32_MIG_RES_LED;
    return 1;
  }
  if (strcmp(name, "ic") == 0) {
    *out_mask = VM32_MIG_RES_IC;
    return 1;
  }
  return 0;
}

static int vm_mig_parse_mask_list(const char *csv, uint32_t *out_mask)
{
  char tmp[80];
  char *tok = NULL;
  char *save = NULL;
  uint32_t mask = 0U;

  if (csv == NULL || out_mask == NULL) {
    return 0;
  }
  if (strcmp(csv, "all") == 0) {
    *out_mask = VM32_MIG_RES_ALL;
    return 1;
  }
  if (strcmp(csv, "none") == 0 || csv[0] == '\0') {
    *out_mask = 0U;
    return 1;
  }
  if (strlen(csv) >= sizeof(tmp)) {
    return 0;
  }

  strcpy(tmp, csv);
  tok = strtok_r(tmp, ",", &save);
  while (tok != NULL) {
    uint32_t one = 0U;
    if (!vm_mig_parse_mask(tok, &one) || one == VM32_MIG_RES_ALL) {
      return 0;
    }
    mask |= one;
    tok = strtok_r(NULL, ",", &save);
  }
  *out_mask = mask;
  return 1;
}

static int vm_mig_json_get_string(const char *json, const char *key, char *out, size_t out_size)
{
  char pat[24];
  const char *p;
  size_t i = 0U;

  if (json == NULL || key == NULL || out == NULL || out_size == 0U) {
    return -1;
  }

  snprintf(pat, sizeof(pat), "\"%s\":", key);
  p = strstr(json, pat);
  if (p == NULL) {
    return 0;
  }
  p += strlen(pat);
  while (*p == ' ' || *p == '\t') {
    p++;
  }
  if (*p != '"') {
    return -1;
  }
  p++;
  while (*p != '\0' && *p != '"') {
    if ((i + 1U) >= out_size) {
      return -1;
    }
    out[i++] = *p++;
  }
  if (*p != '"') {
    return -1;
  }
  out[i] = '\0';
  return 1;
}

static int vm_mig_json_get_bool(const char *json, const char *key, uint8_t *out)
{
  char pat[24];
  const char *p;

  if (json == NULL || key == NULL || out == NULL) {
    return -1;
  }

  snprintf(pat, sizeof(pat), "\"%s\":", key);
  p = strstr(json, pat);
  if (p == NULL) {
    return 0;
  }
  p += strlen(pat);
  while (*p == ' ' || *p == '\t') {
    p++;
  }
  if (strncmp(p, "true", 4) == 0) {
    *out = 1U;
    return 1;
  }
  if (strncmp(p, "false", 5) == 0) {
    *out = 0U;
    return 1;
  }
  return -1;
}

static int vm_mig_apply_json(Vm32 *vm, const char *json, char *err, size_t err_size)
{
  char mode[16];
  char allow[80];
  char deny[80];
  uint8_t reset = 0U;
  int mode_rc;
  int allow_rc;
  int deny_rc;
  int reset_rc;
  uint8_t changed = 0U;

  if (vm == NULL || json == NULL || err == NULL || err_size == 0U) {
    return 0;
  }

  if (json[0] != '{') {
    snprintf(err, err_size, "json must start with '{'");
    return 0;
  }

  mode_rc = vm_mig_json_get_string(json, "mode", mode, sizeof(mode));
  allow_rc = vm_mig_json_get_string(json, "allow", allow, sizeof(allow));
  deny_rc = vm_mig_json_get_string(json, "deny", deny, sizeof(deny));
  reset_rc = vm_mig_json_get_bool(json, "reset", &reset);

  if (mode_rc < 0 || allow_rc < 0 || deny_rc < 0 || reset_rc < 0) {
    snprintf(err, err_size, "bad json field format");
    return 0;
  }
  if (mode_rc == 0 && allow_rc == 0 && deny_rc == 0 && reset_rc == 0) {
    snprintf(err, err_size, "json has no supported keys");
    return 0;
  }

  if (reset_rc == 1 && reset != 0U) {
    vm32_mig_reset(vm);
    changed = 1U;
  }

  if (allow_rc == 1) {
    uint32_t allow_mask = 0U;
    if (!vm_mig_parse_mask_list(allow, &allow_mask)) {
      snprintf(err, err_size, "bad allow list");
      return 0;
    }
    vm32_mig_deny(vm, VM32_MIG_RES_ALL);
    vm32_mig_allow(vm, allow_mask);
    changed = 1U;
  }

  if (deny_rc == 1) {
    uint32_t deny_mask = 0U;
    if (!vm_mig_parse_mask_list(deny, &deny_mask)) {
      snprintf(err, err_size, "bad deny list");
      return 0;
    }
    vm32_mig_deny(vm, deny_mask);
    changed = 1U;
  }

  if (mode_rc == 1) {
    if (strcmp(mode, "off") == 0) {
      vm32_mig_set_mode(vm, VM32_MIG_MODE_OFF);
    } else if (strcmp(mode, "monitor") == 0) {
      vm32_mig_set_mode(vm, VM32_MIG_MODE_MONITOR);
    } else if (strcmp(mode, "enforce") == 0) {
      vm32_mig_set_mode(vm, VM32_MIG_MODE_ENFORCE);
    } else {
      snprintf(err, err_size, "bad mode");
      return 0;
    }
    changed = 1U;
  }

  if (!changed) {
    snprintf(err, err_size, "no effective changes");
    return 0;
  }
  err[0] = '\0';
  return 1;
}

static void vm_mig_mask_to_text(uint32_t mask, char *buf, size_t buf_size)
{
  size_t idx = 0U;
  uint8_t first = 1U;

  if (buf == NULL || buf_size == 0U) {
    return;
  }
  buf[0] = '\0';

  if ((mask & VM32_MIG_RES_UART_TX) != 0U) {
    idx += (size_t)snprintf(buf + idx, buf_size - idx, "%suart_tx", first ? "" : ",");
    first = 0U;
  }
  if ((mask & VM32_MIG_RES_UART_RX) != 0U && idx < buf_size) {
    idx += (size_t)snprintf(buf + idx, buf_size - idx, "%suart_rx", first ? "" : ",");
    first = 0U;
  }
  if ((mask & VM32_MIG_RES_LED) != 0U && idx < buf_size) {
    idx += (size_t)snprintf(buf + idx, buf_size - idx, "%sled", first ? "" : ",");
    first = 0U;
  }
  if ((mask & VM32_MIG_RES_IC) != 0U && idx < buf_size) {
    idx += (size_t)snprintf(buf + idx, buf_size - idx, "%sic", first ? "" : ",");
    first = 0U;
  }
  if (first && idx < buf_size) {
    (void)snprintf(buf, buf_size, "none");
  }
}

static void vm_mig_log_status(void)
{
  Vm32MigStatus st;
  char allow_buf[64];
  char buf[224];

  vm32_mig_status(&vm32, &st);
  vm_mig_mask_to_text(st.allow_mask, allow_buf, sizeof(allow_buf));
  snprintf(buf, sizeof(buf),
           "vm mig: mode=%s allow=%s violations=%lu blocked=%u last=%s@0x%04lX %s\r\n",
           vm32_mig_mode_name(st.mode),
           allow_buf,
           (unsigned long)st.violations,
           (unsigned)st.enforce_blocked,
           vm32_mig_resource_name(st.last_resource),
           (unsigned long)(st.last_addr & 0xFFFFU),
           st.last_write ? "write" : "read");
  log_send_blocking(buf);
}

static void bringup_log_check(void)
{
  char buf[64];
  uint32_t shcsr_need = (SCB_SHCSR_USGFAULTENA_Msk |
                         SCB_SHCSR_BUSFAULTENA_Msk |
                         SCB_SHCSR_MEMFAULTENA_Msk);
  uint32_t clk = SystemCoreClock;
  uint32_t heap = xPortGetFreeHeapSize();
  uint32_t vtor = SCB->VTOR;
  uint32_t shcsr = SCB->SHCSR;
  uint8_t ok_clk = (clk >= BRINGUP_CLK_MIN_HZ && clk <= BRINGUP_CLK_MAX_HZ) ? 1U : 0U;
  uint8_t ok_vtor = (vtor == FLASH_BASE) ? 1U : 0U;
  uint8_t ok_fault = ((shcsr & shcsr_need) == shcsr_need) ? 1U : 0U;
  uint8_t ok_heap = (heap >= 4096U) ? 1U : 0U;
  uint8_t ok_ipc = (g_shared_ctrl.ipc_cmd_q != 0U && g_shared_ctrl.ipc_resp_q != 0U) ? 1U : 0U;
  uint8_t ok_mpu = 0U;
  uint8_t ok_all;
  Vm32MigStatus mig;

#if APP_DISABLE_MPU
  ok_mpu = ((MPU->CTRL & MPU_CTRL_ENABLE_Msk) == 0U) ? 1U : 0U;
#else
  ok_mpu = ((MPU->CTRL & MPU_CTRL_ENABLE_Msk) != 0U) ? 1U : 0U;
#endif
  vm32_mig_status(&vm32, &mig);
  ok_all = (uint8_t)(ok_clk && ok_vtor && ok_fault && ok_heap && ok_ipc && ok_mpu);

  snprintf(buf, sizeof(buf), "bringup clk=%lu %s\r\n",
           (unsigned long)clk, ok_clk ? "ok" : "bad");
  log_send_blocking(buf);
  snprintf(buf, sizeof(buf), "bringup vtor=0x%08lX %s\r\n",
           (unsigned long)vtor, ok_vtor ? "ok" : "bad");
  log_send_blocking(buf);
  snprintf(buf, sizeof(buf), "bringup shcsr=0x%08lX %s\r\n",
           (unsigned long)shcsr, ok_fault ? "ok" : "bad");
  log_send_blocking(buf);
  snprintf(buf, sizeof(buf), "bringup heap=%lu %s\r\n",
           (unsigned long)heap, ok_heap ? "ok" : "bad");
  log_send_blocking(buf);
  snprintf(buf, sizeof(buf), "bringup ipc=%s\r\n", ok_ipc ? "ok" : "bad");
  log_send_blocking(buf);
  snprintf(buf, sizeof(buf), "bringup mpu=%s\r\n", ok_mpu ? "ok" : "bad");
  log_send_blocking(buf);
  snprintf(buf, sizeof(buf), "bringup mig=%s vio=%lu\r\n",
           vm32_mig_mode_name(mig.mode), (unsigned long)mig.violations);
  log_send_blocking(buf);
  snprintf(buf, sizeof(buf), "bringup result=%s\r\n", ok_all ? "pass" : "fail");
  log_send_blocking(buf);
}

static void bringup_log_json(void)
{
  char buf[64];
  uint32_t shcsr_need = (SCB_SHCSR_USGFAULTENA_Msk |
                         SCB_SHCSR_BUSFAULTENA_Msk |
                         SCB_SHCSR_MEMFAULTENA_Msk);
  uint32_t clk = SystemCoreClock;
  uint32_t heap = xPortGetFreeHeapSize();
  uint8_t ok_clk = (clk >= BRINGUP_CLK_MIN_HZ && clk <= BRINGUP_CLK_MAX_HZ) ? 1U : 0U;
  uint8_t ok_fault = ((SCB->SHCSR & shcsr_need) == shcsr_need) ? 1U : 0U;
  uint8_t ok_mpu = 0U;
  uint8_t ok_all;

#if APP_DISABLE_MPU
  ok_mpu = ((MPU->CTRL & MPU_CTRL_ENABLE_Msk) == 0U) ? 1U : 0U;
#else
  ok_mpu = ((MPU->CTRL & MPU_CTRL_ENABLE_Msk) != 0U) ? 1U : 0U;
#endif
  ok_all = (uint8_t)(ok_clk && ok_fault && ok_mpu);
  snprintf(buf, sizeof(buf),
           "{\"type\":\"bringup\",\"ok\":%u,\"clk\":%lu,\"heap\":%lu}\r\n",
           (unsigned)ok_all,
           (unsigned long)clk,
           (unsigned long)heap);
  log_send_blocking(buf);
}

static void bringup_log_mpu(void)
{
  char buf[64];
  uint32_t saved_rnr = MPU->RNR;

  snprintf(buf, sizeof(buf), "mpu ctrl=0x%08lX\r\n", (unsigned long)MPU->CTRL);
  log_send_blocking(buf);
  for (uint32_t i = 0; i < configTOTAL_MPU_REGIONS; ++i) {
    uint32_t rbar;
    uint32_t rasr;
    MPU->RNR = i;
    rbar = MPU->RBAR;
    rasr = MPU->RASR;
    snprintf(buf, sizeof(buf), "mpu r%lu en=%u\r\n",
             (unsigned long)i,
             (unsigned)((rasr & MPU_RASR_ENABLE_Msk) ? 1U : 0U));
    log_send_blocking(buf);
    snprintf(buf, sizeof(buf), "  rbar=0x%08lX\r\n", (unsigned long)rbar);
    log_send_blocking(buf);
    snprintf(buf, sizeof(buf), "  rasr=0x%08lX\r\n", (unsigned long)rasr);
    log_send_blocking(buf);
  }
  MPU->RNR = saved_rnr;
}

static void bringup_phase_log_usage(void)
{
  log_send_blocking("bringup [check|json|mpu]\r\n");
  log_send_blocking("bringup phase [show|json|run|reset|rerun <phase>|rollback <phase>]\r\n");
  log_send_blocking("bringup phase [inject <phase> [code]|clearfail <phase|all>]\r\n");
  log_send_blocking("bringup stage [show|json|wait|wait-json]\r\n");
}

static void bringup_phase_log_status_dump(uint8_t json_mode)
{
  char buf[256];
  BringupPhaseId phase;

  if (json_mode == 0U) {
    if (bringup_phase_model.active_valid != 0U) {
      snprintf(buf, sizeof(buf),
               "bringup phase-summary boot_complete=%u active=%s last_error=%ld inject_mask=0x%08lX\r\n",
               (unsigned)bringup_phase_model.boot_complete,
               bringup_phase_name((BringupPhaseId)bringup_phase_model.active_phase),
               (long)bringup_phase_model.last_error,
               (unsigned long)bringup_phase_model.inject_mask);
    } else {
      snprintf(buf, sizeof(buf),
               "bringup phase-summary boot_complete=%u active=none last_error=%ld inject_mask=0x%08lX\r\n",
               (unsigned)bringup_phase_model.boot_complete,
               (long)bringup_phase_model.last_error,
               (unsigned long)bringup_phase_model.inject_mask);
    }
    log_send_blocking(buf);
  } else {
    if (bringup_phase_model.active_valid != 0U) {
      snprintf(buf, sizeof(buf),
               "{\"type\":\"bringup-phase-summary\",\"boot_complete\":%u,\"active\":\"%s\",\"last_error\":%ld,\"inject_mask\":%lu}\r\n",
               (unsigned)bringup_phase_model.boot_complete,
               bringup_phase_name((BringupPhaseId)bringup_phase_model.active_phase),
               (long)bringup_phase_model.last_error,
               (unsigned long)bringup_phase_model.inject_mask);
    } else {
      snprintf(buf, sizeof(buf),
               "{\"type\":\"bringup-phase-summary\",\"boot_complete\":%u,\"active\":\"none\",\"last_error\":%ld,\"inject_mask\":%lu}\r\n",
               (unsigned)bringup_phase_model.boot_complete,
               (long)bringup_phase_model.last_error,
               (unsigned long)bringup_phase_model.inject_mask);
    }
    log_send_blocking(buf);
  }

  for (phase = BRINGUP_PHASE_ROM_EARLY_INIT; phase < BRINGUP_PHASE_COUNT; ++phase) {
    BringupPhaseSlot slot = {0};
    if (!bringup_phase_get_slot(&bringup_phase_model, phase, &slot)) {
      continue;
    }
    if (json_mode == 0U) {
      snprintf(buf, sizeof(buf),
               "bringup phase %s st=%s att=%lu fail=%lu rb=%lu enter=%lu leave=%lu err=%ld\r\n",
               bringup_phase_name(phase),
               bringup_phase_status_name(slot.status),
               (unsigned long)slot.attempts,
               (unsigned long)slot.fail_count,
               (unsigned long)slot.rollback_count,
               (unsigned long)slot.enter_seq,
               (unsigned long)slot.leave_seq,
               (long)slot.last_error);
    } else {
      snprintf(buf, sizeof(buf),
               "{\"type\":\"bringup-phase\",\"name\":\"%s\",\"status\":\"%s\",\"attempts\":%lu,\"fails\":%lu,\"rollbacks\":%lu,\"enter_seq\":%lu,\"leave_seq\":%lu,\"last_error\":%ld}\r\n",
               bringup_phase_name(phase),
               bringup_phase_status_name(slot.status),
               (unsigned long)slot.attempts,
               (unsigned long)slot.fail_count,
               (unsigned long)slot.rollback_count,
               (unsigned long)slot.enter_seq,
               (unsigned long)slot.leave_seq,
               (long)slot.last_error);
    }
    log_send_blocking(buf);
  }
}

static void bringup_stage_log_status_dump(uint8_t json_mode)
{
  char buf[320];
  BringupStageId current_stage = BRINGUP_STAGE_INIT;
  BringupPhaseStatus current_status = BRINGUP_PHASE_STATUS_PENDING;
  BringupPhaseId current_phase = BRINGUP_PHASE_ROM_EARLY_INIT;
  uint8_t blocked = 1U;

  (void)bringup_stage_current(&bringup_phase_model,
                              &current_stage,
                              &current_status,
                              &current_phase);
  if (bringup_phase_model.boot_complete != 0U &&
      current_status == BRINGUP_PHASE_STATUS_DONE) {
    blocked = 0U;
  }

  if (json_mode == 0U) {
    snprintf(buf, sizeof(buf),
             "bringup stage-summary boot_complete=%u current=%s status=%s phase=%s blocked=%u last_error=%ld\r\n",
             (unsigned)bringup_phase_model.boot_complete,
             bringup_stage_name(current_stage),
             bringup_phase_status_name(current_status),
             bringup_phase_name(current_phase),
             (unsigned)blocked,
             (long)bringup_phase_model.last_error);
    log_send_blocking(buf);
  } else {
    snprintf(buf, sizeof(buf),
             "{\"type\":\"bringup-stage-summary\",\"boot_complete\":%u,\"current\":\"%s\",\"status\":\"%s\",\"phase\":\"%s\",\"blocked\":%u,\"last_error\":%ld}\r\n",
             (unsigned)bringup_phase_model.boot_complete,
             bringup_stage_name(current_stage),
             bringup_phase_status_name(current_status),
             bringup_phase_name(current_phase),
             (unsigned)blocked,
             (long)bringup_phase_model.last_error);
    log_send_blocking(buf);
  }

  for (uint32_t i = 0U; i < (uint32_t)BRINGUP_STAGE_COUNT; ++i) {
    BringupStageSlot slot = {0};
    BringupStageId stage = (BringupStageId)i;
    if (!bringup_stage_get_slot(&bringup_phase_model, stage, &slot)) {
      continue;
    }
    if (json_mode == 0U) {
      snprintf(buf, sizeof(buf),
               "bringup stage %s st=%s att=%lu fail=%lu rb=%lu enter=%lu leave=%lu err=%ld entry_evt=%s exit_evt=%s\r\n",
               bringup_stage_name(stage),
               bringup_phase_status_name(slot.status),
               (unsigned long)slot.attempts,
               (unsigned long)slot.fail_count,
               (unsigned long)slot.rollback_count,
               (unsigned long)slot.enter_seq,
               (unsigned long)slot.leave_seq,
               (long)slot.last_error,
               bringup_stage_entry_event(stage),
               bringup_stage_exit_event(stage));
    } else {
      snprintf(buf, sizeof(buf),
               "{\"type\":\"bringup-stage\",\"name\":\"%s\",\"status\":\"%s\",\"attempts\":%lu,\"fails\":%lu,\"rollbacks\":%lu,\"enter_seq\":%lu,\"leave_seq\":%lu,\"last_error\":%ld,\"entry_event\":\"%s\",\"exit_event\":\"%s\"}\r\n",
               bringup_stage_name(stage),
               bringup_phase_status_name(slot.status),
               (unsigned long)slot.attempts,
               (unsigned long)slot.fail_count,
               (unsigned long)slot.rollback_count,
               (unsigned long)slot.enter_seq,
               (unsigned long)slot.leave_seq,
               (long)slot.last_error,
               bringup_stage_entry_event(stage),
               bringup_stage_exit_event(stage));
    }
    log_send_blocking(buf);
  }
}

static void bringup_stage_log_wait(uint8_t json_mode)
{
  char buf[320];
  BringupStageId current_stage = BRINGUP_STAGE_INIT;
  BringupPhaseStatus current_status = BRINGUP_PHASE_STATUS_PENDING;
  BringupPhaseId current_phase = BRINGUP_PHASE_ROM_EARLY_INIT;
  KdiDriverId target_drivers[KDI_DRIVER_COUNT];
  const char *target_reasons[KDI_DRIVER_COUNT];
  KdiDriverState target_states[KDI_DRIVER_COUNT];
  int target_state_rc[KDI_DRIVER_COUNT];
  uint8_t target_waiting[KDI_DRIVER_COUNT];
  uint32_t target_count = 0U;
  uint32_t target_limit = 0U;
  uint32_t waiting_count = 0U;
  uint8_t blocked = 1U;

  (void)bringup_stage_current(&bringup_phase_model,
                              &current_stage,
                              &current_status,
                              &current_phase);
  if (bringup_phase_model.boot_complete != 0U &&
      current_status == BRINGUP_PHASE_STATUS_DONE) {
    blocked = 0U;
  }

  target_count = dependency_graph_stage_drivers(current_stage,
                                                target_drivers,
                                                target_reasons,
                                                (uint32_t)KDI_DRIVER_COUNT);
  target_limit = target_count;
  if (target_limit > (uint32_t)KDI_DRIVER_COUNT) {
    target_limit = (uint32_t)KDI_DRIVER_COUNT;
  }

  for (uint32_t i = 0U; i < target_limit; ++i) {
    KdiDriverState st = KDI_STATE_INIT;
    int st_rc = kdi_driver_get_state(target_drivers[i], &st);
    uint8_t waiting = (st_rc != KDI_OK || st != KDI_STATE_ACTIVE) ? 1U : 0U;

    target_states[i] = st;
    target_state_rc[i] = st_rc;
    target_waiting[i] = waiting;
    if (waiting != 0U) {
      waiting_count++;
    }
  }

  if (json_mode == 0U) {
    snprintf(buf, sizeof(buf),
             "bringup stage-wait-summary stage=%s status=%s blocked=%u phase=%s targets=%lu waiting=%lu\r\n",
             bringup_stage_name(current_stage),
             bringup_phase_status_name(current_status),
             (unsigned)blocked,
             bringup_phase_name(current_phase),
             (unsigned long)target_count,
             (unsigned long)waiting_count);
  } else {
    snprintf(buf, sizeof(buf),
             "{\"type\":\"bringup-stage-wait-summary\",\"stage\":\"%s\",\"status\":\"%s\",\"blocked\":%u,\"phase\":\"%s\",\"targets\":%lu,\"waiting\":%lu}\r\n",
             bringup_stage_name(current_stage),
             bringup_phase_status_name(current_status),
             (unsigned)blocked,
             bringup_phase_name(current_phase),
             (unsigned long)target_count,
             (unsigned long)waiting_count);
  }
  log_send_blocking(buf);

  for (uint32_t i = 0U; i < target_limit; ++i) {
    const char *reason = (target_reasons[i] != NULL) ? target_reasons[i] : "stage dependency";

    if (target_waiting[i] == 0U) {
      continue;
    }
    if (json_mode == 0U) {
      snprintf(buf, sizeof(buf),
               "bringup stage-wait driver=%s st=%s st_rc=%s reason=%s\r\n",
               kdi_driver_name(target_drivers[i]),
               kdi_driver_state_name(target_states[i]),
               kdi_result_name(target_state_rc[i]),
               reason);
    } else {
      snprintf(buf, sizeof(buf),
               "{\"type\":\"bringup-stage-wait-driver\",\"stage\":\"%s\",\"driver\":\"%s\",\"driver_state\":\"%s\",\"state_rc\":\"%s\",\"reason\":\"%s\"}\r\n",
               bringup_stage_name(current_stage),
               kdi_driver_name(target_drivers[i]),
               kdi_driver_state_name(target_states[i]),
               kdi_result_name(target_state_rc[i]),
               reason);
    }
    log_send_blocking(buf);

    {
      const DepResourceEdge *resource_refs[32];
      uint32_t res_count =
        dependency_graph_driver_resources(target_drivers[i],
                                          resource_refs,
                                          (uint32_t)(sizeof(resource_refs) / sizeof(resource_refs[0])));
      if (res_count > (uint32_t)(sizeof(resource_refs) / sizeof(resource_refs[0]))) {
        res_count = (uint32_t)(sizeof(resource_refs) / sizeof(resource_refs[0]));
      }
      for (uint32_t r = 0U; r < res_count; ++r) {
        const DepResourceEdge *edge = resource_refs[r];
        if (edge == NULL) {
          continue;
        }
        if (json_mode == 0U) {
          snprintf(buf, sizeof(buf),
                   "bringup stage-wait resource driver=%s dep=%s:%s reason=%s\r\n",
                   kdi_driver_name(edge->driver),
                   dependency_graph_resource_kind_name(edge->kind),
                   edge->resource_id,
                   edge->reason);
        } else {
          snprintf(buf, sizeof(buf),
                   "{\"type\":\"bringup-stage-wait-resource\",\"stage\":\"%s\",\"driver\":\"%s\",\"kind\":\"%s\",\"resource\":\"%s\",\"reason\":\"%s\"}\r\n",
                   bringup_stage_name(current_stage),
                   kdi_driver_name(edge->driver),
                   dependency_graph_resource_kind_name(edge->kind),
                   edge->resource_id,
                   edge->reason);
        }
        log_send_blocking(buf);
      }
    }
  }

  if (blocked != 0U && waiting_count == 0U) {
    const char *fallback_reason = "phase sequencing barrier";
    BringupPhaseId wait_phase = current_phase;

    if (current_status == BRINGUP_PHASE_STATUS_PENDING &&
        current_phase > BRINGUP_PHASE_ROM_EARLY_INIT) {
      wait_phase = (BringupPhaseId)((uint32_t)current_phase - 1U);
      fallback_reason = "waiting previous phase done";
    } else if (current_status == BRINGUP_PHASE_STATUS_ROLLED_BACK) {
      fallback_reason = "phase rolled back and pending rerun";
    } else if (current_status == BRINGUP_PHASE_STATUS_FAILED) {
      fallback_reason = "phase failed and pending recovery";
    }

    if (json_mode == 0U) {
      snprintf(buf, sizeof(buf),
               "bringup stage-wait fallback phase=%s reason=%s\r\n",
               bringup_phase_name(wait_phase),
               fallback_reason);
    } else {
      snprintf(buf, sizeof(buf),
               "{\"type\":\"bringup-stage-wait-fallback\",\"stage\":\"%s\",\"phase\":\"%s\",\"reason\":\"%s\"}\r\n",
               bringup_stage_name(current_stage),
               bringup_phase_name(wait_phase),
               fallback_reason);
    }
    log_send_blocking(buf);
  }
}

static int bringup_phase_driver_is_active(KdiDriverId driver)
{
  KdiDriverState st = KDI_STATE_DEAD;
  return (kdi_driver_get_state(driver, &st) == KDI_OK && st == KDI_STATE_ACTIVE) ? 1 : 0;
}

static int bringup_phase_live_check(BringupPhaseId phase, int32_t *out_error)
{
  uint32_t shcsr_need = (SCB_SHCSR_USGFAULTENA_Msk |
                         SCB_SHCSR_BUSFAULTENA_Msk |
                         SCB_SHCSR_MEMFAULTENA_Msk);
  int32_t err = 0;
  int ok = 0;

  switch (phase) {
    case BRINGUP_PHASE_ROM_EARLY_INIT:
      ok = (SystemCoreClock >= BRINGUP_CLK_MIN_HZ && SystemCoreClock <= BRINGUP_CLK_MAX_HZ &&
            g_shared_ctrl.build_id != 0U) ? 1 : 0;
      err = -2101;
      break;
    case BRINGUP_PHASE_MPU_SETUP:
#if APP_DISABLE_MPU
      ok = (SCB->VTOR == FLASH_BASE &&
            (SCB->SHCSR & shcsr_need) == shcsr_need &&
            (MPU->CTRL & MPU_CTRL_ENABLE_Msk) == 0U) ? 1 : 0;
#else
      ok = (SCB->VTOR == FLASH_BASE &&
            (SCB->SHCSR & shcsr_need) == shcsr_need &&
            (MPU->CTRL & MPU_CTRL_ENABLE_Msk) != 0U) ? 1 : 0;
#endif
      err = -2201;
      break;
    case BRINGUP_PHASE_KERNEL_START:
      ok = (xPortGetFreeHeapSize() >= 4096U && log_queue != NULL) ? 1 : 0;
      err = -2301;
      break;
    case BRINGUP_PHASE_DRIVER_PROBE_DIAG:
      ok = bringup_phase_driver_is_active(KDI_DRIVER_DIAG);
      err = -2401;
      break;
    case BRINGUP_PHASE_DRIVER_PROBE_UART:
      ok = bringup_phase_driver_is_active(KDI_DRIVER_UART);
      err = -2402;
      break;
    case BRINGUP_PHASE_DRIVER_PROBE_SENSOR:
      ok = bringup_phase_driver_is_active(KDI_DRIVER_SENSOR);
      err = -2403;
      break;
    case BRINGUP_PHASE_DRIVER_PROBE_VM:
      ok = bringup_phase_driver_is_active(KDI_DRIVER_VM_RUNTIME);
      err = -2404;
      break;
    case BRINGUP_PHASE_SERVICE_REGISTRATION:
      ok = (log_queue != NULL &&
            ipc_cmd_queue != NULL &&
            ipc_resp_queue != NULL &&
            g_shared_ctrl.ipc_cmd_q != 0U &&
            g_shared_ctrl.ipc_resp_q != 0U) ? 1 : 0;
      err = -2501;
      break;
    case BRINGUP_PHASE_USER_WORKLOAD_ENABLE:
      ok = (bringup_user_task_created != 0U && g_cli_ready != 0U) ? 1 : 0;
      err = -2601;
      break;
    default:
      ok = 0;
      err = -2999;
      break;
  }

  if (out_error != NULL) {
    *out_error = ok ? 0 : err;
  }
  return ok;
}

static void bringup_phase_run_pipeline(uint8_t reset_model, BringupPhaseId start_phase)
{
  char buf[160];
  BringupPhaseId phase;

  if (reset_model != 0U) {
    bringup_phase_reset_execution(&bringup_phase_model);
    start_phase = BRINGUP_PHASE_ROM_EARLY_INIT;
  } else {
    (void)bringup_phase_rollback_from(&bringup_phase_model, start_phase);
  }

  for (phase = start_phase; phase < BRINGUP_PHASE_COUNT; ++phase) {
    int rc = bringup_phase_begin(&bringup_phase_model, phase);
    int32_t err_code = 0;
    int32_t injected = 0;

    if (rc != 0) {
      snprintf(buf, sizeof(buf), "bringup phase run begin %s: rc=%d\r\n",
               bringup_phase_name(phase), rc);
      log_send_blocking(buf);
      return;
    }

    if (bringup_phase_consume_injected_failure(&bringup_phase_model, phase, &injected)) {
      (void)bringup_phase_fail(&bringup_phase_model, phase, injected);
      (void)bringup_phase_rollback_from(&bringup_phase_model, phase);
      snprintf(buf, sizeof(buf),
               "bringup phase run fail %s: injected(%ld), logical rollback applied\r\n",
               bringup_phase_name(phase), (long)injected);
      log_send_blocking(buf);
      return;
    }

    if (!bringup_phase_live_check(phase, &err_code)) {
      (void)bringup_phase_fail(&bringup_phase_model, phase, err_code);
      (void)bringup_phase_rollback_from(&bringup_phase_model, phase);
      snprintf(buf, sizeof(buf),
               "bringup phase run fail %s: check(%ld), logical rollback applied\r\n",
               bringup_phase_name(phase), (long)err_code);
      log_send_blocking(buf);
      return;
    }

    rc = bringup_phase_succeed(&bringup_phase_model, phase);
    if (rc != 0) {
      snprintf(buf, sizeof(buf), "bringup phase run commit %s: rc=%d\r\n",
               bringup_phase_name(phase), rc);
      log_send_blocking(buf);
      return;
    }
  }

  log_send_blocking("bringup phase run: ok\r\n");
}

static void bringup_handle_cmd(char *args)
{
  char *p = args;
  char *cmd = next_token(&p);

  if (cmd == NULL || strcmp(cmd, "check") == 0) {
    bringup_log_check();
    return;
  }
  if (strcmp(cmd, "json") == 0) {
    bringup_log_json();
    return;
  }
  if (strcmp(cmd, "mpu") == 0) {
    bringup_log_mpu();
    return;
  }
  if (strcmp(cmd, "stage") == 0) {
    char *action = next_token(&p);
    if (action == NULL || strcmp(action, "show") == 0 || strcmp(action, "dump") == 0) {
      bringup_stage_log_status_dump(0U);
      bringup_stage_log_wait(0U);
      return;
    }
    if (strcmp(action, "json") == 0) {
      bringup_stage_log_status_dump(1U);
      bringup_stage_log_wait(1U);
      return;
    }
    if (strcmp(action, "wait") == 0) {
      bringup_stage_log_wait(0U);
      return;
    }
    if (strcmp(action, "wait-json") == 0) {
      bringup_stage_log_wait(1U);
      return;
    }
    bringup_phase_log_usage();
    return;
  }
  if (strcmp(cmd, "phase") == 0) {
    char *action = next_token(&p);
    char buf[160];

    if (action == NULL || strcmp(action, "show") == 0 || strcmp(action, "dump") == 0) {
      bringup_phase_log_status_dump(0U);
      return;
    }
    if (strcmp(action, "json") == 0) {
      bringup_phase_log_status_dump(1U);
      return;
    }
    if (strcmp(action, "reset") == 0) {
      bringup_phase_reset_execution(&bringup_phase_model);
      log_send_blocking("bringup phase reset: ok\r\n");
      return;
    }
    if (strcmp(action, "run") == 0) {
      bringup_phase_run_pipeline(1U, BRINGUP_PHASE_ROM_EARLY_INIT);
      return;
    }
    if (strcmp(action, "rerun") == 0) {
      BringupPhaseId phase = BRINGUP_PHASE_ROM_EARLY_INIT;
      char *phase_txt = next_token(&p);
      if (!bringup_phase_parse_name(phase_txt, &phase)) {
        bringup_phase_log_usage();
        return;
      }
      bringup_phase_run_pipeline(0U, phase);
      return;
    }
    if (strcmp(action, "rollback") == 0) {
      BringupPhaseId phase = BRINGUP_PHASE_ROM_EARLY_INIT;
      char *phase_txt = next_token(&p);
      uint32_t changed;
      if (!bringup_phase_parse_name(phase_txt, &phase)) {
        bringup_phase_log_usage();
        return;
      }
      changed = bringup_phase_rollback_from(&bringup_phase_model, phase);
      snprintf(buf, sizeof(buf),
               "bringup phase rollback %s: changed=%lu (logical)\r\n",
               bringup_phase_name(phase), (unsigned long)changed);
      log_send_blocking(buf);
      return;
    }
    if (strcmp(action, "inject") == 0) {
      BringupPhaseId phase = BRINGUP_PHASE_ROM_EARLY_INIT;
      char *phase_txt = next_token(&p);
      char *code_txt = next_token(&p);
      long code = 0L;
      char *end = NULL;
      if (!bringup_phase_parse_name(phase_txt, &phase)) {
        bringup_phase_log_usage();
        return;
      }
      if (code_txt != NULL) {
        code = strtol(code_txt, &end, 0);
        if (end == code_txt || *end != '\0') {
          bringup_phase_log_usage();
          return;
        }
      }
      (void)bringup_phase_set_injected_failure(&bringup_phase_model, phase, (int32_t)code);
      snprintf(buf, sizeof(buf), "bringup phase inject %s: ok code=%ld\r\n",
               bringup_phase_name(phase),
               (long)bringup_phase_model.inject_error[(uint32_t)phase]);
      log_send_blocking(buf);
      return;
    }
    if (strcmp(action, "clearfail") == 0) {
      char *phase_txt = next_token(&p);
      if (phase_txt == NULL || strcmp(phase_txt, "all") == 0) {
        bringup_phase_clear_all_injected_failures(&bringup_phase_model);
        log_send_blocking("bringup phase clearfail all: ok\r\n");
        return;
      } else {
        BringupPhaseId phase = BRINGUP_PHASE_ROM_EARLY_INIT;
        if (!bringup_phase_parse_name(phase_txt, &phase)) {
          bringup_phase_log_usage();
          return;
        }
        bringup_phase_clear_injected_failure(&bringup_phase_model, phase);
        snprintf(buf, sizeof(buf), "bringup phase clearfail %s: ok\r\n",
                 bringup_phase_name(phase));
        log_send_blocking(buf);
        return;
      }
    }

    bringup_phase_log_usage();
    return;
  }

  bringup_phase_log_usage();
}

static void bringup_boot_phase_begin(BringupPhaseId phase)
{
  (void)bringup_phase_begin(&bringup_phase_model, phase);
}

static void bringup_boot_phase_succeed(BringupPhaseId phase)
{
  (void)bringup_phase_succeed(&bringup_phase_model, phase);
}

static void bringup_boot_phase_fail(BringupPhaseId phase, int32_t err)
{
  (void)bringup_phase_fail(&bringup_phase_model, phase, err);
  (void)bringup_phase_rollback_from(&bringup_phase_model, phase);
}

static void bringup_boot_phase_fail_halt(BringupPhaseId phase,
                                         int32_t err,
                                         const char *msg)
{
  bringup_boot_phase_fail(phase, err);
  board_uart_write(msg);
  for (;;) {
  }
}

static int kdi_bootstrap_require(const char *label, int rc)
{
  char buf[96];

  if (rc == KDI_OK) {
    return 1;
  }
  snprintf(buf, sizeof(buf), "kdi boot fail %s: %s\r\n", label, kdi_result_name(rc));
  board_uart_write(buf);
  return 0;
}

static int kdi_bootstrap_driver_online(const char *label, KdiDriverId driver, KdiCapToken token)
{
  char stage[48];

  snprintf(stage, sizeof(stage), "%s_probe", label);
  if (!kdi_bootstrap_require(stage, kdi_driver_probe(driver, token))) {
    return 0;
  }
  snprintf(stage, sizeof(stage), "%s_ready", label);
  if (!kdi_bootstrap_require(stage, kdi_driver_probe_done(driver, token, 1U))) {
    return 0;
  }
  snprintf(stage, sizeof(stage), "%s_active", label);
  if (!kdi_bootstrap_require(stage, kdi_driver_activate(driver, token))) {
    return 0;
  }
  return 1;
}

static int kdi_bootstrap_contracts(void)
{
  KdiIrqRequest irq_req;
  KdiDmaRequest dma_req;
  KdiMpuRequest mpu_req;

  bringup_boot_phase_begin(BRINGUP_PHASE_DRIVER_PROBE_DIAG);
  kdi_init();
  kdi_set_now_ms_fn(kdi_now_ms_from_rtos);

  if (!kdi_bootstrap_require("ttl_uart", kdi_set_token_ttl_ms(KDI_DRIVER_UART, 120000U))) {
    bringup_boot_phase_fail(BRINGUP_PHASE_DRIVER_PROBE_DIAG, -2411);
    return 0;
  }
  if (!kdi_bootstrap_require("ttl_sensor", kdi_set_token_ttl_ms(KDI_DRIVER_SENSOR, 120000U))) {
    bringup_boot_phase_fail(BRINGUP_PHASE_DRIVER_PROBE_DIAG, -2412);
    return 0;
  }
  if (!kdi_bootstrap_require("ttl_vm", kdi_set_token_ttl_ms(KDI_DRIVER_VM_RUNTIME, 60000U))) {
    bringup_boot_phase_fail(BRINGUP_PHASE_DRIVER_PROBE_DIAG, -2413);
    return 0;
  }
  if (!kdi_bootstrap_require("ttl_diag", kdi_set_token_ttl_ms(KDI_DRIVER_DIAG, 300000U))) {
    bringup_boot_phase_fail(BRINGUP_PHASE_DRIVER_PROBE_DIAG, -2414);
    return 0;
  }

  if (!kdi_bootstrap_require("token_kernel", kdi_acquire_token(KDI_DRIVER_KERNEL, &kdi_caps.kernel))) {
    bringup_boot_phase_fail(BRINGUP_PHASE_DRIVER_PROBE_DIAG, -2415);
    return 0;
  }
  if (!kdi_bootstrap_require("token_diag", kdi_acquire_token(KDI_DRIVER_DIAG, &kdi_caps.diag))) {
    bringup_boot_phase_fail(BRINGUP_PHASE_DRIVER_PROBE_DIAG, -2416);
    return 0;
  }
  if (!kdi_bootstrap_require("token_uart", kdi_acquire_token(KDI_DRIVER_UART, &kdi_caps.uart))) {
    bringup_boot_phase_fail(BRINGUP_PHASE_DRIVER_PROBE_DIAG, -2417);
    return 0;
  }
  if (!kdi_bootstrap_require("token_sensor", kdi_acquire_token(KDI_DRIVER_SENSOR, &kdi_caps.sensor))) {
    bringup_boot_phase_fail(BRINGUP_PHASE_DRIVER_PROBE_DIAG, -2418);
    return 0;
  }
  if (!kdi_bootstrap_require("token_vm", kdi_acquire_token(KDI_DRIVER_VM_RUNTIME, &kdi_caps.vm_runtime))) {
    bringup_boot_phase_fail(BRINGUP_PHASE_DRIVER_PROBE_DIAG, -2419);
    return 0;
  }

  if (!kdi_bootstrap_driver_online("diag", KDI_DRIVER_DIAG, kdi_caps.diag)) {
    bringup_boot_phase_fail(BRINGUP_PHASE_DRIVER_PROBE_DIAG, -2420);
    return 0;
  }
  if (!kdi_bootstrap_require("diag_power", kdi_power_hook(KDI_DRIVER_DIAG, kdi_caps.diag, KDI_POWER_ONLINE))) {
    bringup_boot_phase_fail(BRINGUP_PHASE_DRIVER_PROBE_DIAG, -2421);
    return 0;
  }
  if (!kdi_bootstrap_require("diag_reset", kdi_reset_hook(KDI_DRIVER_DIAG, kdi_caps.diag, KDI_RESET_COMPLETE))) {
    bringup_boot_phase_fail(BRINGUP_PHASE_DRIVER_PROBE_DIAG, -2422);
    return 0;
  }
  bringup_boot_phase_succeed(BRINGUP_PHASE_DRIVER_PROBE_DIAG);

  bringup_boot_phase_begin(BRINGUP_PHASE_DRIVER_PROBE_UART);
  if (!kdi_bootstrap_driver_online("uart", KDI_DRIVER_UART, kdi_caps.uart)) {
    bringup_boot_phase_fail(BRINGUP_PHASE_DRIVER_PROBE_UART, -2430);
    return 0;
  }

  irq_req.irqn = (int16_t)KDI_BOOT_UART_IRQn;
  irq_req.priority = 5U;
  if (!kdi_bootstrap_require("uart_irq", kdi_request_irq(KDI_DRIVER_UART, kdi_caps.uart, &irq_req))) {
    bringup_boot_phase_fail(BRINGUP_PHASE_DRIVER_PROBE_UART, -2431);
    return 0;
  }
  bringup_boot_phase_succeed(BRINGUP_PHASE_DRIVER_PROBE_UART);

  bringup_boot_phase_begin(BRINGUP_PHASE_DRIVER_PROBE_SENSOR);
  if (!kdi_bootstrap_driver_online("sensor", KDI_DRIVER_SENSOR, kdi_caps.sensor)) {
    bringup_boot_phase_fail(BRINGUP_PHASE_DRIVER_PROBE_SENSOR, -2440);
    return 0;
  }
  dma_req.base = (uintptr_t)&g_shared_adc;
  dma_req.size = SHARED_ADC_REGION_SIZE;
  dma_req.align = 4U;
  dma_req.direction = 0U;
  if (!kdi_bootstrap_require("sensor_dma", kdi_declare_dma_buffer(KDI_DRIVER_SENSOR, kdi_caps.sensor, &dma_req))) {
    bringup_boot_phase_fail(BRINGUP_PHASE_DRIVER_PROBE_SENSOR, -2441);
    return 0;
  }
  bringup_boot_phase_succeed(BRINGUP_PHASE_DRIVER_PROBE_SENSOR);

  bringup_boot_phase_begin(BRINGUP_PHASE_DRIVER_PROBE_VM);
  if (!kdi_bootstrap_driver_online("vm", KDI_DRIVER_VM_RUNTIME, kdi_caps.vm_runtime)) {
    bringup_boot_phase_fail(BRINGUP_PHASE_DRIVER_PROBE_VM, -2450);
    return 0;
  }

  mpu_req.region_index = 0U;
  mpu_req.base = (uint32_t)(uintptr_t)&g_shared_adc;
  mpu_req.size = SHARED_ADC_REGION_SIZE;
  mpu_req.attrs = 0U;
  if (!kdi_bootstrap_require("vm_mpu_adc", kdi_request_mpu_region(KDI_DRIVER_VM_RUNTIME, kdi_caps.vm_runtime, &mpu_req))) {
    bringup_boot_phase_fail(BRINGUP_PHASE_DRIVER_PROBE_VM, -2451);
    return 0;
  }
  mpu_req.region_index = 1U;
  mpu_req.base = (uint32_t)(uintptr_t)&g_shared_ctrl;
  mpu_req.size = SHARED_CTRL_REGION_SIZE;
  if (!kdi_bootstrap_require("vm_mpu_ctrl", kdi_request_mpu_region(KDI_DRIVER_VM_RUNTIME, kdi_caps.vm_runtime, &mpu_req))) {
    bringup_boot_phase_fail(BRINGUP_PHASE_DRIVER_PROBE_VM, -2452);
    return 0;
  }
  mpu_req.region_index = 2U;
  mpu_req.base = (uint32_t)(uintptr_t)&g_shared_stats;
  mpu_req.size = STATS_BUF_REGION_SIZE;
  if (!kdi_bootstrap_require("vm_mpu_stats", kdi_request_mpu_region(KDI_DRIVER_VM_RUNTIME, kdi_caps.vm_runtime, &mpu_req))) {
    bringup_boot_phase_fail(BRINGUP_PHASE_DRIVER_PROBE_VM, -2453);
    return 0;
  }
  bringup_boot_phase_succeed(BRINGUP_PHASE_DRIVER_PROBE_VM);

  board_uart_write("kdi boot ok\r\n");
  return 1;
}

static void kdi_log_usage(void)
{
  log_send_blocking("kdi show|last|probe <allow|deny|authfail>|token <show|rotate|revoke|ttl>|driver <show|probe|ready|fail|active|error|reset|reinit|reclaim>|irq <show|budget|cooldown|starve|enter|defer|exit|worker|poll|unsafe|storm>|fault <show|domain>|cap <show|json|review|review-json> [driver]\r\n");
}

static void kdi_log_show(void)
{
  char buf[176];
  KdiStats stats;
  KdiIrqStats irq_stats;

  kdi_get_stats(&stats);
  kdi_irq_get_stats(&irq_stats);
  snprintf(buf, sizeof(buf),
           "kdi stats allow=%lu deny=%lu reject=%lu faults=%lu\r\n",
           (unsigned long)stats.allow_total,
           (unsigned long)stats.deny_total,
           (unsigned long)stats.reject_total,
           (unsigned long)stats.fault_reports);
  log_send_blocking(buf);
  snprintf(buf, sizeof(buf),
           "kdi stats auth=%lu tok_rot=%lu tok_rev=%lu tok_exp=%lu st_fail=%lu reclaim=%lu\r\n",
           (unsigned long)stats.auth_fail_total,
           (unsigned long)stats.token_rotate_total,
           (unsigned long)stats.token_revoke_total,
           (unsigned long)stats.token_expire_total,
           (unsigned long)stats.state_fail_total,
           (unsigned long)stats.force_reclaim_total);
  log_send_blocking(buf);
  snprintf(buf, sizeof(buf),
           "kdi irq enter=%lu thr=%lu cool=%lu recov=%lu defer=%lu drop=%lu worker=%lu werr=%lu starve=%lu unsafe=%lu pending=%lu\r\n",
           (unsigned long)irq_stats.irq_enter_total,
           (unsigned long)irq_stats.irq_throttle_total,
           (unsigned long)irq_stats.irq_cooldown_total,
           (unsigned long)irq_stats.irq_recover_total,
           (unsigned long)irq_stats.irq_defer_total,
           (unsigned long)irq_stats.irq_drop_total,
           (unsigned long)irq_stats.irq_worker_total,
           (unsigned long)irq_stats.irq_worker_error_total,
           (unsigned long)irq_stats.irq_starvation_total,
           (unsigned long)irq_stats.irq_unsafe_total,
           (unsigned long)irq_stats.deferred_pending);
  log_send_blocking(buf);

  for (uint32_t i = 0U; i < (uint32_t)KDI_DRIVER_COUNT; ++i) {
    const KdiPolicy *p = kdi_get_policy((KdiDriverId)i);
    KdiDriverState st = KDI_STATE_INIT;
    (void)kdi_driver_get_state((KdiDriverId)i, &st);
    if (p == NULL) {
      continue;
    }
    snprintf(buf, sizeof(buf),
             "kdi drv=%s st=%s mpu=%u irq=%u dma=%u fault=%u pwr=%u rst=%u\r\n",
             kdi_driver_name((KdiDriverId)i),
             kdi_driver_state_name(st),
             (unsigned)p->allow_mpu,
             (unsigned)p->allow_irq,
             (unsigned)p->allow_dma,
             (unsigned)p->allow_fault,
             (unsigned)p->allow_power,
             (unsigned)p->allow_reset);
    log_send_blocking(buf);
    snprintf(buf, sizeof(buf),
             "kdi lim drv=%s mpu_max=%u irq=[%d..%d] dma_max=%lu\r\n",
             kdi_driver_name((KdiDriverId)i),
             (unsigned)p->max_mpu_regions,
             (int)p->min_irqn,
             (int)p->max_irqn,
             (unsigned long)p->max_dma_bytes);
    log_send_blocking(buf);
  }
}

static void kdi_log_last(void)
{
  char buf[144];
  KdiDecision last;
  int rc = kdi_last_decision(&last);

  if (rc != KDI_OK) {
    log_send_blocking("kdi last: none\r\n");
    return;
  }
  snprintf(buf, sizeof(buf),
           "kdi last drv=%s req=%s rc=%s arg0=0x%08lX arg1=0x%08lX\r\n",
           kdi_driver_name((KdiDriverId)last.driver),
           kdi_request_name((KdiRequestType)last.req),
           kdi_result_name(last.rc),
           (unsigned long)last.arg0,
           (unsigned long)last.arg1);
  log_send_blocking(buf);
}

static void kdi_probe_allow(void)
{
  char buf[96];
  KdiIrqRequest irq_req = {
    .irqn = (int16_t)KDI_BOOT_UART_IRQn,
    .priority = 6U,
  };
  int rc = kdi_request_irq(KDI_DRIVER_UART, kdi_caps.uart, &irq_req);
  snprintf(buf, sizeof(buf), "kdi probe allow: %s\r\n", kdi_result_name(rc));
  log_send_blocking(buf);
}

static void kdi_probe_deny(void)
{
  char buf[96];
  int rc = kdi_reset_hook(KDI_DRIVER_SENSOR, kdi_caps.sensor, KDI_RESET_ASSERT);
  snprintf(buf, sizeof(buf), "kdi probe deny: %s\r\n", kdi_result_name(rc));
  log_send_blocking(buf);
}

static void kdi_probe_authfail(void)
{
  char buf[96];
  int rc = kdi_reset_hook(KDI_DRIVER_SENSOR, KDI_CAP_INVALID, KDI_RESET_ASSERT);
  snprintf(buf, sizeof(buf), "kdi probe authfail: %s\r\n", kdi_result_name(rc));
  log_send_blocking(buf);
}

static int kdi_parse_driver(const char *name, KdiDriverId *out)
{
  if (name == NULL || out == NULL) {
    return 0;
  }
  if (strcmp(name, "kernel") == 0) {
    *out = KDI_DRIVER_KERNEL;
    return 1;
  }
  if (strcmp(name, "uart") == 0) {
    *out = KDI_DRIVER_UART;
    return 1;
  }
  if (strcmp(name, "sensor") == 0) {
    *out = KDI_DRIVER_SENSOR;
    return 1;
  }
  if (strcmp(name, "vm") == 0 || strcmp(name, "vm-runtime") == 0) {
    *out = KDI_DRIVER_VM_RUNTIME;
    return 1;
  }
  if (strcmp(name, "diag") == 0) {
    *out = KDI_DRIVER_DIAG;
    return 1;
  }
  return 0;
}

static KdiCapToken *kdi_cap_slot(KdiDriverId driver)
{
  switch (driver) {
    case KDI_DRIVER_KERNEL:
      return &kdi_caps.kernel;
    case KDI_DRIVER_UART:
      return &kdi_caps.uart;
    case KDI_DRIVER_SENSOR:
      return &kdi_caps.sensor;
    case KDI_DRIVER_VM_RUNTIME:
      return &kdi_caps.vm_runtime;
    case KDI_DRIVER_DIAG:
      return &kdi_caps.diag;
    default:
      return NULL;
  }
}

static void kdi_log_token_usage(void)
{
  log_send_blocking("kdi token show | kdi token rotate|revoke <kernel|uart|sensor|vm|diag>\r\n");
  log_send_blocking("kdi token ttl <kernel|uart|sensor|vm|diag> <ms>  (0 means never expire)\r\n");
}

static void kdi_log_driver_usage(void)
{
  log_send_blocking("kdi driver show | kdi driver probe|ready|fail|active <driver>\r\n");
  log_send_blocking("kdi driver error <driver> <code> | reset|reinit|reclaim <driver>\r\n");
}

static void kdi_log_irq_usage(void)
{
  log_send_blocking("kdi irq show | kdi irq budget <driver> <per_sec> | kdi irq cooldown <driver> <base_ms> [max_ms]\r\n");
  log_send_blocking("kdi irq starve <ms>\r\n");
  log_send_blocking("kdi irq enter|defer|exit <driver> [work_id] [arg] | kdi irq worker [n]\r\n");
  log_send_blocking("kdi irq poll | kdi irq unsafe <driver> <malloc|printf|policy>\r\n");
  log_send_blocking("kdi irq storm <driver> <count>\r\n");
}

static void kdi_log_fault_usage(void)
{
  log_send_blocking("kdi fault show | kdi fault domain <kernel|uart|sensor|vm|diag>\r\n");
}

static void kdi_cap_mask_text(uint32_t mask, char *buf, size_t buf_size)
{
  size_t used = 0U;
  uint8_t wrote = 0U;

  if (buf == NULL || buf_size == 0U) {
    return;
  }
  buf[0] = '\0';

  for (uint32_t req = (uint32_t)KDI_REQ_MPU; req <= (uint32_t)KDI_REQ_RESET; ++req) {
    if ((mask & KDI_CAP_REQ_BIT(req)) == 0U) {
      continue;
    }
    used += (size_t)snprintf(buf + used,
                             (used < buf_size) ? (buf_size - used) : 0U,
                             "%s%s",
                             wrote ? "," : "",
                             kdi_request_name((KdiRequestType)req));
    wrote = 1U;
    if (used >= buf_size) {
      break;
    }
  }

  if (!wrote) {
    (void)snprintf(buf, buf_size, "none");
  }
}

static uint32_t kdi_cap_total_requests(const KdiCapUsageTrace *trace)
{
  uint32_t total = 0U;

  if (trace == NULL) {
    return 0U;
  }
  for (uint32_t req = (uint32_t)KDI_REQ_MPU; req <= (uint32_t)KDI_REQ_RESET; ++req) {
    total += trace->request_total[req];
  }
  return total;
}

static const char *kdi_cap_policy_allow_field(KdiRequestType req)
{
  switch (req) {
    case KDI_REQ_MPU:
      return "allow_mpu";
    case KDI_REQ_IRQ:
      return "allow_irq";
    case KDI_REQ_DMA:
      return "allow_dma";
    case KDI_REQ_FAULT:
      return "allow_fault";
    case KDI_REQ_POWER:
      return "allow_power";
    case KDI_REQ_RESET:
      return "allow_reset";
    default:
      return "allow_unknown";
  }
}

static const char *kdi_cap_usage_window_text(const KdiCapUsageTrace *trace,
                                             KdiRequestType req,
                                             char *buf,
                                             size_t buf_size)
{
  uint32_t idx;

  if (buf == NULL || buf_size == 0U) {
    return "none";
  }
  if (trace == NULL) {
    (void)snprintf(buf, buf_size, "none");
    return buf;
  }

  idx = (uint32_t)req;
  if (idx > (uint32_t)KDI_REQ_RESET ||
      trace->request_total[idx] == 0U) {
    (void)snprintf(buf, buf_size, "none");
    return buf;
  }

  (void)snprintf(buf, buf_size, "[%lu,%lu]",
                 (unsigned long)trace->request_first_ms[idx],
                 (unsigned long)trace->request_last_ms[idx]);
  return buf;
}

static uint8_t kdi_cap_review_risk_level(uint32_t obs_window_ms,
                                         uint32_t total_requests,
                                         KdiDriverState state)
{
  if ((state == KDI_STATE_ACTIVE || state == KDI_STATE_READY) &&
      obs_window_ms >= 30000U &&
      total_requests >= 6U) {
    return 1U;
  }
  if (obs_window_ms >= 8000U &&
      total_requests >= 2U &&
      state != KDI_STATE_INIT &&
      state != KDI_STATE_PROBE) {
    return 2U;
  }
  return 3U;
}

static const char *kdi_cap_review_risk_name(uint8_t risk)
{
  switch (risk) {
    case 1U:
      return "low";
    case 2U:
      return "medium";
    case 3U:
      return "high";
    default:
      return "unknown";
  }
}

static const char *kdi_cap_review_risk_note(uint8_t risk)
{
  switch (risk) {
    case 1U:
      return "No usage in a long stable window; removal is low risk.";
    case 2U:
      return "No usage observed but coverage is moderate; verify suspend/recovery paths before removal.";
    case 3U:
      return "Observation window is short or driver is unstable; keep until broader workload coverage.";
    default:
      return "Insufficient evidence to grade removal risk.";
  }
}

static const char *kdi_cap_review_risk_hint(uint8_t risk)
{
  switch (risk) {
    case 1U:
      return "long-window-no-usage";
    case 2U:
      return "coverage-limited-recheck";
    case 3U:
      return "short-window-or-unstable";
    default:
      return "insufficient-evidence";
  }
}

static void kdi_log_cap_usage(void)
{
  log_send_blocking("kdi cap show|json|review|review-json [kernel|uart|sensor|vm|diag]\r\n");
}

static void kdi_log_cap_state(uint8_t json_mode, uint8_t filter_valid, KdiDriverId filter_driver)
{
  char buf[512];

  for (uint32_t i = 0U; i < (uint32_t)KDI_DRIVER_COUNT; ++i) {
    KdiDriverId driver = (KdiDriverId)i;
    KdiCapUsageTrace trace = {0};
    char declared[48];
    char used[48];
    char active[48];
    char declared_not_used[48];
    int rc;

    if (filter_valid != 0U && driver != filter_driver) {
      continue;
    }

    rc = kdi_cap_usage_get(driver, &trace);
    kdi_cap_mask_text(trace.declared_mask, declared, sizeof(declared));
    kdi_cap_mask_text(trace.used_mask, used, sizeof(used));
    kdi_cap_mask_text(trace.active_mask, active, sizeof(active));
    kdi_cap_mask_text(trace.declared_not_used_mask, declared_not_used, sizeof(declared_not_used));

    if (json_mode == 0U) {
      snprintf(buf, sizeof(buf),
               "kdi cap drv=%s declared=%s used=%s active=%s decl_unused=%s obs_ms=[%lu,%lu] rc=%s\r\n",
               kdi_driver_name(driver),
               declared,
               used,
               active,
               declared_not_used,
               (unsigned long)trace.observation_start_ms,
               (unsigned long)trace.observation_end_ms,
               kdi_result_name(rc));
    } else {
      snprintf(buf, sizeof(buf),
               "{\"type\":\"kdi-cap\",\"driver\":\"%s\",\"declared\":\"%s\",\"used\":\"%s\",\"active\":\"%s\",\"declared_not_used\":\"%s\",\"declared_mask\":%lu,\"used_mask\":%lu,\"active_mask\":%lu,\"declared_not_used_mask\":%lu,\"obs_start_ms\":%lu,\"obs_end_ms\":%lu,\"obs_window_ms\":%lu,\"rc\":\"%s\"}\r\n",
               kdi_driver_name(driver),
               declared,
               used,
               active,
               declared_not_used,
               (unsigned long)trace.declared_mask,
               (unsigned long)trace.used_mask,
               (unsigned long)trace.active_mask,
               (unsigned long)trace.declared_not_used_mask,
               (unsigned long)trace.observation_start_ms,
               (unsigned long)trace.observation_end_ms,
               (unsigned long)trace.observation_window_ms,
               kdi_result_name(rc));
    }
    log_send_blocking(buf);

    if (json_mode == 0U) {
      snprintf(buf, sizeof(buf),
               "kdi cap cnt drv=%s mpu=%lu irq=%lu dma=%lu fault=%lu power=%lu reset=%lu\r\n",
               kdi_driver_name(driver),
               (unsigned long)trace.request_total[KDI_REQ_MPU],
               (unsigned long)trace.request_total[KDI_REQ_IRQ],
               (unsigned long)trace.request_total[KDI_REQ_DMA],
               (unsigned long)trace.request_total[KDI_REQ_FAULT],
               (unsigned long)trace.request_total[KDI_REQ_POWER],
               (unsigned long)trace.request_total[KDI_REQ_RESET]);
      log_send_blocking(buf);
    }
  }
}

static void kdi_log_cap_review_driver(KdiDriverId driver,
                                      const KdiCapUsageTrace *trace,
                                      KdiDriverState state,
                                      uint8_t json_mode,
                                      const char *prefix)
{
  char buf[560];
  uint8_t emitted = 0U;
  uint32_t total_requests = kdi_cap_total_requests(trace);
  uint8_t risk = kdi_cap_review_risk_level(trace->observation_window_ms,
                                           total_requests,
                                           state);
  const char *risk_name = kdi_cap_review_risk_name(risk);
  const char *risk_note = kdi_cap_review_risk_note(risk);

  for (uint32_t req = (uint32_t)KDI_REQ_MPU; req <= (uint32_t)KDI_REQ_RESET; ++req) {
    uint32_t bit = KDI_CAP_REQ_BIT(req);
    const char *cap_name = kdi_request_name((KdiRequestType)req);
    const char *policy_field = kdi_cap_policy_allow_field((KdiRequestType)req);
    char usage_window[40];

    if ((trace->declared_not_used_mask & bit) == 0U) {
      continue;
    }

    emitted = 1U;
    (void)kdi_cap_usage_window_text(trace,
                                    (KdiRequestType)req,
                                    usage_window,
                                    sizeof(usage_window));

    if (json_mode == 0U) {
      snprintf(buf, sizeof(buf),
               "%s drv=%s remove=%s review=policy.%s:1->0 window=obs[%lu,%lu] usage=%s risk=%s note=\"%s\"\r\n",
               prefix,
               kdi_driver_name(driver),
               cap_name,
               policy_field,
               (unsigned long)trace->observation_start_ms,
               (unsigned long)trace->observation_end_ms,
               usage_window,
               risk_name,
               risk_note);
    } else {
      snprintf(buf, sizeof(buf),
               "{\"type\":\"kdi-cap-review\",\"driver\":\"%s\",\"remove\":\"%s\",\"review\":\"policy.%s:1->0\",\"obs_start_ms\":%lu,\"obs_end_ms\":%lu,\"obs_window_ms\":%lu,\"usage_window\":\"%s\",\"risk\":\"%s\",\"risk_note\":\"%s\"}\r\n",
               kdi_driver_name(driver),
               cap_name,
               policy_field,
               (unsigned long)trace->observation_start_ms,
               (unsigned long)trace->observation_end_ms,
               (unsigned long)trace->observation_window_ms,
               usage_window,
               risk_name,
               risk_note);
    }
    log_send_blocking(buf);
  }

  if (emitted == 0U) {
    if (json_mode == 0U) {
      snprintf(buf, sizeof(buf),
               "%s drv=%s remove=none window=obs[%lu,%lu] risk=none note=\"no declared-unused capability candidates\"\r\n",
               prefix,
               kdi_driver_name(driver),
               (unsigned long)trace->observation_start_ms,
               (unsigned long)trace->observation_end_ms);
    } else {
      snprintf(buf, sizeof(buf),
               "{\"type\":\"kdi-cap-review\",\"driver\":\"%s\",\"remove\":\"none\",\"obs_start_ms\":%lu,\"obs_end_ms\":%lu,\"obs_window_ms\":%lu,\"risk\":\"none\",\"risk_note\":\"no declared-unused capability candidates\"}\r\n",
               kdi_driver_name(driver),
               (unsigned long)trace->observation_start_ms,
               (unsigned long)trace->observation_end_ms,
               (unsigned long)trace->observation_window_ms);
    }
    log_send_blocking(buf);
  }
}

static void kdi_log_cap_review_state(uint8_t json_mode, uint8_t filter_valid, KdiDriverId filter_driver)
{
  for (uint32_t i = 0U; i < (uint32_t)KDI_DRIVER_COUNT; ++i) {
    KdiDriverId driver = (KdiDriverId)i;
    KdiCapUsageTrace trace = {0};
    KdiDriverState state = KDI_STATE_INIT;

    if (filter_valid != 0U && driver != filter_driver) {
      continue;
    }
    if (kdi_cap_usage_get(driver, &trace) != KDI_OK) {
      continue;
    }
    if (kdi_driver_get_state(driver, &state) != KDI_OK) {
      state = KDI_STATE_INIT;
    }
    kdi_log_cap_review_driver(driver, &trace, state, json_mode, "kdi cap review");
  }
}

static void kdi_log_fault_domain_one(KdiDriverId driver)
{
  char buf[220];
  KdiFaultDomainStats d = {0};
  int rc = kdi_fault_domain_get(driver, &d);

  snprintf(buf, sizeof(buf),
           "kdi fault drv=%s iso=%u active=%u code=0x%04X detail=0x%08lX faults=%lu contain=%lu crash=%lu restart=%lu gen=%lu t_ms=%lu rc=%s\r\n",
           kdi_driver_name(driver),
           (unsigned)d.isolated,
           (unsigned)d.active_fault,
           (unsigned)d.last_code,
           (unsigned long)d.last_detail,
           (unsigned long)d.fault_total,
           (unsigned long)d.contain_total,
           (unsigned long)d.crash_total,
           (unsigned long)d.restart_total,
           (unsigned long)d.generation,
           (unsigned long)d.last_fault_ms,
           kdi_result_name(rc));
  log_send_blocking(buf);
}

static void kdi_log_fault_state(void)
{
  KdiDriverId driver = KDI_DRIVER_KERNEL;
  KdiFaultReport report = {0};

  for (uint32_t i = 0U; i < (uint32_t)KDI_DRIVER_COUNT; ++i) {
    kdi_log_fault_domain_one((KdiDriverId)i);
  }

  if (kdi_last_fault(&driver, &report) != KDI_OK) {
    log_send_blocking("kdi fault last: none\r\n");
    return;
  }

  {
    char buf[128];
    snprintf(buf, sizeof(buf),
             "kdi fault last drv=%s code=0x%04X detail=0x%08lX\r\n",
             kdi_driver_name(driver),
             (unsigned)report.code,
             (unsigned long)report.detail);
    log_send_blocking(buf);
  }
}

static void kdi_log_driver_state(void)
{
  char buf[136];

  for (uint32_t i = 0U; i < (uint32_t)KDI_DRIVER_COUNT; ++i) {
    KdiDriverState st = KDI_STATE_INIT;
    uint8_t active = 0U;
    int st_rc = kdi_driver_get_state((KdiDriverId)i, &st);
    int tok_rc = kdi_token_is_active((KdiDriverId)i, &active);
    snprintf(buf, sizeof(buf),
             "kdi state drv=%s st=%s st_rc=%s tok_active=%u tok_rc=%s\r\n",
             kdi_driver_name((KdiDriverId)i),
             kdi_driver_state_name(st),
             kdi_result_name(st_rc),
             (unsigned)active,
             kdi_result_name(tok_rc));
    log_send_blocking(buf);
  }
}

static void kdi_log_irq_state(void)
{
  char buf[196];
  KdiIrqStats stats = {0};

  kdi_irq_get_stats(&stats);
  snprintf(buf, sizeof(buf),
           "kdi irq stats enter=%lu throttle=%lu cool=%lu recov=%lu defer=%lu drop=%lu worker=%lu werr=%lu starve=%lu unsafe=%lu pending=%lu starve_ms=%lu\r\n",
           (unsigned long)stats.irq_enter_total,
           (unsigned long)stats.irq_throttle_total,
           (unsigned long)stats.irq_cooldown_total,
           (unsigned long)stats.irq_recover_total,
           (unsigned long)stats.irq_defer_total,
           (unsigned long)stats.irq_drop_total,
           (unsigned long)stats.irq_worker_total,
           (unsigned long)stats.irq_worker_error_total,
           (unsigned long)stats.irq_starvation_total,
           (unsigned long)stats.irq_unsafe_total,
           (unsigned long)stats.deferred_pending,
           (unsigned long)stats.starvation_ms);
  log_send_blocking(buf);

  for (uint32_t i = 0U; i < (uint32_t)KDI_DRIVER_COUNT; ++i) {
    KdiIrqDriverStats s = {0};
    int rc = kdi_irq_get_driver_stats((KdiDriverId)i, &s);
    snprintf(buf, sizeof(buf),
             "kdi irq drv=%s in_irq=%u thr=%u hook=%u lvl=%u budget=%lu window=%lu cool=[%lu..%lu] until=%lu pending=%lu rc=%s\r\n",
             kdi_driver_name((KdiDriverId)i),
             (unsigned)s.in_irq,
             (unsigned)s.throttled,
             (unsigned)s.worker_registered,
             (unsigned)s.cooldown_level,
             (unsigned long)s.budget_per_sec,
             (unsigned long)s.window_count,
             (unsigned long)s.cooldown_base_ms,
             (unsigned long)s.cooldown_max_ms,
             (unsigned long)s.cooldown_until_ms,
             (unsigned long)s.deferred_pending,
             kdi_result_name(rc));
    log_send_blocking(buf);
  }
}

static int kdi_parse_irq_unsafe_op(const char *name, KdiIrqUnsafeOp *out)
{
  if (name == NULL || out == NULL) {
    return 0;
  }
  if (strcmp(name, "malloc") == 0) {
    *out = KDI_IRQ_UNSAFE_MALLOC;
    return 1;
  }
  if (strcmp(name, "printf") == 0) {
    *out = KDI_IRQ_UNSAFE_PRINTF;
    return 1;
  }
  if (strcmp(name, "policy") == 0) {
    *out = KDI_IRQ_UNSAFE_POLICY;
    return 1;
  }
  return 0;
}

static void kdi_log_token_state(void)
{
  char buf[128];

  for (uint32_t i = 0U; i < (uint32_t)KDI_DRIVER_COUNT; ++i) {
    uint8_t active = 0U;
    uint32_t ttl_ms = 0U;
    uint32_t remain_ms = 0U;
    int rc = kdi_token_is_active((KdiDriverId)i, &active);
    int ttl_rc = kdi_get_token_ttl_ms((KdiDriverId)i, &ttl_ms);
    int rem_rc = kdi_token_remaining_ms((KdiDriverId)i, &remain_ms);

    if (rem_rc != KDI_OK) {
      remain_ms = 0U;
    }
    snprintf(buf, sizeof(buf),
             "kdi token drv=%s active=%u ttl=%lu rem=%lu rc=%s ttl_rc=%s rem_rc=%s\r\n",
             kdi_driver_name((KdiDriverId)i),
             (unsigned)active,
             (unsigned long)ttl_ms,
             (unsigned long)remain_ms,
             kdi_result_name(rc),
             kdi_result_name(ttl_rc),
             kdi_result_name(rem_rc));
    log_send_blocking(buf);
  }
}

static void kdi_handle_token_cmd(char *args)
{
  char *p = args;
  char *action = next_token(&p);
  char *name;
  char buf[120];
  KdiDriverId driver;
  KdiCapToken token = KDI_CAP_INVALID;
  KdiCapToken *slot;
  int rc;

  if (action == NULL || strcmp(action, "show") == 0) {
    kdi_log_token_state();
    return;
  }
  if (strcmp(action, "rotate") != 0 &&
      strcmp(action, "revoke") != 0 &&
      strcmp(action, "ttl") != 0) {
    kdi_log_token_usage();
    return;
  }

  name = next_token(&p);
  if (!kdi_parse_driver(name, &driver)) {
    kdi_log_token_usage();
    return;
  }

  if (strcmp(action, "rotate") == 0) {
    rc = kdi_rotate_token(driver, &token);
    if (rc == KDI_OK) {
      slot = kdi_cap_slot(driver);
      if (slot != NULL) {
        *slot = token;
      }
    }
    snprintf(buf, sizeof(buf), "kdi token rotate %s: %s\r\n",
             kdi_driver_name(driver), kdi_result_name(rc));
    log_send_blocking(buf);
    return;
  }

  if (strcmp(action, "ttl") == 0) {
    uint32_t ttl_ms = 0U;
    char *ttl_text = next_token(&p);
    if (!parse_u32(ttl_text, &ttl_ms)) {
      kdi_log_token_usage();
      return;
    }
    rc = kdi_set_token_ttl_ms(driver, ttl_ms);
    snprintf(buf, sizeof(buf), "kdi token ttl %s=%lu: %s\r\n",
             kdi_driver_name(driver),
             (unsigned long)ttl_ms,
             kdi_result_name(rc));
    log_send_blocking(buf);
    return;
  }

  rc = kdi_revoke_token(driver);
  if (rc == KDI_OK) {
    slot = kdi_cap_slot(driver);
    if (slot != NULL) {
      *slot = KDI_CAP_INVALID;
    }
  }
  snprintf(buf, sizeof(buf), "kdi token revoke %s: %s\r\n",
           kdi_driver_name(driver), kdi_result_name(rc));
  log_send_blocking(buf);
}

static void kdi_handle_driver_cmd(char *args)
{
  char *p = args;
  char *action = next_token(&p);
  char *name;
  char buf[120];
  KdiDriverId driver;
  KdiCapToken token = KDI_CAP_INVALID;
  KdiCapToken *slot;
  int rc;

  if (action == NULL || strcmp(action, "show") == 0) {
    kdi_log_driver_state();
    return;
  }

  name = next_token(&p);
  if (!kdi_parse_driver(name, &driver)) {
    kdi_log_driver_usage();
    return;
  }
  slot = kdi_cap_slot(driver);
  if (slot != NULL) {
    token = *slot;
  }

  if (strcmp(action, "probe") == 0) {
    rc = kdi_driver_probe(driver, token);
  } else if (strcmp(action, "ready") == 0) {
    rc = kdi_driver_probe_done(driver, token, 1U);
  } else if (strcmp(action, "fail") == 0) {
    rc = kdi_driver_probe_done(driver, token, 0U);
  } else if (strcmp(action, "active") == 0) {
    rc = kdi_driver_activate(driver, token);
  } else if (strcmp(action, "error") == 0) {
    uint32_t code = 0U;
    char *code_text = next_token(&p);
    if (!parse_u32(code_text, &code) || code == 0U || code > 0xFFFFU) {
      kdi_log_driver_usage();
      return;
    }
    rc = kdi_driver_runtime_error(driver, token, (uint16_t)code);
  } else if (strcmp(action, "reset") == 0) {
    rc = kdi_driver_reset(driver, kdi_caps.kernel);
  } else if (strcmp(action, "reinit") == 0) {
    KdiCapToken new_token = KDI_CAP_INVALID;
    rc = kdi_driver_reinit(driver, kdi_caps.kernel, &new_token);
    if (rc == KDI_OK && slot != NULL) {
      *slot = new_token;
    }
  } else if (strcmp(action, "reclaim") == 0) {
    rc = kdi_driver_force_reclaim(driver, kdi_caps.kernel);
    if (rc == KDI_OK && slot != NULL) {
      *slot = KDI_CAP_INVALID;
    }
  } else {
    kdi_log_driver_usage();
    return;
  }

  snprintf(buf, sizeof(buf), "kdi driver %s %s: %s\r\n",
           action, kdi_driver_name(driver), kdi_result_name(rc));
  log_send_blocking(buf);
}

static void kdi_handle_irq_cmd(char *args)
{
  char *p = args;
  char *action = next_token(&p);
  char buf[136];
  KdiDriverId driver = KDI_DRIVER_KERNEL;
  KdiCapToken token = KDI_CAP_INVALID;
  KdiCapToken *slot = NULL;
  int rc = KDI_ERR_BAD_ARG;

  if (action == NULL || strcmp(action, "show") == 0) {
    kdi_log_irq_state();
    return;
  }

  if (strcmp(action, "worker") == 0) {
    uint32_t n = 8U;
    uint32_t processed = 0U;
    char *txt = next_token(&p);
    if (txt != NULL && !parse_u32(txt, &n)) {
      kdi_log_irq_usage();
      return;
    }
    rc = kdi_irq_worker_run(n, &processed);
    snprintf(buf, sizeof(buf), "kdi irq worker n=%lu processed=%lu rc=%s\r\n",
             (unsigned long)n,
             (unsigned long)processed,
             kdi_result_name(rc));
    log_send_blocking(buf);
    return;
  }

  if (strcmp(action, "poll") == 0) {
    uint32_t detected = 0U;
    rc = kdi_irq_poll_starvation(&detected);
    snprintf(buf, sizeof(buf), "kdi irq poll detected=%lu rc=%s\r\n",
             (unsigned long)detected,
             kdi_result_name(rc));
    log_send_blocking(buf);
    return;
  }

  if (strcmp(action, "starve") == 0) {
    uint32_t ms = 0U;
    char *txt = next_token(&p);
    if (!parse_u32(txt, &ms)) {
      kdi_log_irq_usage();
      return;
    }
    rc = kdi_irq_set_starvation_ms(ms);
    snprintf(buf, sizeof(buf), "kdi irq starve=%lu rc=%s\r\n",
             (unsigned long)ms,
             kdi_result_name(rc));
    log_send_blocking(buf);
    return;
  }

  {
    char *name = next_token(&p);
    if (!kdi_parse_driver(name, &driver)) {
      kdi_log_irq_usage();
      return;
    }
  }
  slot = kdi_cap_slot(driver);
  if (slot != NULL) {
    token = *slot;
  }

  if (strcmp(action, "budget") == 0) {
    uint32_t per_sec = 0U;
    char *txt = next_token(&p);
    if (!parse_u32(txt, &per_sec)) {
      kdi_log_irq_usage();
      return;
    }
    rc = kdi_irq_set_budget_per_sec(driver, per_sec);
    snprintf(buf, sizeof(buf), "kdi irq budget %s=%lu rc=%s\r\n",
             kdi_driver_name(driver),
             (unsigned long)per_sec,
             kdi_result_name(rc));
    log_send_blocking(buf);
    return;
  }

  if (strcmp(action, "cooldown") == 0) {
    uint32_t base_ms = 0U;
    uint32_t max_ms = 0U;
    char *base_txt = next_token(&p);
    char *max_txt = next_token(&p);
    if (!parse_u32(base_txt, &base_ms)) {
      kdi_log_irq_usage();
      return;
    }
    if (max_txt != NULL) {
      if (!parse_u32(max_txt, &max_ms)) {
        kdi_log_irq_usage();
        return;
      }
    } else {
      max_ms = base_ms;
    }
    rc = kdi_irq_set_cooldown_ms(driver, base_ms, max_ms);
    snprintf(buf, sizeof(buf), "kdi irq cooldown %s base=%lu max=%lu rc=%s\r\n",
             kdi_driver_name(driver),
             (unsigned long)base_ms,
             (unsigned long)max_ms,
             kdi_result_name(rc));
    log_send_blocking(buf);
    return;
  }

  if (strcmp(action, "enter") == 0) {
    rc = kdi_irq_enter(driver, token);
    snprintf(buf, sizeof(buf), "kdi irq enter %s: %s\r\n",
             kdi_driver_name(driver),
             kdi_result_name(rc));
    log_send_blocking(buf);
    return;
  }

  if (strcmp(action, "defer") == 0) {
    uint32_t work = 1U;
    uint32_t arg = 0U;
    char *work_txt = next_token(&p);
    char *arg_txt = next_token(&p);
    if (work_txt != NULL && !parse_u32(work_txt, &work)) {
      kdi_log_irq_usage();
      return;
    }
    if (arg_txt != NULL && !parse_u32(arg_txt, &arg)) {
      kdi_log_irq_usage();
      return;
    }
    rc = kdi_irq_defer(driver, token, (uint16_t)(work & 0xFFFFU), arg);
    snprintf(buf, sizeof(buf), "kdi irq defer %s work=0x%04lX arg=0x%08lX rc=%s\r\n",
             kdi_driver_name(driver),
             (unsigned long)(work & 0xFFFFU),
             (unsigned long)arg,
             kdi_result_name(rc));
    log_send_blocking(buf);
    return;
  }

  if (strcmp(action, "exit") == 0) {
    rc = kdi_irq_exit(driver, token);
    snprintf(buf, sizeof(buf), "kdi irq exit %s: %s\r\n",
             kdi_driver_name(driver),
             kdi_result_name(rc));
    log_send_blocking(buf);
    return;
  }

  if (strcmp(action, "unsafe") == 0) {
    KdiIrqUnsafeOp op = KDI_IRQ_UNSAFE_MALLOC;
    char *op_txt = next_token(&p);
    if (!kdi_parse_irq_unsafe_op(op_txt, &op)) {
      kdi_log_irq_usage();
      return;
    }
    rc = kdi_irq_unsafe_op(driver, token, op);
    snprintf(buf, sizeof(buf), "kdi irq unsafe %s op=%s rc=%s\r\n",
             kdi_driver_name(driver),
             op_txt,
             kdi_result_name(rc));
    log_send_blocking(buf);
    return;
  }

  if (strcmp(action, "storm") == 0) {
    uint32_t n = 0U;
    uint32_t ok = 0U;
    uint32_t fail = 0U;
    char *txt = next_token(&p);
    if (!parse_u32(txt, &n) || n == 0U) {
      kdi_log_irq_usage();
      return;
    }
    for (uint32_t i = 0U; i < n; ++i) {
      rc = kdi_irq_enter(driver, token);
      if (rc != KDI_OK) {
        fail++;
        continue;
      }
      (void)kdi_irq_defer(driver, token, (uint16_t)(0x100U + i), i);
      (void)kdi_irq_exit(driver, token);
      ok++;
    }
    snprintf(buf, sizeof(buf), "kdi irq storm %s n=%lu ok=%lu fail=%lu last=%s\r\n",
             kdi_driver_name(driver),
             (unsigned long)n,
             (unsigned long)ok,
             (unsigned long)fail,
             kdi_result_name(rc));
    log_send_blocking(buf);
    return;
  }

  kdi_log_irq_usage();
}

static void kdi_handle_fault_cmd(char *args)
{
  char *p = args;
  char *action = next_token(&p);

  if (action == NULL || strcmp(action, "show") == 0) {
    kdi_log_fault_state();
    return;
  }

  if (strcmp(action, "domain") == 0) {
    char *name = next_token(&p);
    KdiDriverId driver = KDI_DRIVER_KERNEL;
    if (!kdi_parse_driver(name, &driver)) {
      kdi_log_fault_usage();
      return;
    }
    kdi_log_fault_domain_one(driver);
    return;
  }

  kdi_log_fault_usage();
}

static void kdi_handle_cap_cmd(char *args)
{
  char *p = args;
  char *action = next_token(&p);
  char *name = next_token(&p);
  KdiDriverId driver = KDI_DRIVER_KERNEL;
  uint8_t filter_valid = 0U;

  if (name != NULL) {
    if (!kdi_parse_driver(name, &driver)) {
      kdi_log_cap_usage();
      return;
    }
    filter_valid = 1U;
  }

  if (action == NULL || strcmp(action, "show") == 0) {
    kdi_log_cap_state(0U, filter_valid, driver);
    return;
  }
  if (strcmp(action, "json") == 0) {
    kdi_log_cap_state(1U, filter_valid, driver);
    return;
  }
  if (strcmp(action, "review") == 0) {
    kdi_log_cap_review_state(0U, filter_valid, driver);
    return;
  }
  if (strcmp(action, "review-json") == 0) {
    kdi_log_cap_review_state(1U, filter_valid, driver);
    return;
  }
  kdi_log_cap_usage();
}

static void kdi_handle_cmd(char *args)
{
  char *p = args;
  char *cmd = next_token(&p);

  if (cmd == NULL || strcmp(cmd, "show") == 0) {
    kdi_log_show();
    return;
  }
  if (strcmp(cmd, "last") == 0) {
    kdi_log_last();
    return;
  }
  if (strcmp(cmd, "probe") == 0) {
    char *mode = next_token(&p);
    if (mode == NULL) {
      kdi_log_usage();
      return;
    }
    if (strcmp(mode, "allow") == 0) {
      kdi_probe_allow();
      return;
    }
    if (strcmp(mode, "deny") == 0) {
      kdi_probe_deny();
      return;
    }
    if (strcmp(mode, "authfail") == 0) {
      kdi_probe_authfail();
      return;
    }
    kdi_log_usage();
    return;
  }
  if (strcmp(cmd, "token") == 0) {
    kdi_handle_token_cmd(p);
    return;
  }
  if (strcmp(cmd, "driver") == 0) {
    kdi_handle_driver_cmd(p);
    return;
  }
  if (strcmp(cmd, "irq") == 0) {
    kdi_handle_irq_cmd(p);
    return;
  }
  if (strcmp(cmd, "fault") == 0) {
    kdi_handle_fault_cmd(p);
    return;
  }
  if (strcmp(cmd, "cap") == 0) {
    kdi_handle_cap_cmd(p);
    return;
  }
  kdi_log_usage();
}

static void dep_log_usage(void)
{
  log_send_blocking("dep show|json|impact|impact-json <kernel|uart|sensor|vm|diag>\r\n");
  log_send_blocking("dep whatif|whatif-json <reset|throttle|deny> <kernel|uart|sensor|vm|diag>\r\n");
}

static void dep_log_show(uint8_t json_mode)
{
  char buf[220];
  uint32_t driver_edge_count = 0U;
  uint32_t resource_edge_count = 0U;
  uint32_t stage_edge_count = 0U;
  const DepDriverEdge *driver_edges = dependency_graph_driver_edges(&driver_edge_count);
  const DepResourceEdge *resource_edges = dependency_graph_resource_edges(&resource_edge_count);
  const DepStageDriverEdge *stage_edges = dependency_graph_stage_driver_edges(&stage_edge_count);

  if (json_mode == 0U) {
    snprintf(buf, sizeof(buf),
             "dep summary driver_edges=%lu resource_edges=%lu stage_edges=%lu\r\n",
             (unsigned long)driver_edge_count,
             (unsigned long)resource_edge_count,
             (unsigned long)stage_edge_count);
    log_send_blocking(buf);
  } else {
    snprintf(buf, sizeof(buf),
             "{\"type\":\"dep-summary\",\"driver_edges\":%lu,\"resource_edges\":%lu,\"stage_edges\":%lu}\r\n",
             (unsigned long)driver_edge_count,
             (unsigned long)resource_edge_count,
             (unsigned long)stage_edge_count);
    log_send_blocking(buf);
  }

  for (uint32_t i = 0U; i < driver_edge_count; ++i) {
    const DepDriverEdge *e = &driver_edges[i];
    if (json_mode == 0U) {
      snprintf(buf, sizeof(buf),
               "dep driver %s -> %s reason=%s\r\n",
               kdi_driver_name(e->from),
               kdi_driver_name(e->to),
               e->reason);
    } else {
      snprintf(buf, sizeof(buf),
               "{\"type\":\"dep-driver\",\"from\":\"%s\",\"to\":\"%s\",\"reason\":\"%s\"}\r\n",
               kdi_driver_name(e->from),
               kdi_driver_name(e->to),
               e->reason);
    }
    log_send_blocking(buf);
  }

  for (uint32_t i = 0U; i < resource_edge_count; ++i) {
    const DepResourceEdge *e = &resource_edges[i];
    if (json_mode == 0U) {
      snprintf(buf, sizeof(buf),
               "dep resource %s -> %s:%s reason=%s\r\n",
               kdi_driver_name(e->driver),
               dependency_graph_resource_kind_name(e->kind),
               e->resource_id,
               e->reason);
    } else {
      snprintf(buf, sizeof(buf),
               "{\"type\":\"dep-resource\",\"driver\":\"%s\",\"kind\":\"%s\",\"resource\":\"%s\",\"reason\":\"%s\"}\r\n",
               kdi_driver_name(e->driver),
               dependency_graph_resource_kind_name(e->kind),
               e->resource_id,
               e->reason);
    }
    log_send_blocking(buf);
  }

  for (uint32_t i = 0U; i < stage_edge_count; ++i) {
    const DepStageDriverEdge *e = &stage_edges[i];
    BringupStageId stage = bringup_stage_from_phase(e->stage);
    if (json_mode == 0U) {
      snprintf(buf, sizeof(buf),
               "dep stage %s -> %s phase=%s reason=%s\r\n",
               bringup_stage_name(stage),
               kdi_driver_name(e->driver),
               bringup_phase_name(e->stage),
               e->reason);
    } else {
      snprintf(buf, sizeof(buf),
               "{\"type\":\"dep-stage-driver\",\"stage\":\"%s\",\"phase\":\"%s\",\"driver\":\"%s\",\"reason\":\"%s\"}\r\n",
               bringup_stage_name(stage),
               bringup_phase_name(e->stage),
               kdi_driver_name(e->driver),
               e->reason);
    }
    log_send_blocking(buf);
  }
}

static void dep_log_impact(KdiDriverId target, uint8_t json_mode)
{
  char buf[172];
  KdiDriverId impacted[KDI_DRIVER_COUNT];
  uint32_t affected_count =
    dependency_graph_reset_impact(target, impacted, (uint32_t)KDI_DRIVER_COUNT);

  if (json_mode == 0U) {
    snprintf(buf, sizeof(buf),
             "dep impact reset=%s affects=%lu\r\n",
             kdi_driver_name(target),
             (unsigned long)affected_count);
    log_send_blocking(buf);
  } else {
    snprintf(buf, sizeof(buf),
             "{\"type\":\"dep-impact-summary\",\"reset\":\"%s\",\"affected\":%lu}\r\n",
             kdi_driver_name(target),
             (unsigned long)affected_count);
    log_send_blocking(buf);
  }

  if (affected_count == 0U) {
    if (json_mode == 0U) {
      log_send_blocking("dep impact affected=none\r\n");
    } else {
      log_send_blocking("{\"type\":\"dep-impact\",\"driver\":\"none\"}\r\n");
    }
    return;
  }

  for (uint32_t i = 0U; i < affected_count; ++i) {
    if (json_mode == 0U) {
      snprintf(buf, sizeof(buf),
               "dep impact %s -> %s\r\n",
               kdi_driver_name(target),
               kdi_driver_name(impacted[i]));
    } else {
      snprintf(buf, sizeof(buf),
               "{\"type\":\"dep-impact\",\"reset\":\"%s\",\"driver\":\"%s\"}\r\n",
               kdi_driver_name(target),
               kdi_driver_name(impacted[i]));
    }
    log_send_blocking(buf);
  }
}

static const char *dep_driver_edge_reason(KdiDriverId from, KdiDriverId to)
{
  uint32_t edge_count = 0U;
  const DepDriverEdge *edges = dependency_graph_driver_edges(&edge_count);

  for (uint32_t i = 0U; i < edge_count; ++i) {
    if (edges[i].from == from && edges[i].to == to) {
      return edges[i].reason;
    }
  }
  return "transitive dependency";
}

static const char *dep_action_depth_effect(DepHypotheticalAction action, uint8_t depth)
{
  if (depth == 0U) {
    switch (action) {
      case DEP_ACTION_RESET:
        return "target reset outage";
      case DEP_ACTION_THROTTLE:
        return "target throughput/latency degradation";
      case DEP_ACTION_DENY:
        return "target permission denial";
      default:
        return "target action";
    }
  }

  switch (action) {
    case DEP_ACTION_RESET:
      return "upstream service loss propagation";
    case DEP_ACTION_THROTTLE:
      return "upstream backpressure propagation";
    case DEP_ACTION_DENY:
      return "upstream capability block propagation";
    default:
      return "dependency propagation";
  }
}

static void dep_log_whatif(DepHypotheticalAction action, KdiDriverId target, uint8_t json_mode)
{
  char buf[280];
  KdiDriverId impacted[KDI_DRIVER_COUNT];
  uint8_t depth[KDI_DRIVER_COUNT] = {0};
  KdiDriverId via[KDI_DRIVER_COUNT];
  int index_by_driver[KDI_DRIVER_COUNT];
  BringupStageId stage_list[BRINGUP_STAGE_COUNT];
  KdiDriverId stage_driver[BRINGUP_STAGE_COUNT];
  BringupPhaseId stage_phase[BRINGUP_STAGE_COUNT];
  const char *stage_reason[BRINGUP_STAGE_COUNT];
  uint8_t stage_seen[BRINGUP_STAGE_COUNT] = {0};
  uint32_t stage_count = 0U;
  uint32_t impacted_count;

  for (uint32_t i = 0U; i < (uint32_t)KDI_DRIVER_COUNT; ++i) {
    via[i] = KDI_DRIVER_COUNT;
    index_by_driver[i] = -1;
  }

  impacted_count = dependency_graph_action_impact(action,
                                                  target,
                                                  impacted,
                                                  depth,
                                                  via,
                                                  (uint32_t)KDI_DRIVER_COUNT);
  if (impacted_count > (uint32_t)KDI_DRIVER_COUNT) {
    impacted_count = (uint32_t)KDI_DRIVER_COUNT;
  }
  for (uint32_t i = 0U; i < impacted_count; ++i) {
    if ((uint32_t)impacted[i] < (uint32_t)KDI_DRIVER_COUNT) {
      index_by_driver[(uint32_t)impacted[i]] = (int)i;
    }
  }

  for (uint32_t i = 0U; i < impacted_count; ++i) {
    BringupStageId stages[BRINGUP_STAGE_COUNT];
    BringupPhaseId phases[BRINGUP_STAGE_COUNT];
    const char *reasons[BRINGUP_STAGE_COUNT];
    uint32_t driver_stage_count =
      dependency_graph_driver_stages(impacted[i],
                                     stages,
                                     phases,
                                     reasons,
                                     (uint32_t)BRINGUP_STAGE_COUNT);
    if (driver_stage_count > (uint32_t)BRINGUP_STAGE_COUNT) {
      driver_stage_count = (uint32_t)BRINGUP_STAGE_COUNT;
    }
    for (uint32_t j = 0U; j < driver_stage_count; ++j) {
      BringupStageId stage = stages[j];
      if ((uint32_t)stage >= (uint32_t)BRINGUP_STAGE_COUNT) {
        continue;
      }
      if (stage_seen[(uint32_t)stage] != 0U) {
        continue;
      }
      stage_seen[(uint32_t)stage] = 1U;
      if (stage_count < (uint32_t)BRINGUP_STAGE_COUNT) {
        stage_list[stage_count] = stage;
        stage_driver[stage_count] = impacted[i];
        stage_phase[stage_count] = phases[j];
        stage_reason[stage_count] = reasons[j];
      }
      stage_count++;
    }
  }
  if (stage_count > (uint32_t)BRINGUP_STAGE_COUNT) {
    stage_count = (uint32_t)BRINGUP_STAGE_COUNT;
  }

  if (json_mode == 0U) {
    snprintf(buf, sizeof(buf),
             "dep whatif action=%s target=%s static_projection=1 affected_drivers=%lu affected_stages=%lu\r\n",
             dependency_graph_action_name(action),
             kdi_driver_name(target),
             (unsigned long)impacted_count,
             (unsigned long)stage_count);
    log_send_blocking(buf);
  } else {
    snprintf(buf, sizeof(buf),
             "{\"type\":\"dep-whatif-summary\",\"action\":\"%s\",\"target\":\"%s\",\"static_projection\":1,\"affected_drivers\":%lu,\"affected_stages\":%lu}\r\n",
             dependency_graph_action_name(action),
             kdi_driver_name(target),
             (unsigned long)impacted_count,
             (unsigned long)stage_count);
    log_send_blocking(buf);
  }

  for (uint32_t i = 0U; i < impacted_count; ++i) {
    const char *effect = dep_action_depth_effect(action, depth[i]);
    const char *reason = "action target";
    const char *via_name = "none";

    if (depth[i] > 0U && (uint32_t)via[i] < (uint32_t)KDI_DRIVER_COUNT) {
      reason = dep_driver_edge_reason(impacted[i], via[i]);
      via_name = kdi_driver_name(via[i]);
    }

    if (json_mode == 0U) {
      snprintf(buf, sizeof(buf),
               "dep whatif driver=%s depth=%u via=%s effect=%s reason=%s\r\n",
               kdi_driver_name(impacted[i]),
               (unsigned)depth[i],
               via_name,
               effect,
               reason);
    } else {
      snprintf(buf, sizeof(buf),
               "{\"type\":\"dep-whatif-driver\",\"action\":\"%s\",\"target\":\"%s\",\"driver\":\"%s\",\"depth\":%u,\"via\":\"%s\",\"effect\":\"%s\",\"reason\":\"%s\"}\r\n",
               dependency_graph_action_name(action),
               kdi_driver_name(target),
               kdi_driver_name(impacted[i]),
               (unsigned)depth[i],
               via_name,
               effect,
               reason);
    }
    log_send_blocking(buf);
  }

  if (stage_count == 0U) {
    if (json_mode == 0U) {
      log_send_blocking("dep whatif stage=none\r\n");
    } else {
      log_send_blocking("{\"type\":\"dep-whatif-stage\",\"stage\":\"none\"}\r\n");
    }
  } else {
    for (uint32_t i = 0U; i < stage_count; ++i) {
      if (json_mode == 0U) {
        snprintf(buf, sizeof(buf),
                 "dep whatif stage=%s phase=%s via_driver=%s reason=%s\r\n",
                 bringup_stage_name(stage_list[i]),
                 bringup_phase_name(stage_phase[i]),
                 kdi_driver_name(stage_driver[i]),
                 stage_reason[i]);
      } else {
        snprintf(buf, sizeof(buf),
                 "{\"type\":\"dep-whatif-stage\",\"stage\":\"%s\",\"phase\":\"%s\",\"driver\":\"%s\",\"reason\":\"%s\"}\r\n",
                 bringup_stage_name(stage_list[i]),
                 bringup_phase_name(stage_phase[i]),
                 kdi_driver_name(stage_driver[i]),
                 stage_reason[i]);
      }
      log_send_blocking(buf);
    }
  }

  for (uint32_t i = 0U; i < impacted_count; ++i) {
    KdiDriverId middle;
    KdiDriverId upstream;
    const char *r1;
    const char *r2;

    if (depth[i] < 2U) {
      continue;
    }
    middle = via[i];
    if ((uint32_t)middle >= (uint32_t)KDI_DRIVER_COUNT) {
      continue;
    }
    if (index_by_driver[(uint32_t)middle] < 0) {
      continue;
    }
    upstream = via[(uint32_t)index_by_driver[(uint32_t)middle]];
    if ((uint32_t)upstream >= (uint32_t)KDI_DRIVER_COUNT) {
      upstream = target;
    }
    r1 = dep_driver_edge_reason(impacted[i], middle);
    r2 = dep_driver_edge_reason(middle, upstream);

    if (json_mode == 0U) {
      snprintf(buf, sizeof(buf),
               "dep whatif secondary driver=%s chain=%s->%s->%s why=%s + %s\r\n",
               kdi_driver_name(impacted[i]),
               kdi_driver_name(impacted[i]),
               kdi_driver_name(middle),
               kdi_driver_name(upstream),
               r1,
               r2);
    } else {
      snprintf(buf, sizeof(buf),
               "{\"type\":\"dep-whatif-secondary\",\"driver\":\"%s\",\"middle\":\"%s\",\"upstream\":\"%s\",\"reason_a\":\"%s\",\"reason_b\":\"%s\"}\r\n",
               kdi_driver_name(impacted[i]),
               kdi_driver_name(middle),
               kdi_driver_name(upstream),
               r1,
               r2);
    }
    log_send_blocking(buf);
  }
}

static void dep_handle_cmd(char *args)
{
  char *p = args;
  char *cmd = next_token(&p);
  char *action_name;
  char *name;
  DepHypotheticalAction action = DEP_ACTION_RESET;
  KdiDriverId driver;

  if (cmd == NULL || strcmp(cmd, "show") == 0) {
    dep_log_show(0U);
    return;
  }
  if (strcmp(cmd, "json") == 0) {
    dep_log_show(1U);
    return;
  }
  if (strcmp(cmd, "impact") == 0 || strcmp(cmd, "impact-json") == 0) {
    uint8_t json_mode = (strcmp(cmd, "impact-json") == 0) ? 1U : 0U;
    name = next_token(&p);
    if (!kdi_parse_driver(name, &driver)) {
      dep_log_usage();
      return;
    }
    dep_log_impact(driver, json_mode);
    return;
  }
  if (strcmp(cmd, "whatif") == 0 || strcmp(cmd, "whatif-json") == 0) {
    uint8_t json_mode = (strcmp(cmd, "whatif-json") == 0) ? 1U : 0U;
    action_name = next_token(&p);
    name = next_token(&p);
    if (!dependency_graph_parse_action(action_name, &action) ||
        !kdi_parse_driver(name, &driver)) {
      dep_log_usage();
      return;
    }
    dep_log_whatif(action, driver, json_mode);
    return;
  }
  dep_log_usage();
}

static void sonic_log_usage(void)
{
  log_send_blocking("sonic cap|show|get|set|diff|history|preset|apply|commit|confirm|rollback|abort\r\n");
  log_send_blocking("sonic show [db|running|candidate|config|appl|asic]\r\n");
  log_send_blocking("sonic preset list|show|apply <name> [running|candidate]\r\n");
  log_send_blocking("sonic apply <mig|vm> [running|candidate]\r\n");
}

typedef enum {
  SONIC_VIEW_RUNNING = 0,
  SONIC_VIEW_CANDIDATE = 1
} SonicView;

static int sonic_parse_view(const char *name, SonicView *out)
{
  if (name == NULL || out == NULL) {
    return 0;
  }
  if (strcmp(name, "running") == 0) {
    *out = SONIC_VIEW_RUNNING;
    return 1;
  }
  if (strcmp(name, "candidate") == 0) {
    *out = SONIC_VIEW_CANDIDATE;
    return 1;
  }
  return 0;
}

static uint16_t sonic_count_view(SonicView view, SonicLiteDb db)
{
  if (view == SONIC_VIEW_CANDIDATE) {
    return sonic_lite_count_candidate(&sonic_lite, db);
  }
  return sonic_lite_count(&sonic_lite, db);
}

static int sonic_list_view(SonicView view,
                           SonicLiteDb db,
                           uint16_t dense_index,
                           const char **out_key,
                           const char **out_value)
{
  if (view == SONIC_VIEW_CANDIDATE) {
    return sonic_lite_list_candidate(&sonic_lite, db, dense_index, out_key, out_value);
  }
  return sonic_lite_list(&sonic_lite, db, dense_index, out_key, out_value);
}

static int sonic_get_view(SonicView view, SonicLiteDb db, const char *key, const char **out_value)
{
  if (view == SONIC_VIEW_CANDIDATE) {
    return sonic_lite_get_candidate(&sonic_lite, db, key, out_value);
  }
  return sonic_lite_get(&sonic_lite, db, key, out_value);
}

static int sonic_streq_ci(const char *a, const char *b)
{
  if (a == NULL || b == NULL) {
    return 0;
  }
  while (*a != '\0' && *b != '\0') {
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
      return 0;
    }
    a++;
    b++;
  }
  return (*a == '\0' && *b == '\0') ? 1 : 0;
}

static int sonic_parse_bool_text(const char *text, uint8_t *out)
{
  if (text == NULL || out == NULL) {
    return 0;
  }
  if (sonic_streq_ci(text, "1") ||
      sonic_streq_ci(text, "true") ||
      sonic_streq_ci(text, "yes") ||
      sonic_streq_ci(text, "on")) {
    *out = 1U;
    return 1;
  }
  if (sonic_streq_ci(text, "0") ||
      sonic_streq_ci(text, "false") ||
      sonic_streq_ci(text, "no") ||
      sonic_streq_ci(text, "off")) {
    *out = 0U;
    return 1;
  }
  return 0;
}

static int sonic_parse_log_mode_text(const char *text, uint8_t *out_mode)
{
  if (text == NULL || out_mode == NULL) {
    return 0;
  }
  if (strcmp(text, "hb") == 0 || strcmp(text, "0") == 0) {
    *out_mode = 0U;
    return 1;
  }
  if (strcmp(text, "adc") == 0 || strcmp(text, "1") == 0) {
    *out_mode = 1U;
    return 1;
  }
  if (strcmp(text, "all") == 0 || strcmp(text, "2") == 0) {
    *out_mode = 2U;
    return 1;
  }
  return 0;
}

static int sonic_apply_mig_from_view(SonicView view, char *err, size_t err_size)
{
  const char *mode_value = NULL;
  const char *allow_value = NULL;
  const char *deny_value = NULL;
  const char *reset_value = NULL;
  uint8_t has_mode;
  uint8_t has_allow;
  uint8_t has_deny;
  uint8_t has_reset;
  uint8_t reset_bool = 0U;
  uint32_t allow_mask = 0U;
  uint32_t deny_mask = 0U;
  uint8_t changed = 0U;

  if (err == NULL || err_size == 0U) {
    return 0;
  }

  has_mode = (sonic_get_view(view, SONIC_DB_CONFIG, "mig.mode", &mode_value) == 0) ? 1U : 0U;
  has_allow = (sonic_get_view(view, SONIC_DB_CONFIG, "mig.allow", &allow_value) == 0) ? 1U : 0U;
  has_deny = (sonic_get_view(view, SONIC_DB_CONFIG, "mig.deny", &deny_value) == 0) ? 1U : 0U;
  has_reset = (sonic_get_view(view, SONIC_DB_CONFIG, "mig.reset", &reset_value) == 0) ? 1U : 0U;

  if (!has_mode && !has_allow && !has_deny && !has_reset) {
    snprintf(err, err_size, "no mig.* keys in config");
    return 0;
  }

  if (has_reset) {
    if (!sonic_parse_bool_text(reset_value, &reset_bool)) {
      snprintf(err, err_size, "bad mig.reset");
      return 0;
    }
  }
  if (has_allow && !vm_mig_parse_mask_list(allow_value, &allow_mask)) {
    snprintf(err, err_size, "bad mig.allow");
    return 0;
  }
  if (has_deny && !vm_mig_parse_mask_list(deny_value, &deny_mask)) {
    snprintf(err, err_size, "bad mig.deny");
    return 0;
  }

  if (has_reset && reset_bool != 0U) {
    vm32_mig_reset(&vm32);
    changed = 1U;
  }
  if (has_allow) {
    vm32_mig_deny(&vm32, VM32_MIG_RES_ALL);
    vm32_mig_allow(&vm32, allow_mask);
    changed = 1U;
  }
  if (has_deny) {
    vm32_mig_deny(&vm32, deny_mask);
    changed = 1U;
  }
  if (has_mode) {
    if (strcmp(mode_value, "off") == 0) {
      vm32_mig_set_mode(&vm32, VM32_MIG_MODE_OFF);
    } else if (strcmp(mode_value, "monitor") == 0) {
      vm32_mig_set_mode(&vm32, VM32_MIG_MODE_MONITOR);
    } else if (strcmp(mode_value, "enforce") == 0) {
      vm32_mig_set_mode(&vm32, VM32_MIG_MODE_ENFORCE);
    } else {
      snprintf(err, err_size, "bad mig.mode");
      return 0;
    }
    changed = 1U;
  }

  if (!changed) {
    snprintf(err, err_size, "no effective changes");
    return 0;
  }

  err[0] = '\0';
  return 1;
}

static int sonic_apply_vm_from_view(SonicView view, char *err, size_t err_size)
{
  const char *sample_value = NULL;
  const char *log_value = NULL;
  const char *enable_value = NULL;
  const char *mode_value = NULL;
  const char *alarm_mv_value = NULL;
  const char *alarm_enable_value = NULL;
  uint8_t has_sample;
  uint8_t has_log;
  uint8_t has_enable;
  uint8_t has_mode;
  uint8_t has_alarm_mv;
  uint8_t has_alarm_enable;
  uint32_t sample_ms = 0U;
  uint32_t log_ms = 0U;
  uint8_t log_enable = 0U;
  uint8_t log_mode = 0U;
  uint32_t alarm_mv = 0U;
  uint8_t alarm_enable = 0U;
  uint8_t changed = 0U;

  if (err == NULL || err_size == 0U) {
    return 0;
  }

  has_sample = (sonic_get_view(view, SONIC_DB_CONFIG, "vm.sample_ms", &sample_value) == 0) ? 1U : 0U;
  has_log = (sonic_get_view(view, SONIC_DB_CONFIG, "vm.log_ms", &log_value) == 0) ? 1U : 0U;
  has_enable = (sonic_get_view(view, SONIC_DB_CONFIG, "vm.log_enable", &enable_value) == 0) ? 1U : 0U;
  has_mode = (sonic_get_view(view, SONIC_DB_CONFIG, "vm.log_mode", &mode_value) == 0) ? 1U : 0U;
  has_alarm_mv = (sonic_get_view(view, SONIC_DB_CONFIG, "vm.alarm_mv", &alarm_mv_value) == 0) ? 1U : 0U;
  has_alarm_enable = (sonic_get_view(view, SONIC_DB_CONFIG, "vm.alarm_enable", &alarm_enable_value) == 0) ? 1U : 0U;

  if (!has_sample && !has_log && !has_enable && !has_mode && !has_alarm_mv && !has_alarm_enable) {
    snprintf(err, err_size, "no vm.* keys in config");
    return 0;
  }

  if (has_sample) {
    if (!parse_u32(sample_value, &sample_ms) || sample_ms < 10U || sample_ms > 5000U) {
      snprintf(err, err_size, "bad vm.sample_ms");
      return 0;
    }
  }
  if (has_log) {
    if (!parse_u32(log_value, &log_ms) || log_ms < 100U || log_ms > 10000U) {
      snprintf(err, err_size, "bad vm.log_ms");
      return 0;
    }
  }
  if (has_enable) {
    if (!sonic_parse_bool_text(enable_value, &log_enable)) {
      snprintf(err, err_size, "bad vm.log_enable");
      return 0;
    }
  }
  if (has_mode) {
    if (!sonic_parse_log_mode_text(mode_value, &log_mode)) {
      snprintf(err, err_size, "bad vm.log_mode");
      return 0;
    }
  }
  if (has_alarm_mv) {
    if (!parse_u32(alarm_mv_value, &alarm_mv) || alarm_mv < 100U || alarm_mv > 3300U) {
      snprintf(err, err_size, "bad vm.alarm_mv");
      return 0;
    }
  }
  if (has_alarm_enable) {
    if (!sonic_parse_bool_text(alarm_enable_value, &alarm_enable)) {
      snprintf(err, err_size, "bad vm.alarm_enable");
      return 0;
    }
  }

  if (has_sample) {
    g_sample_period_ms = sample_ms;
    changed = 1U;
  }
  if (has_log) {
    g_log_period_ms = log_ms;
    changed = 1U;
  }
  if (has_enable) {
    g_logging_enabled = log_enable;
    changed = 1U;
  }
  if (has_mode) {
    g_log_mode = log_mode;
    changed = 1U;
  }
  if (has_alarm_mv) {
    g_alarm_mv = (uint16_t)alarm_mv;
    g_alarm_enabled = 1U;
    changed = 1U;
  }
  if (has_alarm_enable) {
    if (alarm_enable == 0U) {
      g_alarm_enabled = 0U;
      g_alarm_mv = 0U;
      changed = 1U;
    } else if (has_alarm_mv) {
      g_alarm_enabled = 1U;
      changed = 1U;
    } else if (g_alarm_mv >= 100U && g_alarm_mv <= 3300U) {
      g_alarm_enabled = 1U;
      changed = 1U;
    } else {
      snprintf(err, err_size, "need vm.alarm_mv before enable");
      return 0;
    }
  }

  if (!changed) {
    snprintf(err, err_size, "no effective changes");
    return 0;
  }

  err[0] = '\0';
  return 1;
}

typedef struct {
  const char *key;
  const char *value;
} SonicPresetKv;

typedef struct {
  const char *name;
  const char *desc;
  const SonicPresetKv *items;
  uint8_t count;
} SonicPresetDef;

static const SonicPresetKv sonic_preset_lab_safe[] = {
  {"mig.mode", "monitor"},
  {"mig.allow", "uart_rx,ic"},
  {"mig.deny", "uart_tx"},
  {"mig.reset", "1"},
  {"vm.sample_ms", "250"},
  {"vm.log_ms", "1000"},
  {"vm.log_enable", "on"},
  {"vm.log_mode", "all"},
  {"vm.alarm_mv", "1200"},
  {"vm.alarm_enable", "on"},
};

static const SonicPresetKv sonic_preset_enforce_prod[] = {
  {"mig.mode", "enforce"},
  {"mig.allow", "uart_rx,ic"},
  {"mig.deny", "uart_tx,led"},
  {"mig.reset", "1"},
  {"vm.sample_ms", "100"},
  {"vm.log_ms", "2000"},
  {"vm.log_enable", "off"},
  {"vm.log_mode", "hb"},
  {"vm.alarm_mv", "1500"},
  {"vm.alarm_enable", "on"},
};

static const SonicPresetKv sonic_preset_diag_silent[] = {
  {"mig.mode", "off"},
  {"mig.allow", "all"},
  {"mig.deny", "none"},
  {"mig.reset", "1"},
  {"vm.sample_ms", "500"},
  {"vm.log_ms", "3000"},
  {"vm.log_enable", "off"},
  {"vm.log_mode", "hb"},
  {"vm.alarm_enable", "off"},
};

static const SonicPresetDef sonic_presets[] = {
  {
    .name = "lab-safe",
    .desc = "monitor MIG + rich telemetry",
    .items = sonic_preset_lab_safe,
    .count = (uint8_t)(sizeof(sonic_preset_lab_safe) / sizeof(sonic_preset_lab_safe[0])),
  },
  {
    .name = "enforce-prod",
    .desc = "strict MIG + quiet logs",
    .items = sonic_preset_enforce_prod,
    .count = (uint8_t)(sizeof(sonic_preset_enforce_prod) / sizeof(sonic_preset_enforce_prod[0])),
  },
  {
    .name = "diag-silent",
    .desc = "MIG off + minimal runtime noise",
    .items = sonic_preset_diag_silent,
    .count = (uint8_t)(sizeof(sonic_preset_diag_silent) / sizeof(sonic_preset_diag_silent[0])),
  },
};

static const SonicPresetDef *sonic_find_preset(const char *name)
{
  size_t preset_count = sizeof(sonic_presets) / sizeof(sonic_presets[0]);
  if (name == NULL) {
    return NULL;
  }
  for (size_t i = 0U; i < preset_count; ++i) {
    if (strcmp(name, sonic_presets[i].name) == 0) {
      return &sonic_presets[i];
    }
  }
  return NULL;
}

static void sonic_log_preset_list(void)
{
  char buf[112];
  size_t preset_count = sizeof(sonic_presets) / sizeof(sonic_presets[0]);

  snprintf(buf, sizeof(buf), "sonic preset count=%lu\r\n", (unsigned long)preset_count);
  log_send_blocking(buf);
  for (size_t i = 0U; i < preset_count; ++i) {
    snprintf(buf, sizeof(buf), "sonic preset %s: %s\r\n", sonic_presets[i].name, sonic_presets[i].desc);
    log_send_blocking(buf);
  }
}

static void sonic_log_preset_show(const SonicPresetDef *preset)
{
  char buf[112];
  if (preset == NULL) {
    return;
  }
  snprintf(buf, sizeof(buf), "sonic preset %s (%s)\r\n", preset->name, preset->desc);
  log_send_blocking(buf);
  for (uint32_t i = 0U; i < preset->count; ++i) {
    snprintf(buf, sizeof(buf), "preset %s=%s\r\n", preset->items[i].key, preset->items[i].value);
    log_send_blocking(buf);
  }
}

static int sonic_stage_preset_candidate(const SonicPresetDef *preset, char *err, size_t err_size)
{
  if (preset == NULL || err == NULL || err_size == 0U) {
    return 0;
  }

  sonic_preset_stage_snapshot = sonic_lite.candidate;
  sonic_preset_stage_dirty_snapshot = sonic_lite.candidate_dirty;

  for (uint32_t i = 0U; i < preset->count; ++i) {
    int rc = sonic_lite_set(&sonic_lite, SONIC_DB_CONFIG, preset->items[i].key, preset->items[i].value);
    if (rc == 0) {
      continue;
    }
    sonic_lite.candidate = sonic_preset_stage_snapshot;
    sonic_lite.candidate_dirty = sonic_preset_stage_dirty_snapshot;
    if (rc == -3) {
      snprintf(err, err_size, "db_full on %s", preset->items[i].key);
      return 0;
    }
    snprintf(err, err_size, "set failed on %s", preset->items[i].key);
    return 0;
  }

  err[0] = '\0';
  return 1;
}

static int sonic_apply_preset(const SonicPresetDef *preset, SonicView view, char *err, size_t err_size)
{
  if (preset == NULL || err == NULL || err_size == 0U) {
    return 0;
  }
  if (sonic_confirm_pending) {
    snprintf(err, err_size, "confirm pending");
    return 0;
  }

  if (view == SONIC_VIEW_RUNNING) {
    /* Running preset apply must not inherit unrelated staged candidate changes. */
    sonic_lite_abort(&sonic_lite);
    if (!sonic_stage_preset_candidate(preset, err, err_size)) {
      return 0;
    }
    if (sonic_lite_commit(&sonic_lite) != 0) {
      snprintf(err, err_size, "commit failed");
      return 0;
    }
    sonic_clear_confirm_window();
    if (!sonic_apply_mig_from_view(SONIC_VIEW_RUNNING, err, err_size)) {
      return 0;
    }
    if (!sonic_apply_vm_from_view(SONIC_VIEW_RUNNING, err, err_size)) {
      return 0;
    }
    err[0] = '\0';
    return 1;
  }

  if (!sonic_stage_preset_candidate(preset, err, err_size)) {
    return 0;
  }
  err[0] = '\0';
  return 1;
}

static const char *sonic_view_name(SonicView view)
{
  return (view == SONIC_VIEW_CANDIDATE) ? "candidate" : "running";
}

static uint8_t sonic_tick_reached(TickType_t now, TickType_t deadline)
{
  return ((int32_t)(now - deadline) >= 0) ? 1U : 0U;
}

static uint32_t sonic_confirm_remaining_ms(void)
{
  TickType_t now;
  TickType_t remain_ticks;

  if (!sonic_confirm_pending) {
    return 0U;
  }

  now = xTaskGetTickCount();
  if (sonic_tick_reached(now, sonic_confirm_deadline)) {
    return 0U;
  }

  remain_ticks = sonic_confirm_deadline - now;
  return (uint32_t)(remain_ticks * (TickType_t)portTICK_PERIOD_MS);
}

static void sonic_clear_confirm_window(void)
{
  sonic_confirm_pending = 0U;
  sonic_confirm_deadline = 0U;
  sonic_confirm_window_ms = 0U;
}

static void sonic_restore_pending_rollback(void)
{
  sonic_lite.running = sonic_rollback_snapshot;
  sonic_lite_abort(&sonic_lite);
  sonic_clear_confirm_window();
}

static void sonic_audit_pushf(const char *fmt, ...)
{
  va_list ap;
  uint8_t idx = sonic_audit_head;

  if (fmt == NULL) {
    return;
  }

  va_start(ap, fmt);
  (void)vsnprintf(sonic_audit[idx].msg, sizeof(sonic_audit[idx].msg), fmt, ap);
  va_end(ap);
  sonic_audit[idx].tick = xTaskGetTickCount();

  sonic_audit_head = (uint8_t)((sonic_audit_head + 1U) % SONIC_AUDIT_CAP);
  if (sonic_audit_count < SONIC_AUDIT_CAP) {
    sonic_audit_count++;
  }
}

static void sonic_log_history(uint32_t limit)
{
  char buf[112];
  uint32_t count = (uint32_t)sonic_audit_count;

  if (count == 0U) {
    log_send_blocking("sonic history empty\r\n");
    return;
  }
  if (limit == 0U || limit > count) {
    limit = count;
  }

  for (uint32_t i = 0U; i < limit; ++i) {
    uint8_t slot = (uint8_t)((sonic_audit_head + SONIC_AUDIT_CAP - 1U - i) % SONIC_AUDIT_CAP);
    uint32_t ms = (uint32_t)(sonic_audit[slot].tick * (TickType_t)portTICK_PERIOD_MS);
    snprintf(buf, sizeof(buf),
             "sonic hist[%lu] t=%lums %s\r\n",
             (unsigned long)i,
             (unsigned long)ms,
             sonic_audit[slot].msg);
    log_send_blocking(buf);
  }
}

static void sonic_log_counts(void)
{
  char buf[112];
  snprintf(buf, sizeof(buf),
           "sonic running cfg=%u app=%u asic=%u\r\n",
           (unsigned)sonic_count_view(SONIC_VIEW_RUNNING, SONIC_DB_CONFIG),
           (unsigned)sonic_count_view(SONIC_VIEW_RUNNING, SONIC_DB_APPL),
           (unsigned)sonic_count_view(SONIC_VIEW_RUNNING, SONIC_DB_ASIC));
  log_send_blocking(buf);
  snprintf(buf, sizeof(buf),
           "sonic cand cfg=%u app=%u asic=%u\r\n",
           (unsigned)sonic_count_view(SONIC_VIEW_CANDIDATE, SONIC_DB_CONFIG),
           (unsigned)sonic_count_view(SONIC_VIEW_CANDIDATE, SONIC_DB_APPL),
           (unsigned)sonic_count_view(SONIC_VIEW_CANDIDATE, SONIC_DB_ASIC));
  log_send_blocking(buf);
  snprintf(buf, sizeof(buf),
           "sonic dirty=%u diff=%u/%u/%u\r\n",
           (unsigned)sonic_lite_candidate_dirty(&sonic_lite),
           (unsigned)sonic_lite_diff_count(&sonic_lite, SONIC_DB_CONFIG),
           (unsigned)sonic_lite_diff_count(&sonic_lite, SONIC_DB_APPL),
           (unsigned)sonic_lite_diff_count(&sonic_lite, SONIC_DB_ASIC));
  log_send_blocking(buf);
  if (sonic_confirm_pending) {
    snprintf(buf, sizeof(buf),
             "sonic confirm=pending remain_ms=%lu window_ms=%lu\r\n",
             (unsigned long)sonic_confirm_remaining_ms(),
             (unsigned long)sonic_confirm_window_ms);
    log_send_blocking(buf);
  } else {
    log_send_blocking("sonic confirm=none\r\n");
  }
}

static void sonic_log_db(SonicView view, SonicLiteDb db)
{
  char buf[96];
  uint16_t count = sonic_count_view(view, db);
  if (count == 0U) {
    snprintf(buf, sizeof(buf), "sonic %s %s empty\r\n", sonic_view_name(view), sonic_lite_db_name(db));
    log_send_blocking(buf);
    return;
  }

  for (uint16_t i = 0U; i < count; ++i) {
    const char *key = NULL;
    const char *value = NULL;
    if (sonic_list_view(view, db, i, &key, &value) != 0) {
      continue;
    }
    snprintf(buf, sizeof(buf), "%s %s %s=%s\r\n",
             sonic_view_name(view),
             sonic_lite_db_name(db),
             (key != NULL) ? key : "-",
             (value != NULL) ? value : "-");
    log_send_blocking(buf);
  }
}

static uint16_t sonic_log_diff_db(SonicLiteDb db)
{
  char buf[160];
  uint16_t changes = 0U;
  uint16_t rcount = sonic_count_view(SONIC_VIEW_RUNNING, db);
  uint16_t ccount = sonic_count_view(SONIC_VIEW_CANDIDATE, db);

  for (uint16_t i = 0U; i < rcount; ++i) {
    const char *key = NULL;
    const char *running_value = NULL;
    const char *candidate_value = NULL;

    if (sonic_list_view(SONIC_VIEW_RUNNING, db, i, &key, &running_value) != 0 || key == NULL) {
      continue;
    }
    if (sonic_get_view(SONIC_VIEW_CANDIDATE, db, key, &candidate_value) != 0) {
      snprintf(buf, sizeof(buf),
               "sonic diff %s - %s=%s\r\n",
               sonic_lite_db_name(db),
               key,
               (running_value != NULL) ? running_value : "-");
      log_send_blocking(buf);
      changes++;
      continue;
    }
    if (running_value == NULL || candidate_value == NULL || strcmp(running_value, candidate_value) != 0) {
      snprintf(buf, sizeof(buf),
               "sonic diff %s ~ %s:%s->%s\r\n",
               sonic_lite_db_name(db),
               key,
               (running_value != NULL) ? running_value : "-",
               (candidate_value != NULL) ? candidate_value : "-");
      log_send_blocking(buf);
      changes++;
    }
  }

  for (uint16_t i = 0U; i < ccount; ++i) {
    const char *key = NULL;
    const char *running_value = NULL;
    const char *candidate_value = NULL;

    if (sonic_list_view(SONIC_VIEW_CANDIDATE, db, i, &key, &candidate_value) != 0 || key == NULL) {
      continue;
    }
    if (sonic_get_view(SONIC_VIEW_RUNNING, db, key, &running_value) == 0) {
      continue;
    }
    snprintf(buf, sizeof(buf),
             "sonic diff %s + %s=%s\r\n",
             sonic_lite_db_name(db),
             key,
             (candidate_value != NULL) ? candidate_value : "-");
    log_send_blocking(buf);
    changes++;
  }

  if (changes == 0U) {
    snprintf(buf, sizeof(buf), "sonic diff %s clean\r\n", sonic_lite_db_name(db));
    log_send_blocking(buf);
  }
  return changes;
}

static void sonic_poll_confirm_timeout(void)
{
  if (!sonic_confirm_pending) {
    return;
  }
  if (!sonic_tick_reached(xTaskGetTickCount(), sonic_confirm_deadline)) {
    return;
  }
  sonic_restore_pending_rollback();
  sonic_audit_pushf("rollback timeout");
  log_send_blocking("sonic rollback timeout -> restored running\r\n");
  sonic_log_counts();
}

static void sonic_handle_cmd(char *args)
{
  char *p = args;
  char *cmd = next_token(&p);
  char buf[64];

  if (cmd == NULL) {
    sonic_log_usage();
    return;
  }
  if (strcmp(cmd, "cap") == 0) {
    snprintf(buf, sizeof(buf), "sonic cap db=%u cap=%u\r\n",
             (unsigned)SONIC_DB_COUNT,
             (unsigned)SONIC_LITE_DB_CAP);
    log_send_blocking(buf);
    log_send_blocking("sonic names=config,appl,asic\r\n");
    log_send_blocking("sonic txn=candidate->history/preset/apply/commit/confirm/rollback/abort\r\n");
    log_send_blocking("sonic mig keys=mig.mode,mig.allow,mig.deny,mig.reset\r\n");
    log_send_blocking("sonic vm keys=vm.sample_ms,vm.log_ms,vm.log_enable,vm.log_mode,vm.alarm_mv,vm.alarm_enable\r\n");
    log_send_blocking("sonic commit <rollback_ms> arms rollback timer\r\n");
    return;
  }
  if (strcmp(cmd, "show") == 0) {
    char *name = next_token(&p);
    if (name == NULL || strcmp(name, "db") == 0) {
      sonic_log_counts();
      return;
    }
    {
      SonicView view;
      SonicLiteDb db;
      if (sonic_parse_view(name, &view)) {
        char *dbname = next_token(&p);
        if (dbname == NULL || strcmp(dbname, "db") == 0) {
          sonic_log_counts();
          return;
        }
        if (!sonic_lite_db_from_name(dbname, &db)) {
          log_send_blocking("sonic show running|candidate <config|appl|asic>\r\n");
          return;
        }
        sonic_log_db(view, db);
        return;
      }
      if (!sonic_lite_db_from_name(name, &db)) {
        log_send_blocking("sonic show db|running|candidate|config|appl|asic\r\n");
        return;
      }
      sonic_log_db(SONIC_VIEW_RUNNING, db);
      return;
    }
  }
  if (strcmp(cmd, "get") == 0) {
    SonicView view = SONIC_VIEW_RUNNING;
    SonicLiteDb db;
    const char *value = NULL;
    char *a = next_token(&p);
    char *b = next_token(&p);
    char *c = next_token(&p);
    char *dbname = a;
    char *key = b;

    if (sonic_parse_view(a, &view)) {
      dbname = b;
      key = c;
    }
    if (!sonic_lite_db_from_name(dbname, &db) || key == NULL) {
      log_send_blocking("sonic get [running|candidate] <config|appl|asic> <key>\r\n");
      return;
    }
    if (sonic_get_view(view, db, key, &value) != 0) {
      log_send_blocking("sonic get: not_found\r\n");
      return;
    }
    snprintf(buf, sizeof(buf), "sonic %s %s %s=%s\r\n",
             sonic_view_name(view),
             sonic_lite_db_name(db),
             key,
             (value != NULL) ? value : "-");
    log_send_blocking(buf);
    return;
  }
  if (strcmp(cmd, "diff") == 0) {
    char *dbname = next_token(&p);
    uint16_t total = 0U;

    if (dbname != NULL) {
      SonicLiteDb db;
      if (!sonic_lite_db_from_name(dbname, &db)) {
        log_send_blocking("sonic diff [config|appl|asic]\r\n");
        return;
      }
      total = sonic_log_diff_db(db);
    } else {
      total += sonic_log_diff_db(SONIC_DB_CONFIG);
      total += sonic_log_diff_db(SONIC_DB_APPL);
      total += sonic_log_diff_db(SONIC_DB_ASIC);
    }
    snprintf(buf, sizeof(buf), "sonic diff total=%u\r\n", (unsigned)total);
    log_send_blocking(buf);
    return;
  }
  if (strcmp(cmd, "history") == 0) {
    char *limit_str = next_token(&p);
    char *extra = next_token(&p);
    uint32_t limit = 0U;

    if ((limit_str != NULL && !parse_u32(limit_str, &limit)) || extra != NULL) {
      log_send_blocking("sonic history [n]\r\n");
      return;
    }
    sonic_log_history(limit);
    return;
  }
  if (strcmp(cmd, "preset") == 0) {
    char *sub = next_token(&p);
    if (sub == NULL || strcmp(sub, "list") == 0) {
      sonic_log_preset_list();
      return;
    }
    if (strcmp(sub, "show") == 0) {
      char *name = next_token(&p);
      char *extra = next_token(&p);
      const SonicPresetDef *preset;
      if (name == NULL || extra != NULL) {
        log_send_blocking("sonic preset show <name>\r\n");
        return;
      }
      preset = sonic_find_preset(name);
      if (preset == NULL) {
        log_send_blocking("sonic preset: not_found\r\n");
        return;
      }
      sonic_log_preset_show(preset);
      return;
    }
    if (strcmp(sub, "apply") == 0) {
      char *name = next_token(&p);
      char *view_name = next_token(&p);
      char *extra = next_token(&p);
      SonicView view = SONIC_VIEW_RUNNING;
      const SonicPresetDef *preset;
      char err[64];
      char msg[112];
      if (name == NULL ||
          (view_name != NULL && !sonic_parse_view(view_name, &view)) ||
          extra != NULL) {
        log_send_blocking("sonic preset apply <name> [running|candidate]\r\n");
        return;
      }
      preset = sonic_find_preset(name);
      if (preset == NULL) {
        log_send_blocking("sonic preset: not_found\r\n");
        return;
      }
      if (!sonic_apply_preset(preset, view, err, sizeof(err))) {
        snprintf(msg, sizeof(msg), "sonic preset apply: %s\r\n", err);
        log_send_blocking(msg);
        return;
      }
      snprintf(msg, sizeof(msg), "sonic preset apply ok name=%s view=%s\r\n", preset->name, sonic_view_name(view));
      log_send_blocking(msg);
      sonic_audit_pushf("preset %s %s", preset->name, sonic_view_name(view));
      vm_mig_log_status();
      log_status();
      sonic_log_counts();
      return;
    }
    log_send_blocking("sonic preset list|show|apply <name> [running|candidate]\r\n");
    return;
  }
  if (strcmp(cmd, "apply") == 0) {
    char *target = next_token(&p);
    char *view_name = next_token(&p);
    char *extra = next_token(&p);
    SonicView view = SONIC_VIEW_RUNNING;
    char err[64];
    char msg[96];

    if (target == NULL ||
        (view_name != NULL && !sonic_parse_view(view_name, &view)) ||
        extra != NULL) {
      log_send_blocking("sonic apply <mig|vm> [running|candidate]\r\n");
      return;
    }
    if (strcmp(target, "mig") == 0) {
      if (!sonic_apply_mig_from_view(view, err, sizeof(err))) {
        snprintf(msg, sizeof(msg), "sonic apply mig: %s\r\n", err);
        log_send_blocking(msg);
        return;
      }
      snprintf(msg, sizeof(msg), "sonic apply mig ok view=%s\r\n", sonic_view_name(view));
      log_send_blocking(msg);
      sonic_audit_pushf("apply mig %s", sonic_view_name(view));
      vm_mig_log_status();
      return;
    }
    if (strcmp(target, "vm") == 0) {
      if (!sonic_apply_vm_from_view(view, err, sizeof(err))) {
        snprintf(msg, sizeof(msg), "sonic apply vm: %s\r\n", err);
        log_send_blocking(msg);
        return;
      }
      snprintf(msg, sizeof(msg), "sonic apply vm ok view=%s\r\n", sonic_view_name(view));
      log_send_blocking(msg);
      sonic_audit_pushf("apply vm %s", sonic_view_name(view));
      log_status();
      return;
    }
    log_send_blocking("sonic apply <mig|vm> [running|candidate]\r\n");
    return;
  }
  if (strcmp(cmd, "set") == 0) {
    SonicLiteDb db;
    char *name = next_token(&p);
    char *key = next_token(&p);
    char *value = next_token(&p);
    int rc;
    if (!sonic_lite_db_from_name(name, &db) || key == NULL || value == NULL) {
      log_send_blocking("sonic set <config|appl|asic> <key> <value>\r\n");
      return;
    }
    rc = sonic_lite_set(&sonic_lite, db, key, value);
    if (rc == -3) {
      log_send_blocking("sonic set: db_full\r\n");
      return;
    }
    if (rc != 0) {
      log_send_blocking("sonic set: bad_arg\r\n");
      return;
    }
    snprintf(buf, sizeof(buf), "sonic set staged %s.%s dirty=%u\r\n",
             sonic_lite_db_name(db),
             key,
             (unsigned)sonic_lite_candidate_dirty(&sonic_lite));
    log_send_blocking(buf);
    sonic_audit_pushf("set %s.%s dirty=%u",
                      sonic_lite_db_name(db),
                      key,
                      (unsigned)sonic_lite_candidate_dirty(&sonic_lite));
    return;
  }
  if (strcmp(cmd, "commit") == 0) {
    char *ms = next_token(&p);
    char *extra = next_token(&p);
    uint32_t rollback_ms = 0U;
    TickType_t rollback_ticks = 0;

    if ((ms != NULL && !parse_u32(ms, &rollback_ms)) || extra != NULL) {
      log_send_blocking("sonic commit [rollback_ms]\r\n");
      return;
    }
    if (rollback_ms > 0U) {
      sonic_rollback_snapshot = sonic_lite.running;
    }
    if (sonic_lite_commit(&sonic_lite) != 0) {
      log_send_blocking("sonic commit: err\r\n");
      return;
    }
    if (rollback_ms == 0U) {
      sonic_clear_confirm_window();
      log_send_blocking("sonic commit ok\r\n");
      sonic_audit_pushf("commit");
    } else {
      rollback_ticks = pdMS_TO_TICKS(rollback_ms);
      if (rollback_ticks == 0) {
        rollback_ticks = 1;
      }
      sonic_confirm_pending = 1U;
      sonic_confirm_window_ms = rollback_ms;
      sonic_confirm_deadline = xTaskGetTickCount() + rollback_ticks;
      snprintf(buf, sizeof(buf), "sonic commit ok confirm<=%lums\r\n", (unsigned long)rollback_ms);
      log_send_blocking(buf);
      sonic_audit_pushf("commit window=%lums", (unsigned long)rollback_ms);
    }
    sonic_log_counts();
    return;
  }
  if (strcmp(cmd, "confirm") == 0) {
    if (!sonic_confirm_pending) {
      log_send_blocking("sonic confirm: none\r\n");
      return;
    }
    sonic_clear_confirm_window();
    log_send_blocking("sonic confirm ok\r\n");
    sonic_audit_pushf("confirm");
    sonic_log_counts();
    return;
  }
  if (strcmp(cmd, "rollback") == 0) {
    char *arg = next_token(&p);
    char *extra = next_token(&p);
    if ((arg != NULL && strcmp(arg, "now") != 0) || extra != NULL) {
      log_send_blocking("sonic rollback [now]\r\n");
      return;
    }
    if (!sonic_confirm_pending) {
      log_send_blocking("sonic rollback: none\r\n");
      return;
    }
    sonic_restore_pending_rollback();
    sonic_audit_pushf("rollback manual");
    log_send_blocking("sonic rollback now -> restored running\r\n");
    sonic_log_counts();
    return;
  }
  if (strcmp(cmd, "abort") == 0) {
    sonic_lite_abort(&sonic_lite);
    log_send_blocking("sonic abort ok\r\n");
    sonic_audit_pushf("abort");
    sonic_log_counts();
    return;
  }
  sonic_log_usage();
}

static void vm_handle_cmd(char *args)
{
  char *p = args;
  char *cmd = next_token(&p);
  if (cmd == NULL) {
    log_send_blocking("vm: missing subcommand\r\n");
    return;
  }

  if (strcmp(cmd, "reset") == 0) {
    vm32_reset(&vm32);
    log_send_blocking("vm: reset\r\n");
    return;
  }
  if (strcmp(cmd, "demo") == 0) {
    uint8_t program[] = {
      0x01, 0x41, 0x00, 0x00, 0x00, /* PUSH 65 */
      0x01, 0xF0, 0x0F, 0x00, 0x00, /* PUSH 0x0FF0 */
      0x41,                         /* OUT */
      0xFF                          /* HALT */
    };
    for (uint32_t i = 0; i < sizeof(program); ++i) {
      vm32.mem[i] = program[i];
    }
    log_send_blocking("vm: demo loaded\r\n");
    return;
  }
  if (strcmp(cmd, "scenario") == 0) {
    char *name = next_token(&p);
    if (name == NULL || strcmp(name, "list") == 0) {
      scenario_list(log_send_blocking);
      return;
    }

    ScenarioResult result = scenario_run(&vm32, name);
    if (result.status == SCENARIO_STATUS_NOT_FOUND) {
      log_send_blocking("vm scenario: unknown\r\n");
      return;
    }

    if (result.status == SCENARIO_STATUS_LOAD_FAILED) {
      vm_report_fault_if_needed(result.vm_result);
      scenario_log_result(&result);
      return;
    }

    if (result.status == SCENARIO_STATUS_DISPATCHED) {
      scenario_log_result(&result);
      return;
    }

    if (result.vm_fault) {
      vm_report_fault_if_needed(result.vm_result);
    }
    scenario_log_result(&result);
    return;
  }
  if (strcmp(cmd, "trace") == 0) {
    char *mode = next_token(&p);
    if (mode && strcmp(mode, "on") == 0) {
      vm32.trace = 1;
      log_send_blocking("vm: trace on\r\n");
    } else if (mode && strcmp(mode, "off") == 0) {
      vm32.trace = 0;
      log_send_blocking("vm: trace off\r\n");
    } else {
      log_send_blocking("vm trace on|off\r\n");
    }
    return;
  }
  if (strcmp(cmd, "break") == 0) {
    uint32_t addr = 0;
    if (!parse_u32(next_token(&p), &addr)) {
      log_send_blocking("vm break <addr>\r\n");
      return;
    }
    vm32.bp_addr = addr % VM32_MEM_SIZE;
    vm32.bp_valid = 1;
    log_send_blocking("vm: break set\r\n");
    return;
  }
  if (strcmp(cmd, "clear") == 0) {
    vm32.bp_valid = 0;
    log_send_blocking("vm: break cleared\r\n");
    return;
  }
  if (strcmp(cmd, "breaks") == 0) {
    char buf[64];
    if (vm32.bp_valid) {
      snprintf(buf, sizeof(buf), "vm: break @ 0x%04lX\r\n", (unsigned long)vm32.bp_addr);
    } else {
      snprintf(buf, sizeof(buf), "vm: no breakpoints\r\n");
    }
    log_send_blocking(buf);
    return;
  }
  if (strcmp(cmd, "beat") == 0) {
    uint32_t div = 0;
    char *v = next_token(&p);
    if (!parse_u32(v, &div)) {
      log_send_blocking("vm beat <div>\r\n");
      return;
    }
    vm32.io_beat_div = div;
    log_send_blocking("vm: beat set\r\n");
    return;
  }
  if (strcmp(cmd, "load") == 0) {
    uint32_t addr = 0;
    char *a = next_token(&p);
    if (!parse_u32(a, &addr)) {
      log_send_blocking("vm load <addr> <bytes...>\r\n");
      return;
    }
    uint32_t count = 0;
    char *tok;
    while ((tok = next_token(&p)) != NULL) {
      uint32_t b;
      if (!parse_u32(tok, &b) || b > 0xFFU) {
        log_send_blocking("vm load: bad byte\r\n");
        return;
      }
      vm32.mem[(addr + count) % VM32_MEM_SIZE] = (uint8_t)b;
      count++;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "vm: loaded %lu bytes\r\n", (unsigned long)count);
    log_send_blocking(buf);
    return;
  }
  if (strcmp(cmd, "dump") == 0) {
    uint32_t addr = 0, len = 0;
    if (!parse_u32(next_token(&p), &addr) || !parse_u32(next_token(&p), &len)) {
      log_send_blocking("vm dump <addr> <len>\r\n");
      return;
    }
    char buf[96];
    for (uint32_t i = 0; i < len; i += 16U) {
      size_t idx = 0;
      idx += (size_t)snprintf(buf + idx, sizeof(buf) - idx, "%04lX: ",
                              (unsigned long)((addr + i) & 0xFFFFU));
      for (uint32_t j = 0; j < 16U && (i + j) < len; ++j) {
        idx += (size_t)snprintf(buf + idx, sizeof(buf) - idx, "%02X ",
                                vm32.mem[(addr + i + j) % VM32_MEM_SIZE]);
      }
      idx += (size_t)snprintf(buf + idx, sizeof(buf) - idx, "\r\n");
      log_send_blocking(buf);
    }
    return;
  }
  if (strcmp(cmd, "patch") == 0) {
    uint32_t addr = 0, val = 0;
    if (!parse_u32(next_token(&p), &addr) || !parse_u32(next_token(&p), &val) || val > 0xFFU) {
      log_send_blocking("vm patch <addr> <byte>\r\n");
      return;
    }
    vm32.mem[addr % VM32_MEM_SIZE] = (uint8_t)val;
    log_send_blocking("vm: patched\r\n");
    return;
  }
  if (strcmp(cmd, "step") == 0) {
    uint32_t n = 1;
    char *v = next_token(&p);
    if (v && !parse_u32(v, &n)) {
      log_send_blocking("vm step <n>\r\n");
      return;
    }
    Vm32Result res = VM32_OK;
    for (uint32_t i = 0; i < n; ++i) {
      res = vm32_step(&vm32);
      if (res != VM32_OK) {
        break;
      }
    }
    vm_report_fault_if_needed(res);
    vm_log_result("vm step", res);
    return;
  }
  if (strcmp(cmd, "run") == 0) {
    uint32_t addr = 0;
    uint32_t max = 10000U;
    char *a = next_token(&p);
    if (!parse_u32(a, &addr)) {
      log_send_blocking("vm run <addr> [max]\r\n");
      return;
    }
    char *m = next_token(&p);
    if (m) {
      (void)parse_u32(m, &max);
    }
    vm32.pc = addr % VM32_MEM_SIZE;
    uint8_t bp_hit = 0;
    Vm32Result res = vm_run_loop(max, 1U, 1U, &bp_hit);
    if (bp_hit) {
      log_send_blocking("vm: breakpoint hit\r\n");
    }
    vm_report_fault_if_needed(res);
    vm_log_result("vm run", res);
    return;
  }
  if (strcmp(cmd, "verify") == 0) {
    uint32_t addr = 0;
    uint32_t span = 0;
    Vm32CfgReport cfg;
    char *a = next_token(&p);
    char *s = next_token(&p);

    if (!parse_u32(a, &addr)) {
      log_send_blocking("vm verify <addr> [span]\r\n");
      return;
    }
    if (s != NULL && !parse_u32(s, &span)) {
      log_send_blocking("vm verify <addr> [span]\r\n");
      return;
    }
    if (span == 0U && addr < VM32_MEM_SIZE) {
      span = VM32_MEM_SIZE - addr;
    }

    Vm32Result vres = vm32_verify_bounded_cfg(&vm32, addr, span, &cfg);
    vm_log_cfg_report("vm verify", vres, &cfg);
    return;
  }
  if (strcmp(cmd, "runb") == 0) {
    uint32_t addr = 0;
    uint32_t span = 0;
    Vm32CfgReport cfg;
    char *a = next_token(&p);
    char *s = next_token(&p);

    if (!parse_u32(a, &addr)) {
      log_send_blocking("vm runb <addr> [span]\r\n");
      return;
    }
    if (s != NULL && !parse_u32(s, &span)) {
      log_send_blocking("vm runb <addr> [span]\r\n");
      return;
    }
    if (span == 0U && addr < VM32_MEM_SIZE) {
      span = VM32_MEM_SIZE - addr;
    }

    Vm32Result vres = vm32_verify_bounded_cfg(&vm32, addr, span, &cfg);
    vm_log_cfg_report("vm runb", vres, &cfg);
    if (vres != VM32_OK) {
      return;
    }

    vm32.pc = addr % VM32_MEM_SIZE;
    Vm32Result res = vm_run_loop(cfg.max_steps, 1U, 0U, NULL);
    vm_report_fault_if_needed(res);
    vm_log_result("vm runb", res);
    if (res == VM32_OK) {
      log_send_blocking("vm runb: reached predicted step budget\r\n");
    }
    return;
  }
  if (strcmp(cmd, "mig") == 0) {
    char *sub = next_token(&p);
    if (sub == NULL || strcmp(sub, "status") == 0) {
      vm_mig_log_status();
      return;
    }
    if (strcmp(sub, "mode") == 0) {
      char *mode = next_token(&p);
      if (mode == NULL) {
        log_send_blocking("vm mig mode <off|monitor|enforce>\r\n");
        return;
      }
      if (strcmp(mode, "off") == 0) {
        vm32_mig_set_mode(&vm32, VM32_MIG_MODE_OFF);
      } else if (strcmp(mode, "monitor") == 0) {
        vm32_mig_set_mode(&vm32, VM32_MIG_MODE_MONITOR);
      } else if (strcmp(mode, "enforce") == 0) {
        vm32_mig_set_mode(&vm32, VM32_MIG_MODE_ENFORCE);
      } else {
        log_send_blocking("vm mig mode <off|monitor|enforce>\r\n");
        return;
      }
      vm_mig_log_status();
      return;
    }
    if (strcmp(sub, "allow") == 0 || strcmp(sub, "deny") == 0) {
      uint32_t mask = 0U;
      char *res = next_token(&p);
      if (!vm_mig_parse_mask(res, &mask)) {
        log_send_blocking("vm mig allow|deny <uart_tx|uart_rx|led|ic|all>\r\n");
        return;
      }
      if (strcmp(sub, "allow") == 0) {
        vm32_mig_allow(&vm32, mask);
      } else {
        vm32_mig_deny(&vm32, mask);
      }
      vm_mig_log_status();
      return;
    }
    if (strcmp(sub, "reset") == 0) {
      vm32_mig_reset(&vm32);
      vm_mig_log_status();
      return;
    }
    if (strcmp(sub, "apply") == 0) {
      char *json = next_token(&p);
      char err[64];
      if (json == NULL) {
        log_send_blocking("vm mig apply <json>\r\n");
        return;
      }
      if (!vm_mig_apply_json(&vm32, json, err, sizeof(err))) {
        char buf[96];
        snprintf(buf, sizeof(buf), "vm mig apply: %s\r\n", err);
        log_send_blocking(buf);
        log_send_blocking("json keys: mode,allow,deny,reset (compact no-space)\r\n");
        return;
      }
      vm_mig_log_status();
      return;
    }
    log_send_blocking("vm mig status|mode|allow|deny|reset|apply\r\n");
    return;
  }

  log_send_blocking("vm: unknown subcommand\r\n");
}

static void log_ipc_response(const IpcResponse *resp)
{
  char buf[96];
  const char *status = (resp->status == 0U) ? "ok" : "err";
  snprintf(buf, sizeof(buf), "ipc resp: cmd=%u status=%s value=%lu\r\n",
           (unsigned)resp->cmd, status, (unsigned long)resp->value);
  log_send_blocking(buf);
}

static void heartbeat_task(void *arg)
{
  (void)arg;
  for (;;) {
    driver_profile_sample_now();
    board_led_toggle();
    if (g_logging_enabled && (g_log_mode == 0 || g_log_mode == 2)) {
      log_send_blocking("HB\r\n");
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

static void wait_log_flush(void)
{
  if (log_queue == NULL) {
    return;
  }
  while (uxQueueMessagesWaiting(log_queue) != 0U) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

static void log_status(void)
{
  char buf[128];
  const char *mode = "unk";
  if (g_log_mode == 0U) {
    mode = "hb";
  } else if (g_log_mode == 1U) {
    mode = "adc";
  } else if (g_log_mode == 2U) {
    mode = "all";
  }

  snprintf(buf, sizeof(buf),
           "status: sample_ms=%lu log_ms=%lu log=%s mode=%s alarm=%s(%umV) buf=%lu\r\n",
           (unsigned long)g_sample_period_ms,
           (unsigned long)g_log_period_ms,
           g_logging_enabled ? "on" : "off",
           mode,
           g_alarm_enabled ? "on" : "off",
           (unsigned)g_alarm_mv,
           (unsigned long)mpu_stats_count());
  log_send_blocking(buf);
}

static uint8_t snapshot_collect_recent_indices(uint32_t now_ms,
                                               uint32_t window_ms,
                                               uint8_t *out_indices,
                                               uint8_t out_cap)
{
  uint8_t copied = 0U;
  uint8_t count;
  uint8_t head;
  uint8_t start;

  if (out_indices == NULL || out_cap == 0U) {
    return 0U;
  }

  taskENTER_CRITICAL();
  count = snapshot_event_count;
  head = snapshot_event_head;
  start = (uint8_t)((head + SNAPSHOT_EVENT_CAP - count) % SNAPSHOT_EVENT_CAP);
  for (uint8_t i = 0U; i < count && copied < out_cap; ++i) {
    uint8_t idx = (uint8_t)((start + i) % SNAPSHOT_EVENT_CAP);
    const SnapshotEvent *event = &snapshot_events[idx];
    if ((now_ms - event->ts_ms) > window_ms) {
      continue;
    }
    out_indices[copied++] = idx;
  }
  taskEXIT_CRITICAL();
  return copied;
}

static uint16_t snapshot_slice_start_reason(const SnapshotEvent *prev,
                                            const SnapshotEvent *curr)
{
  uint16_t reason = 0U;

  if (curr == NULL) {
    return 0U;
  }
  if (prev == NULL) {
    return SNAPSHOT_SLICE_START_FIRST;
  }
  if (curr->corr_id != prev->corr_id) {
    reason |= SNAPSHOT_SLICE_START_CORR;
  }
  if ((curr->boundary_flags & SNAPSHOT_EVT_BOUNDARY_RESET) != 0U) {
    reason |= SNAPSHOT_SLICE_START_RESET;
  }
  if ((curr->boundary_flags & SNAPSHOT_EVT_BOUNDARY_FAULT) != 0U) {
    reason |= SNAPSHOT_SLICE_START_FAULT;
  }
  if ((curr->boundary_flags & SNAPSHOT_EVT_BOUNDARY_STAGE) != 0U &&
      (prev->stage_valid == 0U || prev->stage_id != curr->stage_id)) {
    reason |= SNAPSHOT_SLICE_START_STAGE;
  }
  return reason;
}

static void snapshot_event_flags_text(uint16_t flags, char *buf, size_t buf_size)
{
  size_t used = 0U;
  uint8_t wrote = 0U;

  if (buf == NULL || buf_size == 0U) {
    return;
  }

  buf[0] = '\0';
  if ((flags & SNAPSHOT_EVT_BOUNDARY_FAULT) != 0U && used < buf_size) {
    used += (size_t)snprintf(buf + used, buf_size - used, "%sfault", wrote ? "|" : "");
    wrote = 1U;
  }
  if ((flags & SNAPSHOT_EVT_BOUNDARY_RESET) != 0U && used < buf_size) {
    used += (size_t)snprintf(buf + used, buf_size - used, "%sreset", wrote ? "|" : "");
    wrote = 1U;
  }
  if ((flags & SNAPSHOT_EVT_BOUNDARY_STAGE) != 0U && used < buf_size) {
    used += (size_t)snprintf(buf + used, buf_size - used, "%sstage", wrote ? "|" : "");
    wrote = 1U;
  }
  if (!wrote) {
    (void)snprintf(buf, buf_size, "none");
  }
}

static void snapshot_slice_reason_text(uint16_t reason, char *buf, size_t buf_size)
{
  size_t used = 0U;
  uint8_t wrote = 0U;

  if (buf == NULL || buf_size == 0U) {
    return;
  }

  buf[0] = '\0';
  if ((reason & SNAPSHOT_SLICE_START_FIRST) != 0U && used < buf_size) {
    used += (size_t)snprintf(buf + used, buf_size - used, "%sfirst", wrote ? "|" : "");
    wrote = 1U;
  }
  if ((reason & SNAPSHOT_SLICE_START_CORR) != 0U && used < buf_size) {
    used += (size_t)snprintf(buf + used, buf_size - used, "%scorr", wrote ? "|" : "");
    wrote = 1U;
  }
  if ((reason & SNAPSHOT_SLICE_START_RESET) != 0U && used < buf_size) {
    used += (size_t)snprintf(buf + used, buf_size - used, "%sreset", wrote ? "|" : "");
    wrote = 1U;
  }
  if ((reason & SNAPSHOT_SLICE_START_FAULT) != 0U && used < buf_size) {
    used += (size_t)snprintf(buf + used, buf_size - used, "%sfault", wrote ? "|" : "");
    wrote = 1U;
  }
  if ((reason & SNAPSHOT_SLICE_START_STAGE) != 0U && used < buf_size) {
    used += (size_t)snprintf(buf + used, buf_size - used, "%sstage", wrote ? "|" : "");
    wrote = 1U;
  }
  if (!wrote) {
    (void)snprintf(buf, buf_size, "none");
  }
}

static uint8_t snapshot_build_slices(const SnapshotEvent *events,
                                     uint8_t event_count,
                                     SnapshotEventSlice *out_slices,
                                     uint8_t out_cap)
{
  uint8_t slice_count = 0U;
  SnapshotEventSlice *active = NULL;

  if (events == NULL || out_slices == NULL || out_cap == 0U) {
    return 0U;
  }

  for (uint8_t i = 0U; i < event_count; ++i) {
    const SnapshotEvent *curr = &events[i];
    const SnapshotEvent *prev = (i == 0U) ? NULL : &events[(uint8_t)(i - 1U)];
    uint16_t reason = snapshot_slice_start_reason(prev, curr);

    if (reason != 0U) {
      if (slice_count >= out_cap) {
        break;
      }
      active = &out_slices[slice_count++];
      memset(active, 0, sizeof(*active));
      active->begin = i;
      active->corr_id = curr->corr_id;
      active->start_reason = reason;
      if (curr->stage_valid != 0U) {
        active->stage_valid = 1U;
        active->stage_id = curr->stage_id;
      }
    }

    if (active == NULL) {
      continue;
    }

    active->end = i;
    if (curr->stage_valid != 0U) {
      active->stage_valid = 1U;
      active->stage_id = curr->stage_id;
    }
    if ((curr->boundary_flags & SNAPSHOT_EVT_BOUNDARY_FAULT) != 0U &&
        active->fault_events < 255U) {
      active->fault_events++;
    }
    if ((curr->boundary_flags & SNAPSHOT_EVT_BOUNDARY_RESET) != 0U &&
        active->reset_events < 255U) {
      active->reset_events++;
    }
  }

  return slice_count;
}

static void snapshot_log_recent_events(uint32_t now_ms, uint32_t window_ms)
{
  char buf[320];
  uint8_t slice_count;
  uint8_t event_count = snapshot_collect_recent_indices(now_ms,
                                                        window_ms,
                                                        snapshot_work_indices,
                                                        SNAPSHOT_EVENT_CAP);

  snprintf(buf, sizeof(buf),
           "snapshot events window_ms=%lu count=%u\r\n",
           (unsigned long)window_ms,
           (unsigned)event_count);
  log_send_blocking(buf);

  if (event_count == 0U) {
    log_send_blocking("snapshot slices count=0\r\n");
    return;
  }

  for (uint8_t i = 0U; i < event_count; ++i) {
    taskENTER_CRITICAL();
    snapshot_work_events[i] = snapshot_events[snapshot_work_indices[i]];
    taskEXIT_CRITICAL();
  }

  slice_count = snapshot_build_slices(snapshot_work_events,
                                      event_count,
                                      snapshot_work_slices,
                                      SNAPSHOT_EVENT_CAP);
  snprintf(buf, sizeof(buf), "snapshot slices count=%u\r\n", (unsigned)slice_count);
  log_send_blocking(buf);

  for (uint8_t s = 0U; s < slice_count; ++s) {
    const SnapshotEventSlice *slice = &snapshot_work_slices[s];
    const SnapshotEvent *first = &snapshot_work_events[slice->begin];
    const SnapshotEvent *last = &snapshot_work_events[slice->end];
    uint8_t event_total = (uint8_t)(slice->end - slice->begin + 1U);
    SnapshotFaultFeatureVector vec = {0};
    uint8_t has_failure = snapshot_slice_has_failure_event(slice,
                                                           snapshot_work_events,
                                                           event_count);
    uint32_t first_age_ms = now_ms - first->ts_ms;
    uint32_t last_age_ms = now_ms - last->ts_ms;
    const char *stage_name = "none";
    char reason_buf[40];

    if (slice->stage_valid != 0U &&
        slice->stage_id < (uint8_t)BRINGUP_PHASE_COUNT) {
      stage_name = bringup_phase_name((BringupPhaseId)slice->stage_id);
    }
    snapshot_slice_reason_text(slice->start_reason, reason_buf, sizeof(reason_buf));
    snprintf(buf, sizeof(buf),
             "snapshot slice id=%u reason=%s corr_id=0x%08lX stage=%s events=%u fault=%u reset=%u age_ms=[%lu..%lu]\r\n",
             (unsigned)s,
             reason_buf,
             (unsigned long)slice->corr_id,
             stage_name,
             (unsigned)event_total,
             (unsigned)slice->fault_events,
             (unsigned)slice->reset_events,
             (unsigned long)first_age_ms,
             (unsigned long)last_age_ms);
    log_send_blocking(buf);

    if (slice->fault_events != 0U || has_failure != 0U) {
      snapshot_collect_fault_features(snapshot_work_events,
                                      event_count,
                                      slice,
                                      &vec);
    }
    if (slice->fault_events != 0U) {
      snapshot_log_fault_features(s, slice, &vec);
      snapshot_log_fault_hypotheses(s,
                                    snapshot_work_events,
                                    event_count,
                                    slice,
                                    &vec);
    }
    if (has_failure != 0U) {
      snapshot_log_failure_classification(s,
                                          snapshot_work_events,
                                          event_count,
                                          slice,
                                          &vec);
    }

    for (uint8_t i = slice->begin; i <= slice->end; ++i) {
      const SnapshotEvent *event = &snapshot_work_events[i];
      const char *event_stage = "none";
      uint32_t age_ms = now_ms - event->ts_ms;
      char flags_buf[24];

      if (event->stage_valid != 0U &&
          event->stage_id < (uint8_t)BRINGUP_PHASE_COUNT) {
        event_stage = bringup_phase_name((BringupPhaseId)event->stage_id);
      }
      snapshot_event_flags_text(event->boundary_flags, flags_buf, sizeof(flags_buf));
      snprintf(buf, sizeof(buf),
               "snapshot event slice=%u id=%u age_ms=%lu ts_ms=%lu corr_id=0x%08lX flags=%s stage=%s msg=%s\r\n",
               (unsigned)s,
               (unsigned)i,
               (unsigned long)age_ms,
               (unsigned long)event->ts_ms,
               (unsigned long)event->corr_id,
               flags_buf,
               event_stage,
               event->msg);
      log_send_blocking(buf);
    }
  }
}

static uint32_t driver_profile_now_ms(void)
{
  return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static uint8_t driver_profile_irq_bin(uint32_t irq_rate_per_sec)
{
  if (irq_rate_per_sec == 0U) {
    return 0U;
  }
  if (irq_rate_per_sec <= 4U) {
    return 1U;
  }
  if (irq_rate_per_sec <= 15U) {
    return 2U;
  }
  if (irq_rate_per_sec <= 63U) {
    return 3U;
  }
  if (irq_rate_per_sec <= 255U) {
    return 4U;
  }
  return 5U;
}

static uint8_t driver_profile_dma_bin(uint32_t occ_percent)
{
  if (occ_percent == 0U) {
    return 0U;
  }
  if (occ_percent < 25U) {
    return 1U;
  }
  if (occ_percent < 50U) {
    return 2U;
  }
  if (occ_percent < 75U) {
    return 3U;
  }
  if (occ_percent < 100U) {
    return 4U;
  }
  return 5U;
}

static uint32_t driver_profile_dma_percent(uint32_t used, uint32_t depth)
{
  uint32_t pct;

  if (depth == 0U) {
    return 0U;
  }
  pct = (used * 100U + (depth / 2U)) / depth;
  if (pct > 100U) {
    pct = 100U;
  }
  return pct;
}

static void driver_profile_reset_runtime(void)
{
  memset(driver_profile_runtime, 0, sizeof(driver_profile_runtime));
  driver_profile_start_ms = driver_profile_now_ms();
  driver_profile_last_sample_ms = driver_profile_start_ms;
}

static void driver_profile_sample_driver(KdiDriverId driver, uint32_t dt_ms)
{
  DriverProfileRuntime *runtime = NULL;
  KdiIrqDriverCounters irq_cnt = {0};
  KdiDmaRingOccupancy dma = {0};
  int irq_rc;
  int dma_rc;

  if ((uint32_t)driver >= (uint32_t)KDI_DRIVER_COUNT) {
    return;
  }

  runtime = &driver_profile_runtime[driver];

  irq_rc = kdi_irq_get_driver_counters(driver, &irq_cnt);
  if (irq_rc == KDI_OK) {
    if (runtime->irq_prev_valid != 0U && dt_ms != 0U) {
      uint32_t delta = irq_cnt.irq_enter_total - runtime->irq_prev_enter_total;
      uint32_t rate = (uint32_t)((((uint64_t)delta * 1000ULL) + (uint64_t)(dt_ms / 2U)) / (uint64_t)dt_ms);
      uint8_t bin = driver_profile_irq_bin(rate);
      runtime->irq_rate_bins[bin]++;
      runtime->irq_sample_count++;
    }
    runtime->irq_prev_enter_total = irq_cnt.irq_enter_total;
    runtime->irq_prev_valid = 1U;
  }

  dma_rc = kdi_dma_get_ring_occupancy(driver, &dma);
  if (dma_rc == KDI_OK) {
    uint32_t rx_used = (uint32_t)dma.rx_posted + (uint32_t)dma.rx_ready;
    uint32_t tx_used = (uint32_t)dma.tx_pending + (uint32_t)dma.tx_done;
    uint8_t rx_bin = driver_profile_dma_bin(driver_profile_dma_percent(rx_used, dma.rx_depth));
    uint8_t tx_bin = driver_profile_dma_bin(driver_profile_dma_percent(tx_used, dma.tx_depth));
    runtime->dma_rx_occ_bins[rx_bin]++;
    runtime->dma_tx_occ_bins[tx_bin]++;
    runtime->dma_sample_count++;
  }
}

static void driver_profile_sample_now(void)
{
  uint32_t now_ms = driver_profile_now_ms();
  uint32_t dt_ms = now_ms - driver_profile_last_sample_ms;

  if (driver_profile_start_ms == 0U &&
      driver_profile_last_sample_ms == 0U) {
    driver_profile_start_ms = now_ms;
  }

  for (uint32_t i = 0U; i < (uint32_t)KDI_DRIVER_COUNT; ++i) {
    driver_profile_sample_driver((KdiDriverId)i, dt_ms);
  }
  driver_profile_last_sample_ms = now_ms;
}

static uint32_t driver_profile_cap_total(const KdiDriverProfileStats *profile)
{
  uint32_t total = 0U;

  if (profile == NULL) {
    return 0U;
  }
  for (uint32_t req = (uint32_t)KDI_REQ_MPU; req <= (uint32_t)KDI_REQ_RESET; ++req) {
    total += profile->request_total[req];
  }
  return total;
}

static uint32_t driver_profile_per_k(uint32_t value, uint32_t total)
{
  if (total == 0U) {
    return 0U;
  }
  return (uint32_t)((((uint64_t)value * 1000ULL) + (uint64_t)(total / 2U)) / (uint64_t)total);
}

static void driver_profile_log_json(void)
{
  char buf[320];
  uint32_t now_ms;
  uint32_t uptime_ms;

  driver_profile_sample_now();
  now_ms = driver_profile_now_ms();
  uptime_ms = now_ms - driver_profile_start_ms;

  snprintf(buf, sizeof(buf),
           "{\"type\":\"driver-profile-meta\",\"version\":1,\"uptime_ms\":%lu,\"drivers\":%u}\r\n",
           (unsigned long)uptime_ms,
           (unsigned)KDI_DRIVER_COUNT);
  log_send_blocking(buf);

  for (uint32_t i = 0U; i < (uint32_t)KDI_DRIVER_COUNT; ++i) {
    KdiDriverId driver = (KdiDriverId)i;
    const DriverProfileRuntime *runtime = &driver_profile_runtime[i];
    KdiDriverProfileStats profile = {0};
    uint32_t cap_total = 0U;
    int rc = kdi_profile_get_driver(driver, &profile);

    cap_total = driver_profile_cap_total(&profile);
    snprintf(buf, sizeof(buf),
             "{\"type\":\"driver-profile\",\"driver\":\"%s\",\"irq_samples\":%lu,\"dma_samples\":%lu,\"kdi_rc\":\"%s\"}\r\n",
             kdi_driver_name(driver),
             (unsigned long)runtime->irq_sample_count,
             (unsigned long)runtime->dma_sample_count,
             kdi_result_name(rc));
    log_send_blocking(buf);

    snprintf(buf, sizeof(buf),
             "{\"type\":\"driver-profile-irq\",\"driver\":\"%s\",\"dist\":{\"0\":%lu,\"1_4\":%lu,\"5_15\":%lu,\"16_63\":%lu,\"64_255\":%lu,\"256p\":%lu}}\r\n",
             kdi_driver_name(driver),
             (unsigned long)runtime->irq_rate_bins[0],
             (unsigned long)runtime->irq_rate_bins[1],
             (unsigned long)runtime->irq_rate_bins[2],
             (unsigned long)runtime->irq_rate_bins[3],
             (unsigned long)runtime->irq_rate_bins[4],
             (unsigned long)runtime->irq_rate_bins[5]);
    log_send_blocking(buf);

    snprintf(buf, sizeof(buf),
             "{\"type\":\"driver-profile-dma\",\"driver\":\"%s\",\"rx\":{\"0\":%lu,\"1_24\":%lu,\"25_49\":%lu,\"50_74\":%lu,\"75_99\":%lu,\"100\":%lu},\"tx\":{\"0\":%lu,\"1_24\":%lu,\"25_49\":%lu,\"50_74\":%lu,\"75_99\":%lu,\"100\":%lu}}\r\n",
             kdi_driver_name(driver),
             (unsigned long)runtime->dma_rx_occ_bins[0],
             (unsigned long)runtime->dma_rx_occ_bins[1],
             (unsigned long)runtime->dma_rx_occ_bins[2],
             (unsigned long)runtime->dma_rx_occ_bins[3],
             (unsigned long)runtime->dma_rx_occ_bins[4],
             (unsigned long)runtime->dma_rx_occ_bins[5],
             (unsigned long)runtime->dma_tx_occ_bins[0],
             (unsigned long)runtime->dma_tx_occ_bins[1],
             (unsigned long)runtime->dma_tx_occ_bins[2],
             (unsigned long)runtime->dma_tx_occ_bins[3],
             (unsigned long)runtime->dma_tx_occ_bins[4],
             (unsigned long)runtime->dma_tx_occ_bins[5]);
    log_send_blocking(buf);

    snprintf(buf, sizeof(buf),
             "{\"type\":\"driver-profile-kdi-a\",\"driver\":\"%s\",\"total\":%lu,\"mpu\":{\"cnt\":%lu,\"fail\":%lu,\"pk\":%lu},\"irq\":{\"cnt\":%lu,\"fail\":%lu,\"pk\":%lu},\"dma\":{\"cnt\":%lu,\"fail\":%lu,\"pk\":%lu}}\r\n",
             kdi_driver_name(driver),
             (unsigned long)cap_total,
             (unsigned long)profile.request_total[KDI_REQ_MPU],
             (unsigned long)profile.request_fail[KDI_REQ_MPU],
             (unsigned long)driver_profile_per_k(profile.request_total[KDI_REQ_MPU], cap_total),
             (unsigned long)profile.request_total[KDI_REQ_IRQ],
             (unsigned long)profile.request_fail[KDI_REQ_IRQ],
             (unsigned long)driver_profile_per_k(profile.request_total[KDI_REQ_IRQ], cap_total),
             (unsigned long)profile.request_total[KDI_REQ_DMA],
             (unsigned long)profile.request_fail[KDI_REQ_DMA],
             (unsigned long)driver_profile_per_k(profile.request_total[KDI_REQ_DMA], cap_total));
    log_send_blocking(buf);

    snprintf(buf, sizeof(buf),
             "{\"type\":\"driver-profile-kdi-b\",\"driver\":\"%s\",\"fault\":{\"cnt\":%lu,\"fail\":%lu,\"pk\":%lu},\"power\":{\"cnt\":%lu,\"fail\":%lu,\"pk\":%lu},\"reset\":{\"cnt\":%lu,\"fail\":%lu,\"pk\":%lu}}\r\n",
             kdi_driver_name(driver),
             (unsigned long)profile.request_total[KDI_REQ_FAULT],
             (unsigned long)profile.request_fail[KDI_REQ_FAULT],
             (unsigned long)driver_profile_per_k(profile.request_total[KDI_REQ_FAULT], cap_total),
             (unsigned long)profile.request_total[KDI_REQ_POWER],
             (unsigned long)profile.request_fail[KDI_REQ_POWER],
             (unsigned long)driver_profile_per_k(profile.request_total[KDI_REQ_POWER], cap_total),
             (unsigned long)profile.request_total[KDI_REQ_RESET],
             (unsigned long)profile.request_fail[KDI_REQ_RESET],
             (unsigned long)driver_profile_per_k(profile.request_total[KDI_REQ_RESET], cap_total));
    log_send_blocking(buf);

    snprintf(buf, sizeof(buf),
             "{\"type\":\"driver-profile-state-visits\",\"driver\":\"%s\",\"init\":%lu,\"probe\":%lu,\"ready\":%lu,\"active\":%lu,\"error\":%lu,\"reset\":%lu,\"dead\":%lu}\r\n",
             kdi_driver_name(driver),
             (unsigned long)profile.state_visit[KDI_STATE_INIT],
             (unsigned long)profile.state_visit[KDI_STATE_PROBE],
             (unsigned long)profile.state_visit[KDI_STATE_READY],
             (unsigned long)profile.state_visit[KDI_STATE_ACTIVE],
             (unsigned long)profile.state_visit[KDI_STATE_ERROR],
             (unsigned long)profile.state_visit[KDI_STATE_RESET],
             (unsigned long)profile.state_visit[KDI_STATE_DEAD]);
    log_send_blocking(buf);

    for (uint32_t from = 0U; from < KDI_PROFILE_STATE_COUNT; ++from) {
      snprintf(buf, sizeof(buf),
               "{\"type\":\"driver-profile-state-row\",\"driver\":\"%s\",\"from\":\"%s\",\"init\":%lu,\"probe\":%lu,\"ready\":%lu,\"active\":%lu,\"error\":%lu,\"reset\":%lu,\"dead\":%lu}\r\n",
               kdi_driver_name(driver),
               kdi_driver_state_name((KdiDriverState)from),
               (unsigned long)profile.state_transition[from][KDI_STATE_INIT],
               (unsigned long)profile.state_transition[from][KDI_STATE_PROBE],
               (unsigned long)profile.state_transition[from][KDI_STATE_READY],
               (unsigned long)profile.state_transition[from][KDI_STATE_ACTIVE],
               (unsigned long)profile.state_transition[from][KDI_STATE_ERROR],
               (unsigned long)profile.state_transition[from][KDI_STATE_RESET],
               (unsigned long)profile.state_transition[from][KDI_STATE_DEAD]);
      log_send_blocking(buf);
    }
  }
}

static void profile_log_usage(void)
{
  log_send_blocking("profile [json|reset]\r\n");
}

static void profile_handle_cmd(char *args)
{
  char *p = args;
  char *cmd = next_token(&p);

  if (cmd == NULL || strcmp(cmd, "json") == 0) {
    driver_profile_log_json();
    return;
  }
  if (strcmp(cmd, "reset") == 0) {
    driver_profile_reset_runtime();
    log_send_blocking("profile reset: ok\r\n");
    return;
  }
  profile_log_usage();
}

static void snapshot_log_system(const char *reason)
{
  char buf[320];
  uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
  const char *reason_text = (reason != NULL) ? reason : "on_demand";
  const char *active_stage = "none";
  KdiIrqStats irq_global = {0};

  if (bringup_phase_model.active_valid != 0U &&
      bringup_phase_model.active_phase < (uint8_t)BRINGUP_PHASE_COUNT) {
    active_stage = bringup_phase_name((BringupPhaseId)bringup_phase_model.active_phase);
  }

  kdi_irq_get_stats(&irq_global);

  snprintf(buf, sizeof(buf),
           "snapshot begin reason=%s ts_ms=%lu\r\n",
           reason_text,
           (unsigned long)now_ms);
  log_send_blocking(buf);

  snprintf(buf, sizeof(buf),
           "snapshot bringup boot_complete=%u active=%s last_error=%ld\r\n",
           (unsigned)bringup_phase_model.boot_complete,
           active_stage,
           (long)bringup_phase_model.last_error);
  log_send_blocking(buf);

  snprintf(buf, sizeof(buf),
           "snapshot policy global irq_starve_ms=%lu deferred_pending=%lu\r\n",
           (unsigned long)irq_global.starvation_ms,
           (unsigned long)irq_global.deferred_pending);
  log_send_blocking(buf);

  for (uint32_t i = 0U; i < (uint32_t)KDI_DRIVER_COUNT; ++i) {
    KdiDriverId driver = (KdiDriverId)i;
    KdiDriverState st = KDI_STATE_INIT;
    KdiCapUsageTrace cap_trace = {0};
    KdiIrqDriverStats irq_drv = {0};
    KdiIrqDriverCounters irq_cnt = {0};
    KdiDmaRingOccupancy dma = {0};
    char cap_declared[48];
    char cap_used[48];
    char cap_active[48];
    char cap_unused[48];
    uint8_t tok_active = 0U;
    uint32_t ttl_ms = 0U;
    uint32_t rem_ms = 0U;
    int st_rc = kdi_driver_get_state(driver, &st);
    int cap_rc = kdi_cap_usage_get(driver, &cap_trace);
    int tok_rc = kdi_token_is_active(driver, &tok_active);
    int ttl_rc = kdi_get_token_ttl_ms(driver, &ttl_ms);
    int rem_rc = kdi_token_remaining_ms(driver, &rem_ms);
    int irq_rc = kdi_irq_get_driver_stats(driver, &irq_drv);
    int irq_cnt_rc = kdi_irq_get_driver_counters(driver, &irq_cnt);
    int dma_rc = kdi_dma_get_ring_occupancy(driver, &dma);

    kdi_cap_mask_text(cap_trace.declared_mask, cap_declared, sizeof(cap_declared));
    kdi_cap_mask_text(cap_trace.used_mask, cap_used, sizeof(cap_used));
    kdi_cap_mask_text(cap_trace.active_mask, cap_active, sizeof(cap_active));
    kdi_cap_mask_text(cap_trace.declared_not_used_mask, cap_unused, sizeof(cap_unused));

    if (rem_rc != KDI_OK) {
      rem_ms = 0U;
    }

    snprintf(buf, sizeof(buf),
             "snapshot driver=%s state=%s st_rc=%s tok_active=%u tok_rc=%s ttl_ms=%lu rem_ms=%lu ttl_rc=%s rem_rc=%s\r\n",
             kdi_driver_name(driver),
             kdi_driver_state_name(st),
             kdi_result_name(st_rc),
             (unsigned)tok_active,
             kdi_result_name(tok_rc),
             (unsigned long)ttl_ms,
             (unsigned long)rem_ms,
             kdi_result_name(ttl_rc),
             kdi_result_name(rem_rc));
    log_send_blocking(buf);

    snprintf(buf, sizeof(buf),
             "snapshot cap drv=%s declared=%s used=%s active=%s decl_unused=%s cap_rc=%s\r\n",
             kdi_driver_name(driver),
             cap_declared,
             cap_used,
             cap_active,
             cap_unused,
             kdi_result_name(cap_rc));
    log_send_blocking(buf);
    snprintf(buf, sizeof(buf),
             "snapshot ai cap-shrink drv=%s remove=%s window=obs[%lu,%lu] risk=%s note=%s\r\n",
             kdi_driver_name(driver),
             cap_unused,
             (unsigned long)cap_trace.observation_start_ms,
             (unsigned long)cap_trace.observation_end_ms,
             kdi_cap_review_risk_name(kdi_cap_review_risk_level(cap_trace.observation_window_ms,
                                                                kdi_cap_total_requests(&cap_trace),
                                                                st)),
             kdi_cap_review_risk_hint(kdi_cap_review_risk_level(cap_trace.observation_window_ms,
                                                                kdi_cap_total_requests(&cap_trace),
                                                                st)));
    log_send_blocking(buf);

    snprintf(buf, sizeof(buf),
             "snapshot policy drv=%s irq_budget=%lu throttled=%u cooldown_lvl=%u cooldown_until_ms=%lu irq_rc=%s\r\n",
             kdi_driver_name(driver),
             (unsigned long)irq_drv.budget_per_sec,
             (unsigned)irq_drv.throttled,
             (unsigned)irq_drv.cooldown_level,
             (unsigned long)irq_drv.cooldown_until_ms,
             kdi_result_name(irq_rc));
    log_send_blocking(buf);

    snprintf(buf, sizeof(buf),
             "snapshot irq drv=%s enter=%lu throttle=%lu defer=%lu drop=%lu worker=%lu werr=%lu unsafe=%lu window=%lu pending=%lu cnt_rc=%s\r\n",
             kdi_driver_name(driver),
             (unsigned long)irq_cnt.irq_enter_total,
             (unsigned long)irq_cnt.irq_throttle_total,
             (unsigned long)irq_cnt.irq_defer_total,
             (unsigned long)irq_cnt.irq_drop_total,
             (unsigned long)irq_cnt.irq_worker_total,
             (unsigned long)irq_cnt.irq_worker_error_total,
             (unsigned long)irq_cnt.irq_unsafe_total,
             (unsigned long)irq_drv.window_count,
             (unsigned long)irq_drv.deferred_pending,
             kdi_result_name(irq_cnt_rc));
    log_send_blocking(buf);

    snprintf(buf, sizeof(buf),
             "snapshot dma drv=%s rx_posted=%u rx_ready=%u tx_pending=%u tx_done=%u rx_depth=%u tx_depth=%u bufs=%u dma_rc=%s\r\n",
             kdi_driver_name(driver),
             (unsigned)dma.rx_posted,
             (unsigned)dma.rx_ready,
             (unsigned)dma.tx_pending,
             (unsigned)dma.tx_done,
             (unsigned)dma.rx_depth,
             (unsigned)dma.tx_depth,
             (unsigned)dma.total_buffers,
             kdi_result_name(dma_rc));
    log_send_blocking(buf);
  }

  snapshot_log_recent_events(now_ms, SNAPSHOT_EVENT_WINDOW_MS);
  log_send_blocking("snapshot end\r\n");
}

static void cli_task(void *arg)
{
  (void)arg;
  char line[CLI_LINE_MAX];
  size_t len = 0;

  log_send_blocking("CLI ready.\r\n");
  log_send_blocking("Commands: help, status, stat, rate <ms>, log <ms>\r\n");
  log_send_blocking("          enable, disable, mode <hb|adc|all>\r\n");
  log_send_blocking("          snapshot\r\n");
  log_send_blocking("          profile [json|reset]\r\n");
  log_send_blocking("          alarm <mv>, alarm off, dump [n], clear\r\n");
  log_send_blocking("          fault last | fault dump | fault retained [json]\r\n");
  log_send_blocking("          reset cause [json]\r\n");
  log_send_blocking("          bringup [check|json|mpu|phase ...|stage ...]\r\n");
  log_send_blocking("          kdi show|last|probe <allow|deny|authfail>|token <show|rotate|revoke|ttl>|driver <show|probe|ready|fail|active|error|reset|reinit|reclaim>|irq <show|budget|cooldown|starve|enter|defer|exit|worker|poll|unsafe|storm>|fault <show|domain>|cap <show|json|review|review-json> [driver]\r\n");
  log_send_blocking("          dep show|json|impact|impact-json <driver> | dep whatif|whatif-json <reset|throttle|deny> <driver>\r\n");
  log_send_blocking("          sonic cap|show|get|set|diff|history|preset|apply|commit|confirm|rollback|abort\r\n");
  log_send_blocking("          ipc ping|snapshot|rate <ms>|log <ms>\r\n");
  log_send_blocking("          ipc mode <hb|adc|all>|alarm <mv>|alarm off\r\n");
  log_send_blocking("          vm reset|demo|scenario <list|name>|trace on|off\r\n");
  log_send_blocking("          vm beat|break|clear|breaks|load|dump|patch\r\n");
  log_send_blocking("          vm step|run|verify|runb|mig\r\n");
  wait_log_flush();
  g_cli_ready = 1;
  if (bringup_phase_model.active_valid != 0U &&
      bringup_phase_model.active_phase == (uint8_t)BRINGUP_PHASE_USER_WORKLOAD_ENABLE) {
    bringup_boot_phase_succeed(BRINGUP_PHASE_USER_WORKLOAD_ENABLE);
  }

  for (;;) {
    char c;
    sonic_poll_confirm_timeout();
    if (!board_uart_read_char(&c)) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    if (c == '\r' || c == '\n') {
      line[len] = '\0';
      if (len > 0) {
        if (strcmp(line, "help") == 0) {
          log_send_blocking("help | status | stat | rate <ms> | log <ms> | enable | disable\r\n");
          log_send_blocking("snapshot\r\n");
          log_send_blocking("profile [json|reset]\r\n");
          log_send_blocking("mode <hb|adc|all> | alarm <mv> | alarm off | dump [n] | clear\r\n");
          log_send_blocking("fault last | fault dump | fault retained [json] | reset cause [json]\r\n");
          log_send_blocking("bringup [check|json|mpu|phase ...|stage ...] | kdi ... | dep show|json|impact|impact-json <driver> | dep whatif|whatif-json <reset|throttle|deny> <driver> | profile [json|reset]\r\n");
        } else if (strcmp(line, "status") == 0) {
          log_status();
        } else if (strcmp(line, "snapshot") == 0) {
          snapshot_log_system("cli");
        } else if (strcmp(line, "profile") == 0) {
          profile_handle_cmd(line + 7);
        } else if (strncmp(line, "profile ", 8) == 0) {
          profile_handle_cmd(line + 8);
        } else if (strcmp(line, "stat") == 0) {
          g_snapshot_req = 1;
        } else if (strcmp(line, "dump") == 0) {
          mpu_stats_dump(log_queue, 0);
        } else if (strncmp(line, "dump ", 5) == 0) {
          uint32_t v = (uint32_t)strtoul(line + 5, NULL, 10);
          if (v > 0U) {
            mpu_stats_dump(log_queue, v);
          } else {
            log_send_blocking("dump <n> expects n > 0\r\n");
          }
        } else if (strcmp(line, "clear") == 0) {
          mpu_stats_clear();
          log_send_blocking("stats cleared\r\n");
        } else if (strcmp(line, "fault") == 0 || strcmp(line, "fault last") == 0) {
          fault_emit_last_json();
        } else if (strcmp(line, "fault dump") == 0) {
          fault_emit_dump_human();
          fault_emit_dump_json();
        } else if (strcmp(line, "fault retained") == 0) {
          fault_emit_retained_human();
        } else if (strcmp(line, "fault retained json") == 0) {
          fault_emit_retained_json();
        } else if (strcmp(line, "reset cause") == 0) {
          fault_emit_reset_cause_human();
        } else if (strcmp(line, "reset cause json") == 0) {
          fault_emit_reset_cause_json();
        } else if (strcmp(line, "bringup") == 0) {
          bringup_handle_cmd(line + 7);
        } else if (strncmp(line, "bringup ", 8) == 0) {
          bringup_handle_cmd(line + 8);
        } else if (strcmp(line, "kdi") == 0) {
          kdi_handle_cmd(line + 3);
        } else if (strncmp(line, "kdi ", 4) == 0) {
          kdi_handle_cmd(line + 4);
        } else if (strcmp(line, "dep") == 0) {
          dep_handle_cmd(line + 3);
        } else if (strncmp(line, "dep ", 4) == 0) {
          dep_handle_cmd(line + 4);
        } else if (strcmp(line, "sonic") == 0) {
          sonic_handle_cmd(line + 5);
        } else if (strncmp(line, "sonic ", 6) == 0) {
          sonic_handle_cmd(line + 6);
        } else if (strncmp(line, "vm ", 3) == 0) {
          vm_handle_cmd(line + 3);
        } else if (strncmp(line, "ipc ", 4) == 0) {
          IpcCommand cmd = {0};
          BaseType_t sent = pdFALSE;
          const char *args = line + 4;

          if (strcmp(args, "ping") == 0) {
            cmd.cmd = IPC_CMD_PING;
            sent = pdTRUE;
          } else if (strcmp(args, "snapshot") == 0) {
            cmd.cmd = IPC_CMD_SNAPSHOT;
            sent = pdTRUE;
          } else if (strncmp(args, "rate ", 5) == 0) {
            uint32_t v = (uint32_t)strtoul(args + 5, NULL, 10);
            cmd.cmd = IPC_CMD_SET_RATE;
            cmd.value = v;
            sent = pdTRUE;
          } else if (strncmp(args, "log ", 4) == 0) {
            uint32_t v = (uint32_t)strtoul(args + 4, NULL, 10);
            cmd.cmd = IPC_CMD_SET_LOG;
            cmd.value = v;
            sent = pdTRUE;
          } else if (strncmp(args, "mode ", 5) == 0) {
            cmd.cmd = IPC_CMD_SET_MODE;
            if (strcmp(args + 5, "hb") == 0) {
              cmd.arg = 0;
            } else if (strcmp(args + 5, "adc") == 0) {
              cmd.arg = 1;
            } else if (strcmp(args + 5, "all") == 0) {
              cmd.arg = 2;
            } else {
              log_send_blocking("ipc mode must be hb|adc|all\r\n");
              len = 0;
              continue;
            }
            sent = pdTRUE;
          } else if (strncmp(args, "alarm ", 6) == 0) {
            if (strcmp(args + 6, "off") == 0) {
              cmd.cmd = IPC_CMD_ALARM_OFF;
            } else {
              uint32_t mv = (uint32_t)strtoul(args + 6, NULL, 10);
              cmd.cmd = IPC_CMD_SET_ALARM;
              cmd.value = mv;
            }
            sent = pdTRUE;
          } else {
            log_send_blocking("ipc cmd: ping|snapshot|rate|log|mode|alarm\r\n");
          }

          if (sent) {
            if (ipc_cmd_queue == NULL || ipc_resp_queue == NULL) {
              log_send_blocking("ipc not ready\r\n");
            } else if (xQueueSend(ipc_cmd_queue, &cmd, pdMS_TO_TICKS(100)) != pdPASS) {
              log_send_blocking("ipc send failed\r\n");
            } else {
              IpcResponse resp;
              if (xQueueReceive(ipc_resp_queue, &resp, pdMS_TO_TICKS(200)) == pdPASS) {
                log_ipc_response(&resp);
              } else {
                log_send_blocking("ipc timeout\r\n");
              }
            }
          }
        } else if (strncmp(line, "rate ", 5) == 0) {
          uint32_t v = (uint32_t)strtoul(line + 5, NULL, 10);
          if (v >= 10U && v <= 5000U) {
            g_sample_period_ms = v;
            log_status();
          } else {
            log_send_blocking("rate out of range (10..5000)\r\n");
          }
        } else if (strncmp(line, "log ", 4) == 0) {
          uint32_t v = (uint32_t)strtoul(line + 4, NULL, 10);
          if (v >= 100U && v <= 10000U) {
            g_log_period_ms = v;
            log_status();
          } else {
            log_send_blocking("log out of range (100..10000)\r\n");
          }
        } else if (strcmp(line, "enable") == 0) {
          g_logging_enabled = 1;
          log_status();
        } else if (strcmp(line, "disable") == 0) {
          g_logging_enabled = 0;
          log_status();
        } else if (strncmp(line, "mode ", 5) == 0) {
          if (strcmp(line + 5, "hb") == 0) {
            g_log_mode = 0;
          } else if (strcmp(line + 5, "adc") == 0) {
            g_log_mode = 1;
          } else if (strcmp(line + 5, "all") == 0) {
            g_log_mode = 2;
          } else {
            log_send_blocking("mode must be hb|adc|all\r\n");
            len = 0;
            continue;
          }
          log_status();
        } else if (strncmp(line, "alarm ", 6) == 0) {
          if (strcmp(line + 6, "off") == 0) {
            g_alarm_enabled = 0;
            g_alarm_mv = 0;
            log_status();
          } else {
            uint32_t mv = (uint32_t)strtoul(line + 6, NULL, 10);
            if (mv >= 100U && mv <= 3300U) {
              g_alarm_mv = (uint16_t)mv;
              g_alarm_enabled = 1;
              log_status();
            } else {
              log_send_blocking("alarm out of range (100..3300)\r\n");
            }
          }
        } else if (strcmp(line, "alarm off") == 0) {
          g_alarm_enabled = 0;
          g_alarm_mv = 0;
          log_status();
        } else {
          log_send_blocking("unknown command\r\n");
        }
      }
      len = 0;
      continue;
    }

    if (len + 1U < sizeof(line)) {
      line[len++] = c;
    } else {
      len = 0;
      log_send_blocking("line too long\r\n");
    }
  }
}

int main(void)
{
  bringup_phase_model_init(&bringup_phase_model);
  bringup_user_task_created = 0U;

  bringup_boot_phase_begin(BRINGUP_PHASE_ROM_EARLY_INIT);
  board_clock_init();
  board_gpio_init();
  board_uart_init();
  board_adc_init();
  enable_fpu();
  board_uart_write("MicroKernel-MPU boot\r\n");
  fault_boot_init(board_reset_flags_read());
  board_reset_flags_clear();
  fault_emit_reset_cause_human();
  if (fault_last_boot_was_fault_reboot()) {
    fault_emit_retained_human();
  }
  vm32_reset(&vm32);
  sonic_lite_init(&sonic_lite);
  bringup_boot_phase_succeed(BRINGUP_PHASE_ROM_EARLY_INIT);

  bringup_boot_phase_begin(BRINGUP_PHASE_MPU_SETUP);
  SCB->VTOR = FLASH_BASE;
  __DSB();
  __ISB();
  SCB->SHCSR |= (SCB_SHCSR_USGFAULTENA_Msk |
                 SCB_SHCSR_BUSFAULTENA_Msk |
                 SCB_SHCSR_MEMFAULTENA_Msk);

#if APP_DISABLE_MPU
  board_uart_write("MPU disabled\r\n");
  MPU->CTRL = 0;
  __DSB();
  __ISB();
#endif
  bringup_boot_phase_succeed(BRINGUP_PHASE_MPU_SETUP);

  bringup_boot_phase_begin(BRINGUP_PHASE_KERNEL_START);
  for (uint32_t i = 0; i < SHARED_ADC_SAMPLES; ++i) {
    g_shared_adc.samples[i] = 0;
  }
  g_shared_adc.seq = 0;
  g_shared_ctrl.sample_period_ms = 100;
  g_shared_ctrl.log_period_ms = 1000;
  g_shared_ctrl.logging_enabled = 0;
  g_shared_ctrl.cli_ready = 0;
  g_shared_ctrl.log_mode = 2;
  g_shared_ctrl.alarm_mv = 0;
  g_shared_ctrl.alarm_enabled = 0;
  g_shared_ctrl.snapshot_req = 0;
  g_shared_ctrl.build_id = 0x20260204U;
  g_shared_ctrl.mv_scale_uV = 3300000U;
  g_shared_ctrl.adc_full_scale = 4095U;
  g_shared_ctrl.cfg_flags = 0U;
  g_shared_ctrl.ipc_cmd_q = 0U;
  g_shared_ctrl.ipc_resp_q = 0U;
  g_shared_ctrl.stats_head = 0;
  g_shared_ctrl.stats_count = 0;

  for (int i = 0; i < 3; ++i) {
    board_led_toggle();
    board_uart_write("Pre-RTOS\r\n");
    board_delay_ms(200);
  }

  vMPUClearKernelObjectPool();
  void *heap_probe = pvPortMalloc(16);
  if (heap_probe == NULL) {
    board_uart_write("Heap init failed\r\n");
  } else {
    vPortFree(heap_probe);
  }

  {
    char buf[64];
    snprintf(buf, sizeof(buf), "Heap free=%lu\r\n",
             (unsigned long)xPortGetFreeHeapSize());
    board_uart_write(buf);
  }
  bringup_boot_phase_succeed(BRINGUP_PHASE_KERNEL_START);

  if (!kdi_bootstrap_contracts()) {
    board_uart_write("kdi bootstrap failed\r\n");
    for (;;) {
    }
  }
  analysis_engine_init_rule_based(&snapshot_analysis_engine);
  driver_profile_reset_runtime();

  bringup_boot_phase_begin(BRINGUP_PHASE_SERVICE_REGISTRATION);
  board_uart_write("Create log queue...\r\n");
  log_queue = xQueueCreate(LOG_QUEUE_LEN, sizeof(LogMsg));
  if (log_queue == NULL) {
    bringup_boot_phase_fail_halt(BRINGUP_PHASE_SERVICE_REGISTRATION, -2502, "Log queue create failed\r\n");
  }
  board_uart_write("Log queue OK\r\n");

  board_uart_write("Create IPC cmd queue...\r\n");
  ipc_cmd_queue = xQueueCreate(8, sizeof(IpcCommand));
  if (ipc_cmd_queue == NULL) {
    bringup_boot_phase_fail_halt(BRINGUP_PHASE_SERVICE_REGISTRATION, -2503, "IPC cmd queue create failed\r\n");
  }
  board_uart_write("IPC cmd queue OK\r\n");

  board_uart_write("Create IPC resp queue...\r\n");
  ipc_resp_queue = xQueueCreate(8, sizeof(IpcResponse));
  if (ipc_resp_queue == NULL) {
    bringup_boot_phase_fail_halt(BRINGUP_PHASE_SERVICE_REGISTRATION, -2504, "IPC resp queue create failed\r\n");
  }
  board_uart_write("IPC resp queue OK\r\n");

  g_shared_ctrl.ipc_cmd_q = (uint32_t)(uintptr_t)ipc_cmd_queue;
  g_shared_ctrl.ipc_resp_q = (uint32_t)(uintptr_t)ipc_resp_queue;

  board_uart_write("Create log task...\r\n");
  if (xTaskCreate(log_task, "log", 256, NULL, (2 | portPRIVILEGE_BIT), NULL) != pdPASS) {
    bringup_boot_phase_fail_halt(BRINGUP_PHASE_SERVICE_REGISTRATION, -2505, "Log task create failed\r\n");
  }
  board_uart_write("Log task OK\r\n");
  board_uart_write("Create heartbeat task...\r\n");
  if (xTaskCreate(heartbeat_task, "hb", 256, NULL, (1 | portPRIVILEGE_BIT), NULL) != pdPASS) {
    bringup_boot_phase_fail_halt(BRINGUP_PHASE_SERVICE_REGISTRATION, -2506, "Task create failed\r\n");
  }
  board_uart_write("Heartbeat task OK\r\n");
  board_uart_write("Create sensor task...\r\n");
  if (xTaskCreate(sensor_task, "adc", 256, NULL, (2 | portPRIVILEGE_BIT), NULL) != pdPASS) {
    bringup_boot_phase_fail_halt(BRINGUP_PHASE_SERVICE_REGISTRATION, -2507, "Sensor task create failed\r\n");
  }
  board_uart_write("Sensor task OK\r\n");

  board_uart_write("Create CLI task...\r\n");
  if (xTaskCreate(cli_task, "cli", CLI_STACK_WORDS, NULL, (1 | portPRIVILEGE_BIT), NULL) != pdPASS) {
    bringup_boot_phase_fail_halt(BRINGUP_PHASE_SERVICE_REGISTRATION, -2508, "CLI task create failed\r\n");
  }
  board_uart_write("CLI task OK\r\n");
  bringup_boot_phase_succeed(BRINGUP_PHASE_SERVICE_REGISTRATION);

  bringup_boot_phase_begin(BRINGUP_PHASE_USER_WORKLOAD_ENABLE);
  TaskHandle_t user_handle = NULL;
  TaskParameters_t user_params = {
    .pvTaskCode = mpu_user_task,
    .pcName = "user",
    .usStackDepth = USER_STACK_WORDS,
    .pvParameters = log_queue,
    .uxPriority = 1,
    .puxStackBuffer = user_stack,
    .xRegions = {
      { (void *)&g_shared_adc, SHARED_ADC_REGION_SIZE, portMPU_REGION_READ_ONLY | portMPU_REGION_EXECUTE_NEVER },
      { (void *)&g_shared_ctrl, SHARED_CTRL_REGION_SIZE, portMPU_REGION_READ_WRITE | portMPU_REGION_EXECUTE_NEVER },
      { (void *)&g_shared_stats, STATS_BUF_REGION_SIZE, portMPU_REGION_READ_WRITE | portMPU_REGION_EXECUTE_NEVER }
    }
  };
  if (xTaskCreateRestricted(&user_params, &user_handle) != pdPASS) {
    board_uart_write("User task create failed\r\n");
    bringup_boot_phase_fail(BRINGUP_PHASE_USER_WORKLOAD_ENABLE, -2602);
  }
  if (user_handle != NULL) {
    bringup_user_task_created = 1U;
    vGrantAccessToQueue(user_handle, log_queue);
    vGrantAccessToQueue(user_handle, ipc_cmd_queue);
    vGrantAccessToQueue(user_handle, ipc_resp_queue);
  }

  board_uart_write("Start scheduler\r\n");
  vTaskStartScheduler();
  bringup_boot_phase_fail(BRINGUP_PHASE_USER_WORKLOAD_ENABLE, -2603);

  while (1) {
    board_uart_write("Scheduler failed\r\n");
    board_delay_ms(1000);
  }
}
