#include "mpu_demo.h"
#include "stm32f4xx.h"
#include "FreeRTOS.h"
#include "mpu_wrappers.h"
#include "task.h"
#include "queue.h"
#include "static_assert.h"
#include <string.h>

#define STATS_BUF_MASK (STATS_BUF_LEN - 1U)

typedef struct {
  uint32_t seq;
  uint16_t avg;
  uint16_t min;
  uint16_t max;
  uint16_t mv;
  uint32_t tick;
} StatsSample;

static volatile StatsSample *const stats_buf = (volatile StatsSample *)g_shared_stats;

STATIC_ASSERT((STATS_BUF_LEN & STATS_BUF_MASK) == 0U, stats_buf_pow2);

uint32_t mpu_stats_count(void) PRIVILEGED_FUNCTION;
void mpu_stats_clear(void) PRIVILEGED_FUNCTION;
void mpu_stats_dump(void *queue_ptr, uint32_t max_rows) PRIVILEGED_FUNCTION;

static void log_send(QueueHandle_t queue, const char *msg)
{
  LogMsg out = {0};
  size_t i = 0;

  /* Unprivileged tasks should log via the queue, not UART directly. */
  if (queue == NULL || msg == NULL) {
    return;
  }

  do {
    memset(&out, 0, sizeof(out));
    i = 0U;
    for (; i < (LOG_MSG_LEN - 1U) && msg[i] != '\0'; ++i) {
      out.text[i] = msg[i];
    }
    out.text[i] = '\0';
    (void)xQueueSend(queue, &out, portMAX_DELAY);
    msg += i;
  } while (i == (LOG_MSG_LEN - 1U) && msg[0] != '\0');
}

static size_t append_str(char *dst, size_t max, size_t idx, const char *src)
{
  if (idx >= max) {
    return idx;
  }
  while (*src && idx + 1 < max) {
    dst[idx++] = *src++;
  }
  dst[idx] = '\0';
  return idx;
}

static size_t append_u32(char *dst, size_t max, size_t idx, uint32_t value)
{
  char tmp[11];
  size_t len = 0;

  if (value == 0) {
    if (idx + 1 < max) {
      dst[idx++] = '0';
      dst[idx] = '\0';
    }
    return idx;
  }

  while (value > 0 && len < sizeof(tmp)) {
    tmp[len++] = (char)('0' + (value % 10U));
    value /= 10U;
  }

  while (len > 0 && idx + 1 < max) {
    dst[idx++] = tmp[--len];
  }
  dst[idx] = '\0';
  return idx;
}

static void stats_record(uint32_t seq,
                         uint16_t avg,
                         uint16_t min,
                         uint16_t max,
                         uint16_t mv)
{
  uint32_t head = g_shared_ctrl.stats_head;
  StatsSample sample = {
    .seq = seq,
    .avg = avg,
    .min = min,
    .max = max,
    .mv = mv,
    .tick = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS),
  };

  stats_buf[head] = sample;
  g_shared_ctrl.stats_head = (head + 1U) & STATS_BUF_MASK;
  if (g_shared_ctrl.stats_count < STATS_BUF_LEN) {
    g_shared_ctrl.stats_count++;
  }
}

uint32_t mpu_stats_count(void)
{
  uint32_t count;
  taskENTER_CRITICAL();
  count = g_shared_ctrl.stats_count;
  taskEXIT_CRITICAL();
  return count;
}

void mpu_stats_clear(void)
{
  taskENTER_CRITICAL();
  g_shared_ctrl.stats_head = 0;
  g_shared_ctrl.stats_count = 0;
  taskEXIT_CRITICAL();
}

void mpu_stats_dump(void *queue_ptr, uint32_t max_rows)
{
  QueueHandle_t queue = (QueueHandle_t)queue_ptr;
  uint32_t count;
  uint32_t head;

  taskENTER_CRITICAL();
  count = g_shared_ctrl.stats_count;
  head = g_shared_ctrl.stats_head;
  taskEXIT_CRITICAL();

  if (count == 0U) {
    log_send(queue, "stats: empty\r\n");
    return;
  }

  if (max_rows != 0U && max_rows < count) {
    count = max_rows;
  }

  uint32_t start = (head + STATS_BUF_LEN - count) & STATS_BUF_MASK;
  log_send(queue, "seq,avg,min,max,mv,tick_ms\r\n");

  for (uint32_t i = 0; i < count; ++i) {
    uint32_t idx = (start + i) & STATS_BUF_MASK;
    StatsSample sample;
    char buf[96];
    size_t bidx = 0;

    taskENTER_CRITICAL();
    sample = stats_buf[idx];
    taskEXIT_CRITICAL();

    buf[0] = '\0';
    bidx = append_u32(buf, sizeof(buf), bidx, sample.seq);
    bidx = append_str(buf, sizeof(buf), bidx, ",");
    bidx = append_u32(buf, sizeof(buf), bidx, sample.avg);
    bidx = append_str(buf, sizeof(buf), bidx, ",");
    bidx = append_u32(buf, sizeof(buf), bidx, sample.min);
    bidx = append_str(buf, sizeof(buf), bidx, ",");
    bidx = append_u32(buf, sizeof(buf), bidx, sample.max);
    bidx = append_str(buf, sizeof(buf), bidx, ",");
    bidx = append_u32(buf, sizeof(buf), bidx, sample.mv);
    bidx = append_str(buf, sizeof(buf), bidx, ",");
    bidx = append_u32(buf, sizeof(buf), bidx, sample.tick);
    bidx = append_str(buf, sizeof(buf), bidx, "\r\n");
    (void)bidx;
    log_send(queue, buf);
  }
}

static void ipc_send_response(QueueHandle_t resp_q, uint8_t cmd, uint8_t status, uint32_t value)
{
  if (resp_q == NULL) {
    return;
  }
  IpcResponse resp = {
    .cmd = cmd,
    .status = status,
    .value = value
  };
  (void)xQueueSend(resp_q, &resp, 0);
}

static void ipc_handle_command(const IpcCommand *cmd, QueueHandle_t resp_q, uint32_t last_avg, uint32_t last_seq)
{
  if (cmd == NULL) {
    return;
  }

  switch ((IpcCommandId)cmd->cmd) {
    case IPC_CMD_PING:
      ipc_send_response(resp_q, cmd->cmd, 0U, last_seq);
      break;
    case IPC_CMD_SNAPSHOT:
      g_snapshot_req = 1;
      ipc_send_response(resp_q, cmd->cmd, 0U, last_avg);
      break;
    case IPC_CMD_SET_RATE:
      if (cmd->value >= 10U && cmd->value <= 5000U) {
        g_sample_period_ms = cmd->value;
        ipc_send_response(resp_q, cmd->cmd, 0U, cmd->value);
      } else {
        ipc_send_response(resp_q, cmd->cmd, 1U, cmd->value);
      }
      break;
    case IPC_CMD_SET_LOG:
      if (cmd->value >= 100U && cmd->value <= 10000U) {
        g_log_period_ms = cmd->value;
        ipc_send_response(resp_q, cmd->cmd, 0U, cmd->value);
      } else {
        ipc_send_response(resp_q, cmd->cmd, 1U, cmd->value);
      }
      break;
    case IPC_CMD_SET_MODE:
      if (cmd->arg <= 2U) {
        g_log_mode = cmd->arg;
        ipc_send_response(resp_q, cmd->cmd, 0U, cmd->arg);
      } else {
        ipc_send_response(resp_q, cmd->cmd, 1U, cmd->arg);
      }
      break;
    case IPC_CMD_SET_ALARM:
      if (cmd->value >= 100U && cmd->value <= 3300U) {
        g_alarm_mv = (uint16_t)cmd->value;
        g_alarm_enabled = 1;
        ipc_send_response(resp_q, cmd->cmd, 0U, cmd->value);
      } else {
        ipc_send_response(resp_q, cmd->cmd, 1U, cmd->value);
      }
      break;
    case IPC_CMD_ALARM_OFF:
      g_alarm_enabled = 0;
      g_alarm_mv = 0;
      ipc_send_response(resp_q, cmd->cmd, 0U, 0U);
      break;
    default:
      ipc_send_response(resp_q, cmd->cmd, 1U, 0U);
      break;
  }
}

void mpu_user_task(void *arg)
{
  QueueHandle_t queue = (QueueHandle_t)arg;
  QueueHandle_t cmd_q = (QueueHandle_t)(uintptr_t)g_shared_ctrl.ipc_cmd_q;
  QueueHandle_t resp_q = (QueueHandle_t)(uintptr_t)g_shared_ctrl.ipc_resp_q;

  while (g_cli_ready == 0U) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  log_send(queue, "User task: start\r\n");
  log_send(queue, "MPU demo: unprivileged task running\r\n");

  uint32_t last_seq = 0;
  uint32_t last_avg = 0;
  for (;;) {
    IpcCommand cmd;
    while (cmd_q != NULL && xQueueReceive(cmd_q, &cmd, 0) == pdPASS) {
      ipc_handle_command(&cmd, resp_q, last_avg, last_seq);
    }

    if (g_shared_ctrl.cfg_flags & CFG_FLAG_SCENARIO_MPU_FAULT) {
      g_shared_ctrl.cfg_flags &= ~CFG_FLAG_SCENARIO_MPU_FAULT;
      log_send(queue, "Scenario: mpu_overflow\r\n");
      /* Deliberately write to a read-only shared region to trigger MemManage. */
      g_shared_adc.samples[0] ^= 1U;
    }

    uint32_t seq = g_shared_adc.seq;
    uint32_t sum = 0;
    uint16_t min = 0xFFFF;
    uint16_t max = 0;
    for (uint32_t i = 0; i < SHARED_ADC_SAMPLES; ++i) {
      uint16_t sample = g_shared_adc.samples[i];
      sum += sample;
      if (sample < min) {
        min = sample;
      }
      if (sample > max) {
        max = sample;
      }
    }
    uint32_t avg = sum / SHARED_ADC_SAMPLES;
    uint32_t scale_uV = g_shared_ctrl.mv_scale_uV ? g_shared_ctrl.mv_scale_uV : 3300000U;
    uint32_t full_scale = g_shared_ctrl.adc_full_scale ? g_shared_ctrl.adc_full_scale : 4095U;
    uint64_t uv = ((uint64_t)avg * (uint64_t)scale_uV) / (uint64_t)full_scale;
    uint32_t mv = (uint32_t)(uv / 1000U);
    last_avg = avg;

    if (seq != last_seq) {
      stats_record(seq, (uint16_t)avg, min, max, (uint16_t)mv);
    }

    if (g_alarm_enabled && mv >= g_alarm_mv) {
      log_send(queue, "ALARM: mv threshold exceeded\r\n");
    }

    if (g_snapshot_req) {
      char snap[128];
      size_t sidx = 0;
      snap[0] = '\0';
      sidx = append_str(snap, sizeof(snap), sidx, "snap avg=");
      sidx = append_u32(snap, sizeof(snap), sidx, avg);
      sidx = append_str(snap, sizeof(snap), sidx, " min=");
      sidx = append_u32(snap, sizeof(snap), sidx, min);
      sidx = append_str(snap, sizeof(snap), sidx, " max=");
      sidx = append_u32(snap, sizeof(snap), sidx, max);
      sidx = append_str(snap, sizeof(snap), sidx, " mv=");
      sidx = append_u32(snap, sizeof(snap), sidx, mv);
      sidx = append_str(snap, sizeof(snap), sidx, " seq=");
      sidx = append_u32(snap, sizeof(snap), sidx, seq);
      sidx = append_str(snap, sizeof(snap), sidx, "\r\n");
      (void)sidx;
      log_send(queue, snap);
      g_snapshot_req = 0;
    }

    if (seq != last_seq && g_logging_enabled && (g_log_mode == 1 || g_log_mode == 2)) {
      char buf[64];
      size_t idx = 0;
      buf[0] = '\0';
      idx = append_str(buf, sizeof(buf), idx, "ADC avg=");
      idx = append_u32(buf, sizeof(buf), idx, avg);
      idx = append_str(buf, sizeof(buf), idx, " min=");
      idx = append_u32(buf, sizeof(buf), idx, min);
      idx = append_str(buf, sizeof(buf), idx, " max=");
      idx = append_u32(buf, sizeof(buf), idx, max);
      idx = append_str(buf, sizeof(buf), idx, " mv=");
      idx = append_u32(buf, sizeof(buf), idx, mv);
      idx = append_str(buf, sizeof(buf), idx, " seq=");
      idx = append_u32(buf, sizeof(buf), idx, seq);
      idx = append_str(buf, sizeof(buf), idx, "\r\n");
      (void)idx;
      log_send(queue, buf);
      last_seq = seq;
    }
    vTaskDelay(pdMS_TO_TICKS(g_log_period_ms));
  }
}

void mpu_fault_task(void *arg)
{
  QueueHandle_t queue = (QueueHandle_t)arg;

  log_send(queue, "Fault task: start\r\n");
  vTaskDelay(pdMS_TO_TICKS(2000));
  log_send(queue, "Fault task: touching GPIO (should fault)\r\n");

  // Access to peripherals should fault in unprivileged tasks.
  GPIOA->ODR ^= (1U << 5);

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
