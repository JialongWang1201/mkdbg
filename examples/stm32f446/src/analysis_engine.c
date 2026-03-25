#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "analysis_engine.h"
#include "bringup_phase.h"

#define ANALYSIS_ENGINE_IMPL_RULE_BASED 1U
#define ANALYSIS_ENGINE_IMPL_MODEL_ADAPTER 2U
#define ANALYSIS_ENGINE_IMPL_MODEL_MOCK 3U
#define ANALYSIS_EVT_BOUNDARY_FAULT 0x0001U
#define ANALYSIS_EVT_BOUNDARY_STAGE 0x0004U
#define ANALYSIS_HYP_RESET_NEAR_FAULT_MS 1500U
#define ANALYSIS_HYP_STAGE_NEAR_FAULT_MS 2000U

static int analysis_msg_token_char(char c)
{
  return (isalnum((unsigned char)c) || c == '-' || c == '_') ? 1 : 0;
}

static int analysis_msg_contains_token(const char *msg, const char *token)
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
    if (!analysis_msg_token_char(left) && !analysis_msg_token_char(right)) {
      return 1;
    }
    p += token_len;
  }

  return 0;
}

static uint8_t analysis_event_is_irq_related(const char *msg)
{
  if (msg == NULL) {
    return 0U;
  }
  if (strncmp(msg, "kdi irq ", 8) == 0) {
    return 1U;
  }
  return analysis_msg_contains_token(msg, "irq") ? 1U : 0U;
}

static uint8_t analysis_event_is_irq_anomaly_hint(const char *msg)
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
  if (analysis_event_is_irq_related(msg) != 0U &&
      (strstr(msg, "rc=limit") != NULL || strstr(msg, ": limit") != NULL)) {
    return 1U;
  }
  return 0U;
}

static uint8_t analysis_event_is_state_error(const char *msg)
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

static uint8_t analysis_event_is_state_reset(const char *msg)
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

static uint8_t analysis_event_has_cap_eperm(const char *msg)
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

static uint8_t analysis_event_has_mpu_violation(const char *msg)
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

static uint8_t analysis_event_is_dma_related(const char *msg)
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

static uint8_t analysis_event_is_limit_msg(const char *msg)
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

static uint8_t analysis_event_is_dma_pressure_hint(const char *msg)
{
  if (msg == NULL) {
    return 0U;
  }
  if (analysis_event_is_dma_related(msg) == 0U) {
    return 0U;
  }
  if (analysis_event_is_limit_msg(msg) != 0U ||
      strstr(msg, "backpressure") != NULL ||
      strstr(msg, "ring_overflow") != NULL) {
    return 1U;
  }
  return 0U;
}

static uint8_t analysis_event_is_recovery_msg(const char *msg)
{
  if (msg == NULL) {
    return 0U;
  }
  if (strstr(msg, "reinit") != NULL ||
      strstr(msg, "ready") != NULL ||
      strstr(msg, "active") != NULL ||
      strstr(msg, "probe") != NULL) {
    return 1U;
  }
  return 0U;
}

static uint8_t analysis_event_in_fault_lookback(const AnalysisFaultFeatureVector *vec,
                                                const AnalysisEvent *event)
{
  uint32_t begin_ms;

  if (vec == NULL || event == NULL) {
    return 0U;
  }
  begin_ms = (vec->fault_ts_ms > vec->lookback_ms) ? (vec->fault_ts_ms - vec->lookback_ms) : 0U;
  if (event->ts_ms < begin_ms || event->ts_ms > vec->fault_ts_ms) {
    return 0U;
  }
  return 1U;
}

static uint8_t analysis_event_is_timeout_msg(const char *msg)
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

static uint8_t analysis_event_is_dependency_wait_msg(const char *msg)
{
  if (msg == NULL) {
    return 0U;
  }
  if (strncmp(msg, "dep ", 4) == 0) {
    return 1U;
  }
  if (strstr(msg, "dependency") != NULL ||
      strstr(msg, "stage-wait") != NULL ||
      strstr(msg, "phase sequencing barrier") != NULL ||
      strstr(msg, "waiting previous phase done") != NULL ||
      strstr(msg, "blocked=1") != NULL) {
    return 1U;
  }
  return 0U;
}

static uint8_t analysis_event_is_resource_fault_hint(const char *msg)
{
  if (msg == NULL) {
    return 0U;
  }
  if (analysis_event_is_limit_msg(msg) != 0U ||
      analysis_event_is_dma_pressure_hint(msg) != 0U ||
      analysis_event_is_irq_anomaly_hint(msg) != 0U) {
    return 1U;
  }
  if (strstr(msg, "overflow") != NULL ||
      strstr(msg, "exhaust") != NULL ||
      strstr(msg, "resource fault") != NULL) {
    return 1U;
  }
  return 0U;
}

static uint8_t analysis_slice_fault_event_id(const AnalysisEventSlice *slice,
                                             const AnalysisEvent *events,
                                             uint8_t event_count,
                                             uint8_t *out_id)
{
  if (slice == NULL || events == NULL || out_id == NULL) {
    return 0U;
  }
  for (uint8_t i = slice->begin; i <= slice->end && i < event_count; ++i) {
    if ((events[i].boundary_flags & ANALYSIS_EVT_BOUNDARY_FAULT) != 0U) {
      *out_id = i;
      return 1U;
    }
  }
  *out_id = slice->end;
  return 1U;
}

static void analysis_hyp_add_evidence_id(uint8_t event_id,
                                         uint8_t *ids,
                                         uint8_t *count,
                                         uint8_t cap)
{
  if (ids == NULL || count == NULL || cap == 0U) {
    return;
  }
  for (uint8_t i = 0U; i < *count; ++i) {
    if (ids[i] == event_id) {
      return;
    }
  }
  if (*count < cap) {
    ids[*count] = event_id;
    (*count)++;
  }
}

static uint8_t analysis_confidence_clamp(int score)
{
  if (score < 0) {
    return 0U;
  }
  if (score > 99) {
    return 99U;
  }
  return (uint8_t)score;
}

static void analysis_set_hypothesis(AnalysisHypothesisResult *out,
                                    const char *name,
                                    int score,
                                    const uint8_t *evidence,
                                    uint8_t evidence_count,
                                    const char *p1,
                                    const char *p2)
{
  if (out == NULL) {
    return;
  }

  memset(out, 0, sizeof(*out));
  out->name = name;
  out->confidence = analysis_confidence_clamp(score);
  out->evidence_count = evidence_count;
  for (uint8_t i = 0U; i < evidence_count && i < ANALYSIS_EVIDENCE_CAP; ++i) {
    out->evidence_ids[i] = evidence[i];
  }
  if (p1 != NULL) {
    (void)snprintf(out->explain_p1, sizeof(out->explain_p1), "%s", p1);
  }
  if (p2 != NULL) {
    (void)snprintf(out->explain_p2, sizeof(out->explain_p2), "%s", p2);
  }
}

const char *analysis_failure_category_name(AnalysisFailureCategory category)
{
  switch (category) {
    case ANALYSIS_FAILURE_ORDERING_VIOLATION:
      return "ordering_violation";
    case ANALYSIS_FAILURE_MISSING_DEPENDENCY:
      return "missing_dependency";
    case ANALYSIS_FAILURE_TIMEOUT:
      return "timeout";
    case ANALYSIS_FAILURE_PERMISSION_VIOLATION:
      return "permission_violation";
    case ANALYSIS_FAILURE_EARLY_RESOURCE_FAULT:
      return "early_resource_fault";
    default:
      return "ordering_violation";
  }
}

static void analysis_rule_classify_failure(const AnalysisInput *input,
                                           AnalysisResult *out)
{
  static const AnalysisFailureCategory priority_order[] = {
    ANALYSIS_FAILURE_PERMISSION_VIOLATION,
    ANALYSIS_FAILURE_TIMEOUT,
    ANALYSIS_FAILURE_MISSING_DEPENDENCY,
    ANALYSIS_FAILURE_EARLY_RESOURCE_FAULT,
    ANALYSIS_FAILURE_ORDERING_VIOLATION,
  };
  const AnalysisEvent *events = input->events;
  const AnalysisEventSlice *slice = input->slice;
  const AnalysisFaultFeatureVector *vec = input->features;
  int score[ANALYSIS_FAILURE_CATEGORY_COUNT] = {24, 14, 4, 4, 8};
  uint8_t evidence[ANALYSIS_FAILURE_CATEGORY_COUNT][ANALYSIS_EVIDENCE_CAP];
  uint8_t evidence_count[ANALYSIS_FAILURE_CATEGORY_COUNT] = {0};
  uint8_t ordering_hits = 0U;
  uint8_t dependency_hits = 0U;
  uint8_t timeout_hits = 0U;
  uint8_t permission_hits = 0U;
  uint8_t resource_hits = 0U;
  uint8_t early_stage = 0U;
  uint8_t winner_fault_id = 0U;
  uint8_t winner_fault_id_valid = 0U;
  const char *stage_name = "none";
  AnalysisFailureCategory winner = ANALYSIS_FAILURE_ORDERING_VIOLATION;
  int winner_score = -9999;

  memset(evidence, 0, sizeof(evidence));

  if (slice->stage_valid != 0U && slice->stage_id < (uint8_t)BRINGUP_PHASE_COUNT) {
    BringupPhaseId phase = (BringupPhaseId)slice->stage_id;
    BringupStageId stage = bringup_stage_from_phase(phase);
    stage_name = bringup_stage_name(stage);
    if (stage <= BRINGUP_STAGE_KERNEL) {
      early_stage = 1U;
    }
  } else if (input->boot_complete == 0U) {
    early_stage = 1U;
  }

  for (uint8_t i = slice->begin; i <= slice->end && i < input->event_count; ++i) {
    const AnalysisEvent *event = &events[i];

    if (analysis_event_is_state_error(event->msg) != 0U ||
        analysis_event_is_state_reset(event->msg) != 0U ||
        strstr(event->msg, " run fail ") != NULL ||
        strstr(event->msg, "rollback applied") != NULL ||
        strstr(event->msg, "logical rollback applied") != NULL ||
        strstr(event->msg, "rolled_back") != NULL) {
      if (ordering_hits < 0xFFU) {
        ordering_hits++;
      }
      analysis_hyp_add_evidence_id(i,
                                   evidence[ANALYSIS_FAILURE_ORDERING_VIOLATION],
                                   &evidence_count[ANALYSIS_FAILURE_ORDERING_VIOLATION],
                                   ANALYSIS_EVIDENCE_CAP);
    }

    if (analysis_event_is_dependency_wait_msg(event->msg) != 0U) {
      if (dependency_hits < 0xFFU) {
        dependency_hits++;
      }
      analysis_hyp_add_evidence_id(i,
                                   evidence[ANALYSIS_FAILURE_MISSING_DEPENDENCY],
                                   &evidence_count[ANALYSIS_FAILURE_MISSING_DEPENDENCY],
                                   ANALYSIS_EVIDENCE_CAP);
    }

    if (analysis_event_is_timeout_msg(event->msg) != 0U) {
      if (timeout_hits < 0xFFU) {
        timeout_hits++;
      }
      analysis_hyp_add_evidence_id(i,
                                   evidence[ANALYSIS_FAILURE_TIMEOUT],
                                   &evidence_count[ANALYSIS_FAILURE_TIMEOUT],
                                   ANALYSIS_EVIDENCE_CAP);
    }

    if (analysis_event_has_cap_eperm(event->msg) != 0U ||
        analysis_event_has_mpu_violation(event->msg) != 0U ||
        strstr(event->msg, "rc=auth") != NULL ||
        strstr(event->msg, "rc=denied") != NULL ||
        strstr(event->msg, ": auth") != NULL ||
        strstr(event->msg, ": denied") != NULL) {
      if (permission_hits < 0xFFU) {
        permission_hits++;
      }
      analysis_hyp_add_evidence_id(i,
                                   evidence[ANALYSIS_FAILURE_PERMISSION_VIOLATION],
                                   &evidence_count[ANALYSIS_FAILURE_PERMISSION_VIOLATION],
                                   ANALYSIS_EVIDENCE_CAP);
    }

    if (analysis_event_is_resource_fault_hint(event->msg) != 0U) {
      if (resource_hits < 0xFFU) {
        resource_hits++;
      }
      analysis_hyp_add_evidence_id(i,
                                   evidence[ANALYSIS_FAILURE_EARLY_RESOURCE_FAULT],
                                   &evidence_count[ANALYSIS_FAILURE_EARLY_RESOURCE_FAULT],
                                   ANALYSIS_EVIDENCE_CAP);
    }
  }

  if (ordering_hits != 0U) {
    score[ANALYSIS_FAILURE_ORDERING_VIOLATION] += 20;
  }
  if (vec->state_error_reset_loop != 0U) {
    score[ANALYSIS_FAILURE_ORDERING_VIOLATION] += 35;
  }
  if (slice->reset_events != 0U && slice->fault_events != 0U) {
    score[ANALYSIS_FAILURE_ORDERING_VIOLATION] += 8;
  }

  if (dependency_hits != 0U) {
    score[ANALYSIS_FAILURE_MISSING_DEPENDENCY] += 35;
  } else {
    score[ANALYSIS_FAILURE_MISSING_DEPENDENCY] -= 6;
  }
  if (dependency_hits >= 2U) {
    score[ANALYSIS_FAILURE_MISSING_DEPENDENCY] += 10;
  }
  if (slice->fault_events == 0U && dependency_hits != 0U) {
    score[ANALYSIS_FAILURE_MISSING_DEPENDENCY] += 8;
  }

  if (timeout_hits != 0U) {
    score[ANALYSIS_FAILURE_TIMEOUT] += 40;
    if (timeout_hits >= 2U) {
      score[ANALYSIS_FAILURE_TIMEOUT] += 8;
    }
  } else {
    score[ANALYSIS_FAILURE_TIMEOUT] = 0;
  }

  if (permission_hits != 0U) {
    score[ANALYSIS_FAILURE_PERMISSION_VIOLATION] += 35;
  }
  if (vec->cap_eperm != 0U) {
    score[ANALYSIS_FAILURE_PERMISSION_VIOLATION] += 20;
  }
  if (vec->cap_mpu_violation != 0U) {
    score[ANALYSIS_FAILURE_PERMISSION_VIOLATION] += 25;
  }
  if (permission_hits == 0U &&
      vec->cap_eperm == 0U &&
      vec->cap_mpu_violation == 0U) {
    score[ANALYSIS_FAILURE_PERMISSION_VIOLATION] = 0;
  }

  if (resource_hits != 0U) {
    score[ANALYSIS_FAILURE_EARLY_RESOURCE_FAULT] += 20;
  }
  if (vec->kdi_fail_frequent != 0U) {
    score[ANALYSIS_FAILURE_EARLY_RESOURCE_FAULT] += 15;
  }
  if (vec->dma_full != 0U) {
    score[ANALYSIS_FAILURE_EARLY_RESOURCE_FAULT] += 18;
  }
  if (vec->irq_rate_abnormal != 0U) {
    score[ANALYSIS_FAILURE_EARLY_RESOURCE_FAULT] += 10;
  }
  if (vec->irq_deferred_pending >= 8U) {
    score[ANALYSIS_FAILURE_EARLY_RESOURCE_FAULT] += 8;
  }
  if (early_stage != 0U) {
    if (resource_hits != 0U ||
        vec->kdi_fail_frequent != 0U ||
        vec->dma_full != 0U ||
        vec->irq_rate_abnormal != 0U) {
      score[ANALYSIS_FAILURE_EARLY_RESOURCE_FAULT] += 14;
    } else {
      score[ANALYSIS_FAILURE_EARLY_RESOURCE_FAULT] += 2;
    }
  }
  if (resource_hits == 0U &&
      vec->kdi_fail_frequent == 0U &&
      vec->dma_full == 0U &&
      vec->irq_rate_abnormal == 0U) {
    score[ANALYSIS_FAILURE_EARLY_RESOURCE_FAULT] -= 6;
  }

  if (permission_hits != 0U || vec->cap_eperm != 0U || vec->cap_mpu_violation != 0U) {
    score[ANALYSIS_FAILURE_MISSING_DEPENDENCY] -= 8;
    score[ANALYSIS_FAILURE_EARLY_RESOURCE_FAULT] -= 8;
  }
  if (timeout_hits != 0U) {
    score[ANALYSIS_FAILURE_ORDERING_VIOLATION] -= 6;
    score[ANALYSIS_FAILURE_MISSING_DEPENDENCY] -= 3;
  }

  for (uint8_t p = 0U; p < (uint8_t)(sizeof(priority_order) / sizeof(priority_order[0])); ++p) {
    AnalysisFailureCategory cat = priority_order[p];
    int s = score[(uint8_t)cat];
    if (s > winner_score) {
      winner = cat;
      winner_score = s;
    }
  }

  winner_fault_id_valid =
    analysis_slice_fault_event_id(slice, events, input->event_count, &winner_fault_id);
  if (evidence_count[(uint8_t)winner] == 0U) {
    if (winner_fault_id_valid != 0U) {
      analysis_hyp_add_evidence_id(winner_fault_id,
                                   evidence[(uint8_t)winner],
                                   &evidence_count[(uint8_t)winner],
                                   ANALYSIS_EVIDENCE_CAP);
    } else {
      analysis_hyp_add_evidence_id(slice->end,
                                   evidence[(uint8_t)winner],
                                   &evidence_count[(uint8_t)winner],
                                   ANALYSIS_EVIDENCE_CAP);
    }
  }

  out->failure.category = winner;
  out->failure.confidence = analysis_confidence_clamp(winner_score);
  out->failure.evidence_count = evidence_count[(uint8_t)winner];
  for (uint8_t i = 0U; i < out->failure.evidence_count && i < ANALYSIS_EVIDENCE_CAP; ++i) {
    out->failure.evidence_ids[i] = evidence[(uint8_t)winner][i];
  }

  switch (winner) {
    case ANALYSIS_FAILURE_ORDERING_VIOLATION:
      (void)snprintf(out->failure.explain_p1,
                     sizeof(out->failure.explain_p1),
                     "Ordering signals: hits=%u loop=%u reset=%u.",
                     (unsigned)ordering_hits,
                     (unsigned)vec->state_error_reset_loop,
                     (unsigned)slice->reset_events);
      (void)snprintf(out->failure.explain_p2,
                     sizeof(out->failure.explain_p2),
                     "State transitions/regressions indicate lifecycle sequencing broke before failure.");
      break;
    case ANALYSIS_FAILURE_MISSING_DEPENDENCY:
      (void)snprintf(out->failure.explain_p1,
                     sizeof(out->failure.explain_p1),
                     "Dependency wait signals: dep_hits=%u stage=%s.",
                     (unsigned)dependency_hits,
                     stage_name);
      (void)snprintf(out->failure.explain_p2,
                     sizeof(out->failure.explain_p2),
                     "Failure aligns with unresolved stage-driver-resource prerequisites.");
      break;
    case ANALYSIS_FAILURE_TIMEOUT:
      (void)snprintf(out->failure.explain_p1,
                     sizeof(out->failure.explain_p1),
                     "Timeout signals observed: timeout_hits=%u events=%u.",
                     (unsigned)timeout_hits,
                     (unsigned)(slice->end - slice->begin + 1U));
      (void)snprintf(out->failure.explain_p2,
                     sizeof(out->failure.explain_p2),
                     "Operation exceeded completion window and faulted before recovery.");
      break;
    case ANALYSIS_FAILURE_PERMISSION_VIOLATION:
      (void)snprintf(out->failure.explain_p1,
                     sizeof(out->failure.explain_p1),
                     "Permission signals: perm_hits=%u eperm=%u mpu_vio=%u.",
                     (unsigned)permission_hits,
                     (unsigned)vec->cap_eperm,
                     (unsigned)vec->cap_mpu_violation);
      (void)snprintf(out->failure.explain_p2,
                     sizeof(out->failure.explain_p2),
                     "Capability/MPU guard denied access and triggered failure path.");
      break;
    case ANALYSIS_FAILURE_EARLY_RESOURCE_FAULT:
      (void)snprintf(out->failure.explain_p1,
                     sizeof(out->failure.explain_p1),
                     "Resource pressure: hits=%u dma_full=%u irq_abn=%u early=%u.",
                     (unsigned)resource_hits,
                     (unsigned)vec->dma_full,
                     (unsigned)vec->irq_rate_abnormal,
                     (unsigned)early_stage);
      (void)snprintf(out->failure.explain_p2,
                     sizeof(out->failure.explain_p2),
                     "Early bring-up resource pressure likely exhausted queue/budget/capacity.");
      break;
    default:
      break;
  }
}

static void analysis_rule_hypothesis_irq_starvation(const AnalysisInput *input,
                                                    uint8_t fault_id_valid,
                                                    uint8_t fault_id,
                                                    AnalysisHypothesisResult *out)
{
  uint8_t evidence[ANALYSIS_EVIDENCE_CAP] = {0};
  uint8_t evidence_count = 0U;
  uint16_t irq_anom_hits = 0U;
  int score = 20;
  char p1[ANALYSIS_EXPLAIN_TEXT_MAX];
  char p2[ANALYSIS_EXPLAIN_TEXT_MAX];
  const AnalysisFaultFeatureVector *vec = input->features;

  for (uint8_t i = 0U; i < input->event_count; ++i) {
    const AnalysisEvent *event = &input->events[i];
    if (analysis_event_in_fault_lookback(vec, event) == 0U) {
      continue;
    }
    if (analysis_event_is_irq_related(event->msg) != 0U) {
      analysis_hyp_add_evidence_id(i, evidence, &evidence_count, ANALYSIS_EVIDENCE_CAP);
    }
    if (analysis_event_is_irq_anomaly_hint(event->msg) != 0U) {
      irq_anom_hits++;
      analysis_hyp_add_evidence_id(i, evidence, &evidence_count, ANALYSIS_EVIDENCE_CAP);
    }
  }

  if (vec->irq_rate_abnormal != 0U) {
    score += 35;
  }
  if (vec->irq_event_count >= 2U) {
    score += 10;
  }
  if (irq_anom_hits != 0U) {
    score += 12;
  }
  if (vec->irq_deferred_pending >= 8U) {
    score += 10;
  }
  if (vec->kdi_fail_frequent != 0U && vec->irq_event_count != 0U) {
    score += 8;
  }
  if (vec->irq_event_count == 0U && irq_anom_hits == 0U && vec->irq_deferred_pending == 0U) {
    score = 8;
  }

  if (evidence_count == 0U && fault_id_valid != 0U) {
    analysis_hyp_add_evidence_id(fault_id, evidence, &evidence_count, ANALYSIS_EVIDENCE_CAP);
  }

  (void)snprintf(p1,
                 sizeof(p1),
                 "IRQ activity before fault shows irq_evt=%u rate=%u/s pending=%u.",
                 (unsigned)vec->irq_event_count,
                 (unsigned)vec->irq_rate_per_sec,
                 (unsigned)vec->irq_deferred_pending);
  (void)snprintf(p2,
                 sizeof(p2),
                 "Throttle/defer signals suggest handler starvation and delayed service completion.");
  analysis_set_hypothesis(out, "irq_starvation", score, evidence, evidence_count, p1, p2);
}

static void analysis_rule_hypothesis_dma_backpressure(const AnalysisInput *input,
                                                      uint8_t fault_id_valid,
                                                      uint8_t fault_id,
                                                      AnalysisHypothesisResult *out)
{
  uint8_t evidence[ANALYSIS_EVIDENCE_CAP] = {0};
  uint8_t evidence_count = 0U;
  uint16_t dma_pressure_hits = 0U;
  int score = 15;
  char p1[ANALYSIS_EXPLAIN_TEXT_MAX];
  char p2[ANALYSIS_EXPLAIN_TEXT_MAX];
  const AnalysisFaultFeatureVector *vec = input->features;

  for (uint8_t i = 0U; i < input->event_count; ++i) {
    const AnalysisEvent *event = &input->events[i];
    if (analysis_event_in_fault_lookback(vec, event) == 0U) {
      continue;
    }
    if (analysis_event_is_dma_related(event->msg) != 0U) {
      analysis_hyp_add_evidence_id(i, evidence, &evidence_count, ANALYSIS_EVIDENCE_CAP);
    }
    if (analysis_event_is_dma_pressure_hint(event->msg) != 0U) {
      dma_pressure_hits++;
      analysis_hyp_add_evidence_id(i, evidence, &evidence_count, ANALYSIS_EVIDENCE_CAP);
    }
  }

  if (vec->dma_full != 0U) {
    score += 45;
  }
  if (dma_pressure_hits != 0U) {
    score += 15;
  }
  if (vec->dma_idle_spin != 0U && vec->dma_full == 0U) {
    score -= 5;
  }
  if (vec->dma_full == 0U && dma_pressure_hits == 0U) {
    score = 10;
  }
  if (score < 5) {
    score = 5;
  }

  if (evidence_count == 0U && fault_id_valid != 0U) {
    analysis_hyp_add_evidence_id(fault_id, evidence, &evidence_count, ANALYSIS_EVIDENCE_CAP);
  }

  (void)snprintf(p1,
                 sizeof(p1),
                 "DMA ring signals indicate dma_full=%u dma_idle=%u before the fault.",
                 (unsigned)vec->dma_full,
                 (unsigned)vec->dma_idle_spin);
  (void)snprintf(p2,
                 sizeof(p2),
                 "Backpressure risk rises when queue depth saturates and producers outrun consumers.");
  analysis_set_hypothesis(out, "dma_backpressure", score, evidence, evidence_count, p1, p2);
}

static void analysis_rule_hypothesis_capability_violation(const AnalysisInput *input,
                                                          uint8_t fault_id_valid,
                                                          uint8_t fault_id,
                                                          AnalysisHypothesisResult *out)
{
  uint8_t evidence[ANALYSIS_EVIDENCE_CAP] = {0};
  uint8_t evidence_count = 0U;
  uint16_t cap_hits = 0U;
  int score = 10;
  char p1[ANALYSIS_EXPLAIN_TEXT_MAX];
  char p2[ANALYSIS_EXPLAIN_TEXT_MAX];
  const AnalysisFaultFeatureVector *vec = input->features;

  for (uint8_t i = 0U; i < input->event_count; ++i) {
    const AnalysisEvent *event = &input->events[i];
    if (analysis_event_in_fault_lookback(vec, event) == 0U) {
      continue;
    }
    if (analysis_event_has_cap_eperm(event->msg) != 0U ||
        analysis_event_has_mpu_violation(event->msg) != 0U) {
      cap_hits++;
      analysis_hyp_add_evidence_id(i, evidence, &evidence_count, ANALYSIS_EVIDENCE_CAP);
    }
  }

  if (vec->cap_eperm != 0U) {
    score += 35;
  }
  if (vec->cap_mpu_violation != 0U) {
    score += 40;
  }
  if (vec->kdi_fail_frequent != 0U) {
    score += 10;
  }
  if (cap_hits >= 2U) {
    score += 10;
  }
  if (vec->cap_eperm == 0U && vec->cap_mpu_violation == 0U && cap_hits == 0U) {
    score = 6;
  }

  if (evidence_count == 0U && fault_id_valid != 0U) {
    analysis_hyp_add_evidence_id(fault_id, evidence, &evidence_count, ANALYSIS_EVIDENCE_CAP);
  }

  (void)snprintf(p1,
                 sizeof(p1),
                 "Access-control evidence shows eperm=%u mpu_vio=%u in pre-fault events.",
                 (unsigned)vec->cap_eperm,
                 (unsigned)vec->cap_mpu_violation);
  (void)snprintf(p2,
                 sizeof(p2),
                 "Auth/deny or MPU-protection breaches indicate capability boundary violations.");
  analysis_set_hypothesis(out, "capability_violation", score, evidence, evidence_count, p1, p2);
}

static void analysis_rule_hypothesis_ordering_issue(const AnalysisInput *input,
                                                    uint8_t fault_id_valid,
                                                    uint8_t fault_id,
                                                    AnalysisHypothesisResult *out)
{
  uint8_t evidence[ANALYSIS_EVIDENCE_CAP] = {0};
  uint8_t evidence_count = 0U;
  uint8_t reset_seen = 0U;
  uint8_t reset_near_fault = 0U;
  uint8_t recovery_after_reset = 0U;
  uint8_t stage_near_fault = 0U;
  int score = 15;
  char p1[ANALYSIS_EXPLAIN_TEXT_MAX];
  char p2[ANALYSIS_EXPLAIN_TEXT_MAX];
  const AnalysisFaultFeatureVector *vec = input->features;

  for (uint8_t i = 0U; i < input->event_count; ++i) {
    const AnalysisEvent *event = &input->events[i];
    if (analysis_event_in_fault_lookback(vec, event) == 0U) {
      continue;
    }
    if (analysis_event_is_state_error(event->msg) != 0U) {
      analysis_hyp_add_evidence_id(i, evidence, &evidence_count, ANALYSIS_EVIDENCE_CAP);
    }
    if (analysis_event_is_state_reset(event->msg) != 0U) {
      uint32_t delta = vec->fault_ts_ms - event->ts_ms;
      reset_seen = 1U;
      analysis_hyp_add_evidence_id(i, evidence, &evidence_count, ANALYSIS_EVIDENCE_CAP);
      if (delta <= ANALYSIS_HYP_RESET_NEAR_FAULT_MS) {
        reset_near_fault = 1U;
      }
    }
    if (reset_seen != 0U && analysis_event_is_recovery_msg(event->msg) != 0U) {
      recovery_after_reset = 1U;
      analysis_hyp_add_evidence_id(i, evidence, &evidence_count, ANALYSIS_EVIDENCE_CAP);
    }
    if ((event->boundary_flags & ANALYSIS_EVT_BOUNDARY_STAGE) != 0U) {
      uint32_t delta = vec->fault_ts_ms - event->ts_ms;
      if (delta <= ANALYSIS_HYP_STAGE_NEAR_FAULT_MS) {
        stage_near_fault = 1U;
        analysis_hyp_add_evidence_id(i, evidence, &evidence_count, ANALYSIS_EVIDENCE_CAP);
      }
    }
  }

  if (vec->state_error_reset_loop != 0U) {
    score += 35;
  }
  if (reset_near_fault != 0U) {
    score += 15;
  }
  if (recovery_after_reset != 0U) {
    score += 15;
  }
  if (stage_near_fault != 0U) {
    score += 10;
  }
  if (vec->state_error_reset_loop == 0U &&
      reset_near_fault == 0U &&
      recovery_after_reset == 0U &&
      stage_near_fault == 0U) {
    score = 7;
  }

  if (evidence_count == 0U && fault_id_valid != 0U) {
    analysis_hyp_add_evidence_id(fault_id, evidence, &evidence_count, ANALYSIS_EVIDENCE_CAP);
  }

  (void)snprintf(p1,
                 sizeof(p1),
                 "State sequencing saw state_loop=%u reset_near=%u recovery_after=%u.",
                 (unsigned)vec->state_error_reset_loop,
                 (unsigned)reset_near_fault,
                 (unsigned)recovery_after_reset);
  (void)snprintf(p2,
                 sizeof(p2),
                 "Reset/recovery ordering close to fault suggests race or lifecycle ordering defects.");
  analysis_set_hypothesis(out, "ordering_issue", score, evidence, evidence_count, p1, p2);
}

static void analysis_rule_hypothesis_resource_exhaustion(const AnalysisInput *input,
                                                         uint8_t fault_id_valid,
                                                         uint8_t fault_id,
                                                         AnalysisHypothesisResult *out)
{
  uint8_t evidence[ANALYSIS_EVIDENCE_CAP] = {0};
  uint8_t evidence_count = 0U;
  uint16_t limit_hits = 0U;
  int score = 15;
  char p1[ANALYSIS_EXPLAIN_TEXT_MAX];
  char p2[ANALYSIS_EXPLAIN_TEXT_MAX];
  const AnalysisFaultFeatureVector *vec = input->features;

  for (uint8_t i = 0U; i < input->event_count; ++i) {
    const AnalysisEvent *event = &input->events[i];
    if (analysis_event_in_fault_lookback(vec, event) == 0U) {
      continue;
    }
    if (analysis_event_is_limit_msg(event->msg) != 0U) {
      limit_hits++;
      analysis_hyp_add_evidence_id(i, evidence, &evidence_count, ANALYSIS_EVIDENCE_CAP);
    }
    if (analysis_event_is_dma_pressure_hint(event->msg) != 0U ||
        analysis_event_is_irq_anomaly_hint(event->msg) != 0U) {
      analysis_hyp_add_evidence_id(i, evidence, &evidence_count, ANALYSIS_EVIDENCE_CAP);
    }
  }

  if (limit_hits != 0U) {
    score += 25;
  }
  if (vec->kdi_fail_frequent != 0U) {
    score += 20;
  }
  if (vec->dma_full != 0U) {
    score += 20;
  }
  if (vec->irq_rate_abnormal != 0U) {
    score += 10;
  }
  if (vec->irq_deferred_pending >= 8U) {
    score += 10;
  }
  if (vec->lookback_event_count >= 8U) {
    score += 5;
  }
  if (limit_hits == 0U &&
      vec->kdi_fail_frequent == 0U &&
      vec->dma_full == 0U &&
      vec->irq_rate_abnormal == 0U) {
    score = 8;
  }

  if (evidence_count == 0U && fault_id_valid != 0U) {
    analysis_hyp_add_evidence_id(fault_id, evidence, &evidence_count, ANALYSIS_EVIDENCE_CAP);
  }

  (void)snprintf(p1,
                 sizeof(p1),
                 "Pressure signals show limit_hits=%u kdi_fail=%u dma_full=%u pending=%u.",
                 (unsigned)limit_hits,
                 (unsigned)vec->kdi_fail_count,
                 (unsigned)vec->dma_full,
                 (unsigned)vec->irq_deferred_pending);
  (void)snprintf(p2,
                 sizeof(p2),
                 "Capacity pressure likely exhausted queues, tokens, or budget before fault.");
  analysis_set_hypothesis(out, "resource_exhaustion", score, evidence, evidence_count, p1, p2);
}

static int analysis_engine_rule_analyze_fault_slice(const AnalysisEngine *engine,
                                                    const AnalysisInput *input,
                                                    AnalysisResult *out)
{
  uint8_t fault_id = 0U;
  uint8_t fault_id_valid = 0U;

  (void)engine;

  if (input == NULL || out == NULL) {
    return -1;
  }
  if (input->events == NULL || input->slice == NULL || input->features == NULL) {
    return -1;
  }
  if (input->event_count == 0U) {
    return -1;
  }
  if (input->slice->begin > input->slice->end || input->slice->end >= input->event_count) {
    return -1;
  }

  memset(out, 0, sizeof(*out));

  analysis_rule_classify_failure(input, out);

  fault_id_valid = analysis_slice_fault_event_id(input->slice,
                                                 input->events,
                                                 input->event_count,
                                                 &fault_id);

  out->hypothesis_count = ANALYSIS_HYPOTHESIS_CAP;
  analysis_rule_hypothesis_irq_starvation(input,
                                          fault_id_valid,
                                          fault_id,
                                          &out->hypotheses[0]);
  analysis_rule_hypothesis_dma_backpressure(input,
                                            fault_id_valid,
                                            fault_id,
                                            &out->hypotheses[1]);
  analysis_rule_hypothesis_capability_violation(input,
                                                fault_id_valid,
                                                fault_id,
                                                &out->hypotheses[2]);
  analysis_rule_hypothesis_ordering_issue(input,
                                          fault_id_valid,
                                          fault_id,
                                          &out->hypotheses[3]);
  analysis_rule_hypothesis_resource_exhaustion(input,
                                               fault_id_valid,
                                               fault_id,
                                               &out->hypotheses[4]);

  return 0;
}

static int analysis_engine_model_adapter_analyze_fault_slice(const AnalysisEngine *engine,
                                                             const AnalysisInput *input,
                                                             AnalysisResult *out)
{
  if (engine == NULL || engine->adapter.analyze_fault_slice == NULL) {
    return -1;
  }
  return engine->adapter.analyze_fault_slice(input, out, engine->adapter.ctx);
}

static int analysis_model_mock_analyze_fault_slice(const AnalysisInput *input,
                                                   AnalysisResult *out,
                                                   void *ctx)
{
  (void)ctx;
  if (analysis_engine_rule_analyze_fault_slice(NULL, input, out) != 0) {
    return -1;
  }
  (void)snprintf(out->failure.explain_p2,
                 sizeof(out->failure.explain_p2),
                 "Model mock adapter pass-through; swap adapter for learned inference.");
  return 0;
}

void analysis_engine_init_rule_based(AnalysisEngine *engine)
{
  if (engine == NULL) {
    return;
  }
  memset(engine, 0, sizeof(*engine));
  engine->analyze_fault_slice = analysis_engine_rule_analyze_fault_slice;
  engine->impl_id = ANALYSIS_ENGINE_IMPL_RULE_BASED;
  engine->impl_name = "rule-based";
}

void analysis_engine_init_model_adapter(AnalysisEngine *engine,
                                        const AnalysisModelAdapter *adapter)
{
  if (engine == NULL) {
    return;
  }
  memset(engine, 0, sizeof(*engine));
  if (adapter == NULL || adapter->analyze_fault_slice == NULL) {
    return;
  }
  engine->adapter = *adapter;
  engine->analyze_fault_slice = analysis_engine_model_adapter_analyze_fault_slice;
  engine->impl_id = ANALYSIS_ENGINE_IMPL_MODEL_ADAPTER;
  engine->impl_name = (adapter->name != NULL && adapter->name[0] != '\0') ? adapter->name : "model-adapter";
}

void analysis_engine_init_model_mock(AnalysisEngine *engine)
{
  AnalysisModelAdapter adapter;
  memset(&adapter, 0, sizeof(adapter));
  adapter.analyze_fault_slice = analysis_model_mock_analyze_fault_slice;
  adapter.ctx = NULL;
  adapter.name = "model-mock";
  analysis_engine_init_model_adapter(engine, &adapter);
  if (engine != NULL) {
    engine->impl_id = ANALYSIS_ENGINE_IMPL_MODEL_MOCK;
  }
}

int analysis_engine_analyze_fault_slice(const AnalysisEngine *engine,
                                        const AnalysisInput *input,
                                        AnalysisResult *out)
{
  if (engine == NULL || engine->analyze_fault_slice == NULL) {
    return -1;
  }
  return engine->analyze_fault_slice(engine, input, out);
}
