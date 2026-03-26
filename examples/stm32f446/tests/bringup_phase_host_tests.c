#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bringup_phase.h"

static int failures = 0;

static void check(int cond, const char *msg)
{
  if (!cond) {
    printf("FAIL: %s\n", msg);
    failures++;
  }
}

static void check_status(BringupPhaseModel *model,
                         BringupPhaseId phase,
                         BringupPhaseStatus expect,
                         const char *msg)
{
  BringupPhaseSlot slot = {0};

  check(bringup_phase_get_slot(model, phase, &slot) == 1, "get slot");
  check(slot.status == expect, msg);
}

static void run_all_success(BringupPhaseModel *model)
{
  BringupPhaseId p;

  for (p = BRINGUP_PHASE_ROM_EARLY_INIT; p < BRINGUP_PHASE_COUNT; ++p) {
    check(bringup_phase_begin(model, p) == 0, "begin phase");
    check(bringup_phase_succeed(model, p) == 0, "succeed phase");
  }
}

static void test_init_and_names(void)
{
  BringupPhaseModel model;
  BringupPhaseId parsed = BRINGUP_PHASE_COUNT;
  BringupPhaseSlot slot = {0};
  uint32_t i;

  bringup_phase_model_init(&model);
  check(model.active_valid == 0U, "model init no active phase");
  check(model.boot_complete == 0U, "model init not complete");
  for (i = 0U; i < (uint32_t)BRINGUP_PHASE_COUNT; ++i) {
    check(bringup_phase_get_slot(&model, (BringupPhaseId)i, &slot) == 1, "slot readable");
    check(slot.status == BRINGUP_PHASE_STATUS_PENDING, "all slots pending after init");
  }

  check(bringup_phase_parse_name("rom-early-init", &parsed) == 1 &&
        parsed == BRINGUP_PHASE_ROM_EARLY_INIT, "parse long phase name");
  check(bringup_phase_parse_name("mpu", &parsed) == 1 &&
        parsed == BRINGUP_PHASE_MPU_SETUP, "parse alias");
  check(bringup_phase_parse_name("nope", &parsed) == 0, "parse invalid phase rejected");
  check(bringup_phase_name(BRINGUP_PHASE_DRIVER_PROBE) != NULL, "phase name exists");
  check(bringup_phase_status_name(BRINGUP_PHASE_STATUS_ROLLED_BACK) != NULL, "status name exists");
}

static void test_order_and_success_path(void)
{
  BringupPhaseModel model;

  bringup_phase_model_init(&model);

  check(bringup_phase_begin(&model, BRINGUP_PHASE_MPU_SETUP) == -2, "order check blocks skipping rom");
  check(bringup_phase_begin(&model, BRINGUP_PHASE_ROM_EARLY_INIT) == 0, "begin rom");
  check(bringup_phase_begin(&model, BRINGUP_PHASE_MPU_SETUP) == -3, "cannot start second active phase");
  check(bringup_phase_succeed(&model, BRINGUP_PHASE_ROM_EARLY_INIT) == 0, "rom done");

  run_all_success(&model);
  check(bringup_phase_all_done(&model) == 1, "all phases done");
  check(model.boot_complete == 1U, "boot_complete set");
  check_status(&model, BRINGUP_PHASE_USER_WORKLOAD_ENABLE, BRINGUP_PHASE_STATUS_DONE, "last phase done");
}

static void test_fail_rollback_and_retry(void)
{
  BringupPhaseModel model;
  BringupPhaseSlot slot = {0};

  bringup_phase_model_init(&model);
  check(bringup_phase_begin(&model, BRINGUP_PHASE_ROM_EARLY_INIT) == 0, "rom begin");
  check(bringup_phase_succeed(&model, BRINGUP_PHASE_ROM_EARLY_INIT) == 0, "rom done");
  check(bringup_phase_begin(&model, BRINGUP_PHASE_MPU_SETUP) == 0, "mpu begin");
  check(bringup_phase_succeed(&model, BRINGUP_PHASE_MPU_SETUP) == 0, "mpu done");

  check(bringup_phase_begin(&model, BRINGUP_PHASE_KERNEL_START) == 0, "kernel begin");
  check(bringup_phase_fail(&model, BRINGUP_PHASE_KERNEL_START, -42) == 0, "kernel fail");
  check_status(&model, BRINGUP_PHASE_KERNEL_START, BRINGUP_PHASE_STATUS_FAILED, "kernel failed");

  check(bringup_phase_get_slot(&model, BRINGUP_PHASE_KERNEL_START, &slot) == 1, "kernel slot query");
  check(slot.fail_count == 1U, "kernel fail count");
  check(slot.last_error == -42, "kernel last error");
  check(bringup_phase_rollback_from(&model, BRINGUP_PHASE_KERNEL_START) >= 1U, "rollback marks failed phase");
  check_status(&model, BRINGUP_PHASE_KERNEL_START, BRINGUP_PHASE_STATUS_ROLLED_BACK, "kernel rolled back");

  check(bringup_phase_begin(&model, BRINGUP_PHASE_KERNEL_START) == 0, "kernel retry begin");
  check(bringup_phase_succeed(&model, BRINGUP_PHASE_KERNEL_START) == 0, "kernel retry done");
  check(bringup_phase_begin(&model, BRINGUP_PHASE_DRIVER_PROBE_DIAG) == 0, "driver diag begin");
  check(bringup_phase_succeed(&model, BRINGUP_PHASE_DRIVER_PROBE_DIAG) == 0, "driver diag done");
  check(bringup_phase_begin(&model, BRINGUP_PHASE_DRIVER_PROBE_UART) == 0, "driver uart begin");
  check(bringup_phase_succeed(&model, BRINGUP_PHASE_DRIVER_PROBE_UART) == 0, "driver uart done");
  check(bringup_phase_begin(&model, BRINGUP_PHASE_DRIVER_PROBE_SENSOR) == 0, "driver sensor begin");
  check(bringup_phase_succeed(&model, BRINGUP_PHASE_DRIVER_PROBE_SENSOR) == 0, "driver sensor done");
  check(bringup_phase_begin(&model, BRINGUP_PHASE_DRIVER_PROBE_VM) == 0, "driver vm begin");
  check(bringup_phase_succeed(&model, BRINGUP_PHASE_DRIVER_PROBE_VM) == 0, "driver vm done");
  check(bringup_phase_begin(&model, BRINGUP_PHASE_SERVICE_REGISTRATION) == 0, "service begin");
  check(bringup_phase_succeed(&model, BRINGUP_PHASE_SERVICE_REGISTRATION) == 0, "service done");
  check(bringup_phase_begin(&model, BRINGUP_PHASE_USER_WORKLOAD_ENABLE) == 0, "user begin");
  check(bringup_phase_succeed(&model, BRINGUP_PHASE_USER_WORKLOAD_ENABLE) == 0, "user done");
  check(bringup_phase_all_done(&model) == 1, "all done after retry");
}

static void test_injected_failure_one_shot(void)
{
  BringupPhaseModel model;
  int32_t code = 0;

  bringup_phase_model_init(&model);
  check(bringup_phase_set_injected_failure(&model, BRINGUP_PHASE_DRIVER_PROBE, -777) == 0,
        "set injected failure");
  check(bringup_phase_consume_injected_failure(&model, BRINGUP_PHASE_DRIVER_PROBE, &code) == 1,
        "consume injected failure");
  check(code == -777, "injected error code returned");
  check(bringup_phase_consume_injected_failure(&model, BRINGUP_PHASE_DRIVER_PROBE, &code) == 0,
        "injected failure consumed once");

  check(bringup_phase_set_injected_failure(&model, BRINGUP_PHASE_SERVICE_REGISTRATION, 0) == 0,
        "set default injected failure");
  bringup_phase_clear_injected_failure(&model, BRINGUP_PHASE_SERVICE_REGISTRATION);
  check(bringup_phase_consume_injected_failure(&model, BRINGUP_PHASE_SERVICE_REGISTRATION, &code) == 0,
        "clear injected failure");
}

static void test_stage_model_projection(void)
{
  BringupPhaseModel model;
  BringupStageId stage = BRINGUP_STAGE_COUNT;
  BringupPhaseStatus status = BRINGUP_PHASE_STATUS_PENDING;
  BringupPhaseId phase = BRINGUP_PHASE_COUNT;
  BringupStageSlot slot = {0};

  bringup_phase_model_init(&model);

  check(strcmp(bringup_stage_name(BRINGUP_STAGE_DRIVERS), "drivers") == 0, "stage name drivers");
  check(bringup_stage_parse_name("ready", &stage) == 1 &&
        stage == BRINGUP_STAGE_READY, "parse ready stage");
  check(bringup_stage_parse_name("driver", &stage) == 1 &&
        stage == BRINGUP_STAGE_DRIVERS, "parse driver alias stage");
  check(bringup_stage_parse_name("bad", &stage) == 0, "invalid stage parse rejected");
  check(strcmp(bringup_stage_entry_event(BRINGUP_STAGE_INIT), "bringup.stage.init.enter") == 0,
        "stage init entry event name");
  check(strcmp(bringup_stage_exit_event(BRINGUP_STAGE_READY), "bringup.stage.ready.exit") == 0,
        "stage ready exit event name");

  check(bringup_stage_current(&model, &stage, &status, &phase) == 1, "stage current initial");
  check(stage == BRINGUP_STAGE_INIT, "initial stage is init");
  check(status == BRINGUP_PHASE_STATUS_PENDING, "initial stage status pending");
  check(phase == BRINGUP_PHASE_ROM_EARLY_INIT, "initial phase is rom");

  check(bringup_phase_begin(&model, BRINGUP_PHASE_ROM_EARLY_INIT) == 0, "rom begin");
  check(bringup_stage_current(&model, &stage, &status, &phase) == 1, "stage current rom running");
  check(stage == BRINGUP_STAGE_INIT, "rom phase projects to init stage");
  check(status == BRINGUP_PHASE_STATUS_RUNNING, "init stage running while rom active");
  check(phase == BRINGUP_PHASE_ROM_EARLY_INIT, "phase rom while running");
  check(bringup_phase_succeed(&model, BRINGUP_PHASE_ROM_EARLY_INIT) == 0, "rom done");

  check(bringup_phase_begin(&model, BRINGUP_PHASE_MPU_SETUP) == 0, "mpu begin");
  check(bringup_stage_current(&model, &stage, &status, &phase) == 1, "stage current mpu running");
  check(stage == BRINGUP_STAGE_MPU, "mpu phase projects to mpu stage");
  check(status == BRINGUP_PHASE_STATUS_RUNNING, "mpu stage running");
  check(phase == BRINGUP_PHASE_MPU_SETUP, "phase mpu while running");
  check(bringup_phase_succeed(&model, BRINGUP_PHASE_MPU_SETUP) == 0, "mpu done");

  check(bringup_phase_begin(&model, BRINGUP_PHASE_KERNEL_START) == 0, "kernel begin");
  check(bringup_phase_succeed(&model, BRINGUP_PHASE_KERNEL_START) == 0, "kernel done");
  check(bringup_phase_begin(&model, BRINGUP_PHASE_DRIVER_PROBE_DIAG) == 0, "diag begin");
  check(bringup_phase_succeed(&model, BRINGUP_PHASE_DRIVER_PROBE_DIAG) == 0, "diag done");
  check(bringup_phase_begin(&model, BRINGUP_PHASE_DRIVER_PROBE_UART) == 0, "uart begin");
  check(bringup_phase_fail(&model, BRINGUP_PHASE_DRIVER_PROBE_UART, -77) == 0, "uart fail");

  check(bringup_stage_current(&model, &stage, &status, &phase) == 1, "stage current on failure");
  check(stage == BRINGUP_STAGE_DRIVERS, "driver failure projects to drivers stage");
  check(status == BRINGUP_PHASE_STATUS_FAILED, "drivers stage failed");
  check(phase == BRINGUP_PHASE_DRIVER_PROBE_UART, "failed phase reported");

  check(bringup_stage_get_slot(&model, BRINGUP_STAGE_DRIVERS, &slot) == 1, "drivers slot query");
  check(slot.status == BRINGUP_PHASE_STATUS_FAILED, "drivers slot failed");
  check(slot.attempts >= 2U, "drivers stage attempts aggregated");
  check(slot.fail_count >= 1U, "drivers stage fail count aggregated");
  check(slot.enter_seq != 0U && slot.leave_seq != 0U, "drivers stage has enter/leave seq");

  check(bringup_phase_rollback_from(&model, BRINGUP_PHASE_DRIVER_PROBE_DIAG) >= 1U,
        "drivers rollback");
  check(bringup_stage_get_slot(&model, BRINGUP_STAGE_DRIVERS, &slot) == 1, "drivers slot after rollback");
  check(slot.status == BRINGUP_PHASE_STATUS_ROLLED_BACK, "drivers stage rolled back");
}

int main(void)
{
  test_init_and_names();
  test_order_and_success_path();
  test_fail_rollback_and_retry();
  test_injected_failure_one_shot();
  test_stage_model_projection();

  if (failures == 0) {
    printf("bringup_phase_host_tests: PASS\n");
    return 0;
  }
  printf("bringup_phase_host_tests: FAIL (%d)\n", failures);
  return 1;
}
