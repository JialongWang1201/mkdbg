#ifndef BRINGUP_PHASE_H
#define BRINGUP_PHASE_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
  BRINGUP_PHASE_ROM_EARLY_INIT = 0,
  BRINGUP_PHASE_MPU_SETUP = 1,
  BRINGUP_PHASE_KERNEL_START = 2,
  BRINGUP_PHASE_DRIVER_PROBE_DIAG = 3,
  BRINGUP_PHASE_DRIVER_PROBE = BRINGUP_PHASE_DRIVER_PROBE_DIAG, /* compatibility alias */
  BRINGUP_PHASE_DRIVER_PROBE_UART = 4,
  BRINGUP_PHASE_DRIVER_PROBE_SENSOR = 5,
  BRINGUP_PHASE_DRIVER_PROBE_VM = 6,
  BRINGUP_PHASE_SERVICE_REGISTRATION = 7,
  BRINGUP_PHASE_USER_WORKLOAD_ENABLE = 8,
  BRINGUP_PHASE_COUNT = 9
} BringupPhaseId;

typedef enum {
  BRINGUP_STAGE_INIT = 0,
  BRINGUP_STAGE_MPU = 1,
  BRINGUP_STAGE_KERNEL = 2,
  BRINGUP_STAGE_DRIVERS = 3,
  BRINGUP_STAGE_READY = 4,
  BRINGUP_STAGE_COUNT = 5
} BringupStageId;

typedef enum {
  BRINGUP_PHASE_STATUS_PENDING = 0,
  BRINGUP_PHASE_STATUS_RUNNING = 1,
  BRINGUP_PHASE_STATUS_DONE = 2,
  BRINGUP_PHASE_STATUS_FAILED = 3,
  BRINGUP_PHASE_STATUS_ROLLED_BACK = 4
} BringupPhaseStatus;

typedef struct {
  BringupPhaseStatus status;
  uint8_t reserved0;
  uint8_t reserved1;
  uint8_t reserved2;
  uint32_t enter_seq;
  uint32_t leave_seq;
  uint32_t attempts;
  uint32_t fail_count;
  uint32_t rollback_count;
  int32_t last_error;
} BringupPhaseSlot;

typedef struct {
  BringupPhaseStatus status;
  uint8_t reserved0;
  uint8_t reserved1;
  uint8_t reserved2;
  uint32_t enter_seq;
  uint32_t leave_seq;
  uint32_t attempts;
  uint32_t fail_count;
  uint32_t rollback_count;
  int32_t last_error;
} BringupStageSlot;

typedef struct {
  BringupPhaseSlot slots[BRINGUP_PHASE_COUNT];
  uint32_t transition_seq;
  int32_t last_error;
  uint32_t inject_mask;
  int32_t inject_error[BRINGUP_PHASE_COUNT];
  uint8_t active_valid;
  uint8_t active_phase;
  uint8_t boot_complete;
  uint8_t reserved0;
} BringupPhaseModel;

void bringup_phase_model_init(BringupPhaseModel *model);
void bringup_phase_reset_execution(BringupPhaseModel *model);

const char *bringup_phase_name(BringupPhaseId phase);
const char *bringup_phase_status_name(BringupPhaseStatus status);
int bringup_phase_parse_name(const char *text, BringupPhaseId *out);
BringupStageId bringup_stage_from_phase(BringupPhaseId phase);
const char *bringup_stage_name(BringupStageId stage);
int bringup_stage_parse_name(const char *text, BringupStageId *out);
const char *bringup_stage_entry_event(BringupStageId stage);
const char *bringup_stage_exit_event(BringupStageId stage);

int bringup_phase_begin(BringupPhaseModel *model, BringupPhaseId phase);
int bringup_phase_succeed(BringupPhaseModel *model, BringupPhaseId phase);
int bringup_phase_fail(BringupPhaseModel *model, BringupPhaseId phase, int32_t error_code);
uint32_t bringup_phase_rollback_from(BringupPhaseModel *model, BringupPhaseId phase);

int bringup_phase_set_injected_failure(BringupPhaseModel *model,
                                       BringupPhaseId phase,
                                       int32_t error_code);
void bringup_phase_clear_injected_failure(BringupPhaseModel *model, BringupPhaseId phase);
void bringup_phase_clear_all_injected_failures(BringupPhaseModel *model);
int bringup_phase_consume_injected_failure(BringupPhaseModel *model,
                                           BringupPhaseId phase,
                                           int32_t *out_error_code);

int bringup_phase_all_done(const BringupPhaseModel *model);
int bringup_phase_get_slot(const BringupPhaseModel *model,
                           BringupPhaseId phase,
                           BringupPhaseSlot *out_slot);
int bringup_stage_get_slot(const BringupPhaseModel *model,
                           BringupStageId stage,
                           BringupStageSlot *out_slot);
int bringup_stage_current(const BringupPhaseModel *model,
                          BringupStageId *out_stage,
                          BringupPhaseStatus *out_status,
                          BringupPhaseId *out_phase);

#endif
