#include "bringup_phase.h"

#include <string.h>

#define BRINGUP_MANIFEST_INCLUDE_PHASE_SECTION
#include "bringup_manifest_gen.h"

enum {
  BRINGUP_PHASE_OK = 0,
  BRINGUP_PHASE_ERR_BAD_ARG = -1,
  BRINGUP_PHASE_ERR_ORDER = -2,
  BRINGUP_PHASE_ERR_STATE = -3
};

static int bringup_phase_valid(BringupPhaseId phase)
{
  return (phase < BRINGUP_PHASE_COUNT) ? 1 : 0;
}

static int bringup_stage_valid(BringupStageId stage)
{
  return (stage < BRINGUP_STAGE_COUNT) ? 1 : 0;
}

BringupStageId bringup_stage_from_phase(BringupPhaseId phase)
{
  if (!bringup_phase_valid(phase)) {
    return BRINGUP_STAGE_INIT;
  }
  return bringup_manifest_phase_defs[(uint32_t)phase].stage;
}

const char *bringup_stage_name(BringupStageId stage)
{
  if (!bringup_stage_valid(stage)) {
    return "unknown";
  }
  return bringup_manifest_stage_defs[(uint32_t)stage].name;
}

int bringup_stage_parse_name(const char *text, BringupStageId *out)
{
  BringupStageId stage;

  if (text == NULL || out == NULL) {
    return 0;
  }
  for (stage = BRINGUP_STAGE_INIT; stage < BRINGUP_STAGE_COUNT; ++stage) {
    if (strcmp(text, bringup_stage_name(stage)) == 0) {
      *out = stage;
      return 1;
    }
  }
  for (uint32_t i = 0U; i < BRINGUP_MANIFEST_STAGE_ALIAS_COUNT; ++i) {
    if (strcmp(text, bringup_manifest_stage_alias_defs[i].alias) == 0) {
      *out = bringup_manifest_stage_alias_defs[i].stage;
      return 1;
    }
  }
  return 0;
}

const char *bringup_stage_entry_event(BringupStageId stage)
{
  if (!bringup_stage_valid(stage)) {
    return "bringup.stage.unknown.enter";
  }
  return bringup_manifest_stage_defs[(uint32_t)stage].entry_event;
}

const char *bringup_stage_exit_event(BringupStageId stage)
{
  if (!bringup_stage_valid(stage)) {
    return "bringup.stage.unknown.exit";
  }
  return bringup_manifest_stage_defs[(uint32_t)stage].exit_event;
}

static void bringup_stage_phase_range(BringupStageId stage, uint32_t *out_begin, uint32_t *out_end)
{
  if (out_begin == NULL || out_end == NULL) {
    return;
  }
  if (!bringup_stage_valid(stage)) {
    *out_begin = 0U;
    *out_end = 0U;
    return;
  }
  *out_begin = (uint32_t)bringup_manifest_stage_defs[(uint32_t)stage].phase_begin;
  *out_end = (uint32_t)bringup_manifest_stage_defs[(uint32_t)stage].phase_end;
}

static int32_t bringup_phase_default_inject_error(BringupPhaseId phase)
{
  return -1000 - (int32_t)phase;
}

static void bringup_phase_zero_slots(BringupPhaseModel *model)
{
  uint32_t i;

  if (model == NULL) {
    return;
  }
  for (i = 0U; i < (uint32_t)BRINGUP_PHASE_COUNT; ++i) {
    model->slots[i].status = BRINGUP_PHASE_STATUS_PENDING;
    model->slots[i].enter_seq = 0U;
    model->slots[i].leave_seq = 0U;
    model->slots[i].attempts = 0U;
    model->slots[i].fail_count = 0U;
    model->slots[i].rollback_count = 0U;
    model->slots[i].last_error = 0;
  }
  model->active_valid = 0U;
  model->active_phase = 0U;
  model->boot_complete = 0U;
  model->transition_seq = 0U;
  model->last_error = 0;
}

void bringup_phase_model_init(BringupPhaseModel *model)
{
  uint32_t i;

  if (model == NULL) {
    return;
  }
  memset(model, 0, sizeof(*model));
  bringup_phase_zero_slots(model);
  for (i = 0U; i < (uint32_t)BRINGUP_PHASE_COUNT; ++i) {
    model->inject_error[i] = bringup_phase_default_inject_error((BringupPhaseId)i);
  }
}

void bringup_phase_reset_execution(BringupPhaseModel *model)
{
  uint32_t i;
  uint32_t inject_mask = 0U;
  int32_t inject_error[BRINGUP_PHASE_COUNT];

  if (model == NULL) {
    return;
  }

  inject_mask = model->inject_mask;
  for (i = 0U; i < (uint32_t)BRINGUP_PHASE_COUNT; ++i) {
    inject_error[i] = model->inject_error[i];
  }
  bringup_phase_zero_slots(model);
  model->inject_mask = inject_mask;
  for (i = 0U; i < (uint32_t)BRINGUP_PHASE_COUNT; ++i) {
    model->inject_error[i] = inject_error[i];
  }
}

const char *bringup_phase_name(BringupPhaseId phase)
{
  if (!bringup_phase_valid(phase)) {
    return "unknown";
  }
  return bringup_manifest_phase_defs[(uint32_t)phase].name;
}

const char *bringup_phase_status_name(BringupPhaseStatus status)
{
  switch (status) {
    case BRINGUP_PHASE_STATUS_PENDING:
      return "pending";
    case BRINGUP_PHASE_STATUS_RUNNING:
      return "running";
    case BRINGUP_PHASE_STATUS_DONE:
      return "done";
    case BRINGUP_PHASE_STATUS_FAILED:
      return "failed";
    case BRINGUP_PHASE_STATUS_ROLLED_BACK:
      return "rolled_back";
    default:
      return "unknown";
  }
}

int bringup_phase_parse_name(const char *text, BringupPhaseId *out)
{
  BringupPhaseId phase;

  if (text == NULL || out == NULL) {
    return 0;
  }
  for (phase = BRINGUP_PHASE_ROM_EARLY_INIT; phase < BRINGUP_PHASE_COUNT; ++phase) {
    const char *name = bringup_phase_name(phase);
    if (strcmp(name, text) == 0) {
      *out = phase;
      return 1;
    }
  }
  for (uint32_t i = 0U; i < BRINGUP_MANIFEST_PHASE_ALIAS_COUNT; ++i) {
    if (strcmp(text, bringup_manifest_phase_alias_defs[i].alias) == 0) {
      *out = bringup_manifest_phase_alias_defs[i].phase;
      return 1;
    }
  }
  return 0;
}

int bringup_stage_get_slot(const BringupPhaseModel *model,
                           BringupStageId stage,
                           BringupStageSlot *out_slot)
{
  uint32_t begin = 0U;
  uint32_t end = 0U;
  uint32_t i;
  uint32_t last_error_seq = 0U;
  uint8_t any_running = 0U;
  uint8_t any_failed = 0U;
  uint8_t any_rolled = 0U;
  uint8_t any_pending = 0U;
  uint8_t any_done = 0U;
  uint8_t all_done = 1U;

  if (model == NULL || out_slot == NULL || !bringup_stage_valid(stage)) {
    return 0;
  }

  memset(out_slot, 0, sizeof(*out_slot));
  out_slot->status = BRINGUP_PHASE_STATUS_PENDING;
  bringup_stage_phase_range(stage, &begin, &end);

  for (i = begin; i <= end; ++i) {
    const BringupPhaseSlot *phase_slot = &model->slots[i];
    out_slot->attempts += phase_slot->attempts;
    out_slot->fail_count += phase_slot->fail_count;
    out_slot->rollback_count += phase_slot->rollback_count;

    if (phase_slot->enter_seq != 0U &&
        (out_slot->enter_seq == 0U || phase_slot->enter_seq < out_slot->enter_seq)) {
      out_slot->enter_seq = phase_slot->enter_seq;
    }
    if (phase_slot->leave_seq > out_slot->leave_seq) {
      out_slot->leave_seq = phase_slot->leave_seq;
    }
    if (phase_slot->last_error != 0) {
      uint32_t seq = (phase_slot->leave_seq != 0U) ? phase_slot->leave_seq : phase_slot->enter_seq;
      if (seq >= last_error_seq) {
        last_error_seq = seq;
        out_slot->last_error = phase_slot->last_error;
      }
    }

    switch (phase_slot->status) {
      case BRINGUP_PHASE_STATUS_RUNNING:
        any_running = 1U;
        all_done = 0U;
        break;
      case BRINGUP_PHASE_STATUS_FAILED:
        any_failed = 1U;
        all_done = 0U;
        break;
      case BRINGUP_PHASE_STATUS_ROLLED_BACK:
        any_rolled = 1U;
        all_done = 0U;
        break;
      case BRINGUP_PHASE_STATUS_PENDING:
        any_pending = 1U;
        all_done = 0U;
        break;
      case BRINGUP_PHASE_STATUS_DONE:
        any_done = 1U;
        break;
      default:
        any_pending = 1U;
        all_done = 0U;
        break;
    }
  }

  if (any_running != 0U) {
    out_slot->status = BRINGUP_PHASE_STATUS_RUNNING;
  } else if (any_failed != 0U) {
    out_slot->status = BRINGUP_PHASE_STATUS_FAILED;
  } else if (any_rolled != 0U) {
    out_slot->status = BRINGUP_PHASE_STATUS_ROLLED_BACK;
  } else if (all_done != 0U || (any_done != 0U && any_pending == 0U)) {
    out_slot->status = BRINGUP_PHASE_STATUS_DONE;
  } else if (any_done != 0U && any_pending != 0U) {
    out_slot->status = BRINGUP_PHASE_STATUS_RUNNING;
  } else {
    out_slot->status = BRINGUP_PHASE_STATUS_PENDING;
  }

  return 1;
}

int bringup_stage_current(const BringupPhaseModel *model,
                          BringupStageId *out_stage,
                          BringupPhaseStatus *out_status,
                          BringupPhaseId *out_phase)
{
  BringupStageId stage = BRINGUP_STAGE_INIT;
  BringupPhaseStatus status = BRINGUP_PHASE_STATUS_PENDING;
  BringupPhaseId phase = BRINGUP_PHASE_ROM_EARLY_INIT;
  uint32_t i;

  if (model == NULL) {
    return 0;
  }

  if (model->active_valid != 0U && bringup_phase_valid((BringupPhaseId)model->active_phase)) {
    phase = (BringupPhaseId)model->active_phase;
    stage = bringup_stage_from_phase(phase);
    status = BRINGUP_PHASE_STATUS_RUNNING;
  } else if (model->boot_complete != 0U) {
    phase = BRINGUP_PHASE_USER_WORKLOAD_ENABLE;
    stage = BRINGUP_STAGE_READY;
    status = BRINGUP_PHASE_STATUS_DONE;
  } else {
    for (i = 0U; i < (uint32_t)BRINGUP_PHASE_COUNT; ++i) {
      BringupPhaseStatus s = model->slots[i].status;
      if (s != BRINGUP_PHASE_STATUS_DONE) {
        phase = (BringupPhaseId)i;
        stage = bringup_stage_from_phase(phase);
        status = s;
        break;
      }
    }
  }

  if (out_stage != NULL) {
    *out_stage = stage;
  }
  if (out_status != NULL) {
    *out_status = status;
  }
  if (out_phase != NULL) {
    *out_phase = phase;
  }
  return 1;
}

static int bringup_phase_prev_done(const BringupPhaseModel *model, BringupPhaseId phase)
{
  uint32_t i;

  if (model == NULL || !bringup_phase_valid(phase)) {
    return 0;
  }
  for (i = 0U; i < (uint32_t)phase; ++i) {
    if (model->slots[i].status != BRINGUP_PHASE_STATUS_DONE) {
      return 0;
    }
  }
  return 1;
}

int bringup_phase_begin(BringupPhaseModel *model, BringupPhaseId phase)
{
  BringupPhaseSlot *slot;

  if (model == NULL || !bringup_phase_valid(phase)) {
    return BRINGUP_PHASE_ERR_BAD_ARG;
  }
  if (model->active_valid != 0U) {
    return BRINGUP_PHASE_ERR_STATE;
  }
  if (!bringup_phase_prev_done(model, phase)) {
    return BRINGUP_PHASE_ERR_ORDER;
  }

  slot = &model->slots[(uint32_t)phase];
  if (slot->status == BRINGUP_PHASE_STATUS_RUNNING) {
    return BRINGUP_PHASE_ERR_STATE;
  }

  model->transition_seq++;
  slot->status = BRINGUP_PHASE_STATUS_RUNNING;
  slot->enter_seq = model->transition_seq;
  slot->attempts++;
  slot->last_error = 0;
  model->active_valid = 1U;
  model->active_phase = (uint8_t)phase;
  model->boot_complete = 0U;
  return BRINGUP_PHASE_OK;
}

int bringup_phase_succeed(BringupPhaseModel *model, BringupPhaseId phase)
{
  BringupPhaseSlot *slot;
  uint32_t i;

  if (model == NULL || !bringup_phase_valid(phase)) {
    return BRINGUP_PHASE_ERR_BAD_ARG;
  }
  if (model->active_valid == 0U || model->active_phase != (uint8_t)phase) {
    return BRINGUP_PHASE_ERR_STATE;
  }

  slot = &model->slots[(uint32_t)phase];
  if (slot->status != BRINGUP_PHASE_STATUS_RUNNING) {
    return BRINGUP_PHASE_ERR_STATE;
  }

  model->transition_seq++;
  slot->status = BRINGUP_PHASE_STATUS_DONE;
  slot->leave_seq = model->transition_seq;
  slot->last_error = 0;
  model->active_valid = 0U;
  model->active_phase = 0U;
  model->last_error = 0;

  model->boot_complete = 1U;
  for (i = 0U; i < (uint32_t)BRINGUP_PHASE_COUNT; ++i) {
    if (model->slots[i].status != BRINGUP_PHASE_STATUS_DONE) {
      model->boot_complete = 0U;
      break;
    }
  }

  return BRINGUP_PHASE_OK;
}

int bringup_phase_fail(BringupPhaseModel *model, BringupPhaseId phase, int32_t error_code)
{
  BringupPhaseSlot *slot;

  if (model == NULL || !bringup_phase_valid(phase)) {
    return BRINGUP_PHASE_ERR_BAD_ARG;
  }
  if (model->active_valid == 0U || model->active_phase != (uint8_t)phase) {
    return BRINGUP_PHASE_ERR_STATE;
  }

  slot = &model->slots[(uint32_t)phase];
  if (slot->status != BRINGUP_PHASE_STATUS_RUNNING) {
    return BRINGUP_PHASE_ERR_STATE;
  }

  model->transition_seq++;
  slot->status = BRINGUP_PHASE_STATUS_FAILED;
  slot->leave_seq = model->transition_seq;
  slot->fail_count++;
  slot->last_error = error_code;
  model->active_valid = 0U;
  model->active_phase = 0U;
  model->boot_complete = 0U;
  model->last_error = error_code;
  return BRINGUP_PHASE_OK;
}

uint32_t bringup_phase_rollback_from(BringupPhaseModel *model, BringupPhaseId phase)
{
  uint32_t i;
  uint32_t changed = 0U;

  if (model == NULL || !bringup_phase_valid(phase)) {
    return 0U;
  }

  if (model->active_valid != 0U && model->active_phase >= (uint8_t)phase) {
    BringupPhaseSlot *active_slot = &model->slots[model->active_phase];
    model->transition_seq++;
    active_slot->status = BRINGUP_PHASE_STATUS_ROLLED_BACK;
    active_slot->leave_seq = model->transition_seq;
    active_slot->rollback_count++;
    active_slot->last_error = 0;
    model->active_valid = 0U;
    model->active_phase = 0U;
    changed++;
  }

  for (i = (uint32_t)phase; i < (uint32_t)BRINGUP_PHASE_COUNT; ++i) {
    BringupPhaseSlot *slot = &model->slots[i];
    if (slot->status == BRINGUP_PHASE_STATUS_DONE ||
        slot->status == BRINGUP_PHASE_STATUS_FAILED) {
      model->transition_seq++;
      slot->status = BRINGUP_PHASE_STATUS_ROLLED_BACK;
      slot->leave_seq = model->transition_seq;
      slot->rollback_count++;
      slot->last_error = 0;
      changed++;
    }
  }

  model->boot_complete = 0U;
  return changed;
}

int bringup_phase_set_injected_failure(BringupPhaseModel *model,
                                       BringupPhaseId phase,
                                       int32_t error_code)
{
  if (model == NULL || !bringup_phase_valid(phase)) {
    return BRINGUP_PHASE_ERR_BAD_ARG;
  }

  model->inject_mask |= (1UL << (uint32_t)phase);
  model->inject_error[(uint32_t)phase] =
    (error_code != 0) ? error_code : bringup_phase_default_inject_error(phase);
  return BRINGUP_PHASE_OK;
}

void bringup_phase_clear_injected_failure(BringupPhaseModel *model, BringupPhaseId phase)
{
  if (model == NULL || !bringup_phase_valid(phase)) {
    return;
  }
  model->inject_mask &= ~(1UL << (uint32_t)phase);
}

void bringup_phase_clear_all_injected_failures(BringupPhaseModel *model)
{
  if (model == NULL) {
    return;
  }
  model->inject_mask = 0U;
}

int bringup_phase_consume_injected_failure(BringupPhaseModel *model,
                                           BringupPhaseId phase,
                                           int32_t *out_error_code)
{
  uint32_t bit;
  int32_t code;

  if (model == NULL || !bringup_phase_valid(phase)) {
    return 0;
  }
  bit = (1UL << (uint32_t)phase);
  if ((model->inject_mask & bit) == 0U) {
    return 0;
  }

  code = model->inject_error[(uint32_t)phase];
  model->inject_mask &= ~bit;
  if (out_error_code != NULL) {
    *out_error_code = code;
  }
  return 1;
}

int bringup_phase_all_done(const BringupPhaseModel *model)
{
  uint32_t i;

  if (model == NULL) {
    return 0;
  }
  for (i = 0U; i < (uint32_t)BRINGUP_PHASE_COUNT; ++i) {
    if (model->slots[i].status != BRINGUP_PHASE_STATUS_DONE) {
      return 0;
    }
  }
  return 1;
}

int bringup_phase_get_slot(const BringupPhaseModel *model,
                           BringupPhaseId phase,
                           BringupPhaseSlot *out_slot)
{
  if (model == NULL || out_slot == NULL || !bringup_phase_valid(phase)) {
    return 0;
  }
  *out_slot = model->slots[(uint32_t)phase];
  return 1;
}
