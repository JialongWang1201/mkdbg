#ifndef MPU_DEMO_H
#define MPU_DEMO_H

#include <stddef.h>
#include <stdint.h>

#define LOG_MSG_LEN 64

typedef struct {
  char text[LOG_MSG_LEN];
} LogMsg;

#define SHARED_ADC_SAMPLES 64U
#define SHARED_ADC_REGION_SIZE 256U
#define SHARED_ADC_PAD (SHARED_ADC_REGION_SIZE - (SHARED_ADC_SAMPLES * sizeof(uint16_t) + sizeof(uint32_t)))

#define SHARED_CTRL_REGION_SIZE 64U
#define SHARED_CTRL_FIELDS_SIZE (8U * sizeof(uint32_t) + 3U * sizeof(uint16_t) + 5U * sizeof(uint8_t))
#define SHARED_CTRL_PAD (SHARED_CTRL_REGION_SIZE - SHARED_CTRL_FIELDS_SIZE)

#define STATS_BUF_LEN 256U
#define STATS_BUF_REGION_SIZE 4096U

typedef struct {
  volatile uint16_t samples[SHARED_ADC_SAMPLES];
  volatile uint32_t seq;
  uint8_t _pad[SHARED_ADC_PAD];
} SharedAdcData;

typedef struct {
  volatile uint32_t sample_period_ms;
  volatile uint32_t log_period_ms;
  volatile uint16_t alarm_mv;
  volatile uint8_t logging_enabled;
  volatile uint8_t log_mode;
  volatile uint8_t alarm_enabled;
  volatile uint8_t snapshot_req;
  volatile uint8_t cli_ready;
  volatile uint32_t build_id;
  volatile uint32_t mv_scale_uV;
  volatile uint16_t adc_full_scale;
  volatile uint16_t cfg_flags;
  volatile uint32_t ipc_cmd_q;
  volatile uint32_t ipc_resp_q;
  volatile uint32_t stats_head;
  volatile uint32_t stats_count;
  uint8_t _pad[SHARED_CTRL_PAD];
} SharedControl;

typedef enum {
  IPC_CMD_PING = 1,
  IPC_CMD_SNAPSHOT = 2,
  IPC_CMD_SET_RATE = 3,
  IPC_CMD_SET_LOG = 4,
  IPC_CMD_SET_MODE = 5,
  IPC_CMD_SET_ALARM = 6,
  IPC_CMD_ALARM_OFF = 7
} IpcCommandId;

typedef struct {
  uint8_t cmd;
  uint8_t arg;
  uint16_t reserved;
  uint32_t value;
} IpcCommand;

typedef struct {
  uint8_t cmd;
  uint8_t status;
  uint16_t reserved;
  uint32_t value;
} IpcResponse;

extern volatile SharedAdcData g_shared_adc;
extern volatile SharedControl g_shared_ctrl;
extern volatile uint8_t g_shared_stats[STATS_BUF_REGION_SIZE];

#define g_sample_period_ms (g_shared_ctrl.sample_period_ms)
#define g_log_period_ms (g_shared_ctrl.log_period_ms)
#define g_logging_enabled (g_shared_ctrl.logging_enabled)
#define g_cli_ready (g_shared_ctrl.cli_ready)
#define g_log_mode (g_shared_ctrl.log_mode)
#define g_alarm_mv (g_shared_ctrl.alarm_mv)
#define g_alarm_enabled (g_shared_ctrl.alarm_enabled)
#define g_snapshot_req (g_shared_ctrl.snapshot_req)

#define CFG_FLAG_SCENARIO_MPU_FAULT (1U << 0)

void mpu_stats_dump(void *queue, uint32_t max_rows);
void mpu_stats_clear(void);
uint32_t mpu_stats_count(void);

void mpu_user_task(void *arg);
void mpu_fault_task(void *arg);

#endif
