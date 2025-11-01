#include <stdio.h>
#include <string.h>

#include "analysis_engine.h"
#include "bringup_phase.h"

#define EVT_BOUNDARY_FAULT 0x0001U
#define EVT_BOUNDARY_STAGE 0x0004U

static int failures = 0;

static void check(int cond, const char *msg)
{
  if (!cond) {
    printf("FAIL: %s\n", msg);
    failures++;
  }
}

static const AnalysisHypothesisResult *find_hypothesis(const AnalysisResult *result, const char *name)
{
  if (result == NULL || name == NULL) {
    return NULL;
  }
  for (uint8_t i = 0U; i < result->hypothesis_count; ++i) {
    if (result->hypotheses[i].name != NULL && strcmp(result->hypotheses[i].name, name) == 0) {
      return &result->hypotheses[i];
    }
  }
  return NULL;
}

static void test_permission_violation_classification(void)
{
  AnalysisEngine engine = {0};
  AnalysisResult result = {0};
  AnalysisEvent events[] = {
    {1000U, EVT_BOUNDARY_STAGE, 1U, (uint8_t)BRINGUP_PHASE_DRIVER_PROBE_UART, "bringup phase=driver-probe-uart"},
    {2200U, 0U, 0U, 0U, "kdi cap req=irq rc=denied fail=1"},
    {2600U, 0U, 0U, 0U, "Fault(cpu:MemManage) mpu_region=3"},
    {2800U, EVT_BOUNDARY_FAULT, 0U, 0U, "fault driver=uart"},
  };
  AnalysisEventSlice slice = {
    .begin = 0U,
    .end = 3U,
    .stage_valid = 1U,
    .stage_id = (uint8_t)BRINGUP_PHASE_DRIVER_PROBE_UART,
    .fault_events = 1U,
    .reset_events = 0U,
    .start_reason = 0U,
    .corr_id = 0x21000001UL,
  };
  AnalysisFaultFeatureVector vec = {
    .fault_found = 1U,
    .driver_valid = 1U,
    .driver_id = 1U,
    .kdi_fail_frequent = 1U,
    .cap_eperm = 1U,
    .cap_mpu_violation = 1U,
    .fault_ts_ms = 2800U,
    .lookback_ms = 5000U,
  };
  AnalysisInput input = {
    .events = events,
    .event_count = (uint8_t)(sizeof(events) / sizeof(events[0])),
    .slice = &slice,
    .features = &vec,
    .boot_complete = 1U,
  };
  const AnalysisHypothesisResult *cap_hyp = NULL;

  analysis_engine_init_rule_based(&engine);
  check(analysis_engine_analyze_fault_slice(&engine, &input, &result) == 0,
        "permission scenario analyze success");

  check(result.failure.category == ANALYSIS_FAILURE_PERMISSION_VIOLATION,
        "permission scenario classified as permission_violation");
  check(result.failure.confidence >= 60U,
        "permission scenario confidence is high");
  check(result.failure.evidence_count > 0U,
        "permission scenario has evidence");
  check(result.hypothesis_count == ANALYSIS_HYPOTHESIS_CAP,
        "all five hypotheses emitted");

  cap_hyp = find_hypothesis(&result, "capability_violation");
  check(cap_hyp != NULL, "capability_violation hypothesis exists");
  if (cap_hyp != NULL) {
    check(cap_hyp->confidence >= 50U,
          "capability_violation confidence is elevated");
    check(cap_hyp->evidence_count > 0U,
          "capability_violation has evidence");
  }
}

static void test_timeout_classification(void)
{
  AnalysisEngine engine = {0};
  AnalysisResult result = {0};
  AnalysisEvent events[] = {
    {500U, EVT_BOUNDARY_STAGE, 1U, (uint8_t)BRINGUP_PHASE_SERVICE_REGISTRATION, "bringup phase=service-registration"},
    {1200U, 0U, 0U, 0U, "service wait queue_depth=1"},
    {1600U, 0U, 0U, 0U, "operation timeout waiting sensor ready"},
    {1700U, EVT_BOUNDARY_FAULT, 0U, 0U, "fault driver=sensor"},
  };
  AnalysisEventSlice slice = {
    .begin = 0U,
    .end = 3U,
    .stage_valid = 1U,
    .stage_id = (uint8_t)BRINGUP_PHASE_SERVICE_REGISTRATION,
    .fault_events = 1U,
    .reset_events = 0U,
    .start_reason = 0U,
    .corr_id = 0x21000002UL,
  };
  AnalysisFaultFeatureVector vec = {
    .fault_found = 1U,
    .fault_ts_ms = 1700U,
    .lookback_ms = 5000U,
  };
  AnalysisInput input = {
    .events = events,
    .event_count = (uint8_t)(sizeof(events) / sizeof(events[0])),
    .slice = &slice,
    .features = &vec,
    .boot_complete = 1U,
  };

  analysis_engine_init_rule_based(&engine);
  check(analysis_engine_analyze_fault_slice(&engine, &input, &result) == 0,
        "timeout scenario analyze success");

  check(result.failure.category == ANALYSIS_FAILURE_TIMEOUT,
        "timeout scenario classified as timeout");
  check(result.failure.evidence_count > 0U,
        "timeout scenario has evidence");
}

static void test_missing_dependency_classification(void)
{
  AnalysisEngine engine = {0};
  AnalysisResult result = {0};
  AnalysisEvent events[] = {
    {600U, EVT_BOUNDARY_STAGE, 1U, (uint8_t)BRINGUP_PHASE_DRIVER_PROBE_DIAG, "bringup phase=driver-probe-diag"},
    {1000U, 0U, 0U, 0U, "dep uart waiting previous phase done blocked=1"},
    {1300U, 0U, 0U, 0U, "dependency missing stage-wait blocked=1"},
  };
  AnalysisEventSlice slice = {
    .begin = 0U,
    .end = 2U,
    .stage_valid = 1U,
    .stage_id = (uint8_t)BRINGUP_PHASE_DRIVER_PROBE_DIAG,
    .fault_events = 0U,
    .reset_events = 0U,
    .start_reason = 0U,
    .corr_id = 0x21000004UL,
  };
  AnalysisFaultFeatureVector vec = {
    .fault_found = 0U,
    .fault_ts_ms = 1300U,
    .lookback_ms = 5000U,
  };
  AnalysisInput input = {
    .events = events,
    .event_count = (uint8_t)(sizeof(events) / sizeof(events[0])),
    .slice = &slice,
    .features = &vec,
    .boot_complete = 1U,
  };

  analysis_engine_init_rule_based(&engine);
  check(analysis_engine_analyze_fault_slice(&engine, &input, &result) == 0,
        "dependency scenario analyze success");

  check(result.failure.category == ANALYSIS_FAILURE_MISSING_DEPENDENCY,
        "dependency scenario classified as missing_dependency");
  check(result.failure.confidence >= 50U,
        "dependency scenario confidence is elevated");
}

static void test_invalid_input_rejected(void)
{
  AnalysisEngine engine = {0};
  AnalysisResult result = {0};
  AnalysisEvent event = {0U, EVT_BOUNDARY_FAULT, 0U, 0U, "fault"};
  AnalysisEventSlice slice = {0U, 0U, 0U, 0U, 1U, 0U, 0U, 1U};
  AnalysisFaultFeatureVector vec = {
    .fault_found = 1U,
    .fault_ts_ms = 0U,
    .lookback_ms = 1000U,
  };
  AnalysisInput valid = {
    .events = &event,
    .event_count = 1U,
    .slice = &slice,
    .features = &vec,
    .boot_complete = 1U,
  };

  check(analysis_engine_analyze_fault_slice(&engine, &valid, &result) != 0,
        "engine must be initialized before analyze");

  analysis_engine_init_rule_based(&engine);

  check(analysis_engine_analyze_fault_slice(&engine, NULL, &result) != 0,
        "reject null input");
  check(analysis_engine_analyze_fault_slice(&engine, &valid, NULL) != 0,
        "reject null output");

  valid.events = NULL;
  check(analysis_engine_analyze_fault_slice(&engine, &valid, &result) != 0,
        "reject null event array");
}

typedef struct {
  uint8_t called;
  AnalysisFailureCategory category;
} AdapterTestCtx;

static int adapter_test_analyze(const AnalysisInput *input,
                                AnalysisResult *out,
                                void *ctx_ptr)
{
  AdapterTestCtx *ctx = (AdapterTestCtx *)ctx_ptr;
  if (input == NULL || out == NULL || ctx == NULL) {
    return -1;
  }
  memset(out, 0, sizeof(*out));
  ctx->called = 1U;
  out->failure.category = ctx->category;
  out->failure.confidence = 91U;
  out->failure.evidence_count = 1U;
  out->failure.evidence_ids[0] = input->slice->end;
  (void)snprintf(out->failure.explain_p1,
                 sizeof(out->failure.explain_p1),
                 "adapter category=%s",
                 analysis_failure_category_name(ctx->category));
  out->hypothesis_count = 1U;
  out->hypotheses[0].name = "resource_exhaustion";
  out->hypotheses[0].confidence = 77U;
  out->hypotheses[0].evidence_count = 1U;
  out->hypotheses[0].evidence_ids[0] = input->slice->end;
  return 0;
}

static void test_model_adapter_init_and_dispatch(void)
{
  AnalysisEngine engine = {0};
  AnalysisResult result = {0};
  AnalysisEvent event = {0U, EVT_BOUNDARY_FAULT, 0U, 0U, "fault"};
  AnalysisEventSlice slice = {0U, 0U, 0U, 0U, 1U, 0U, 0U, 1U};
  AnalysisFaultFeatureVector vec = {
    .fault_found = 1U,
    .fault_ts_ms = 0U,
    .lookback_ms = 1000U,
  };
  AnalysisInput input = {
    .events = &event,
    .event_count = 1U,
    .slice = &slice,
    .features = &vec,
    .boot_complete = 1U,
  };
  AdapterTestCtx ctx = {
    .called = 0U,
    .category = ANALYSIS_FAILURE_TIMEOUT,
  };
  AnalysisModelAdapter adapter = {
    .analyze_fault_slice = adapter_test_analyze,
    .ctx = &ctx,
    .name = "unit-adapter",
  };

  analysis_engine_init_model_adapter(&engine, &adapter);

  check(engine.impl_id != 0U, "adapter init sets impl id");
  check(engine.impl_name != NULL && strcmp(engine.impl_name, "unit-adapter") == 0,
        "adapter init sets impl name");
  check(analysis_engine_analyze_fault_slice(&engine, &input, &result) == 0,
        "adapter analyze success");
  check(ctx.called == 1U, "adapter callback invoked");
  check(result.failure.category == ANALYSIS_FAILURE_TIMEOUT,
        "adapter result category propagated");
  check(result.hypothesis_count == 1U,
        "adapter hypothesis count propagated");
}

static void test_model_mock_passthrough(void)
{
  AnalysisEngine engine = {0};
  AnalysisResult result = {0};
  AnalysisEvent events[] = {
    {500U, EVT_BOUNDARY_STAGE, 1U, (uint8_t)BRINGUP_PHASE_SERVICE_REGISTRATION, "bringup phase=service-registration"},
    {1600U, 0U, 0U, 0U, "operation timeout waiting sensor ready"},
    {1700U, EVT_BOUNDARY_FAULT, 0U, 0U, "fault driver=sensor"},
  };
  AnalysisEventSlice slice = {
    .begin = 0U,
    .end = 2U,
    .stage_valid = 1U,
    .stage_id = (uint8_t)BRINGUP_PHASE_SERVICE_REGISTRATION,
    .fault_events = 1U,
    .reset_events = 0U,
    .start_reason = 0U,
    .corr_id = 0x22000002UL,
  };
  AnalysisFaultFeatureVector vec = {
    .fault_found = 1U,
    .fault_ts_ms = 1700U,
    .lookback_ms = 5000U,
  };
  AnalysisInput input = {
    .events = events,
    .event_count = (uint8_t)(sizeof(events) / sizeof(events[0])),
    .slice = &slice,
    .features = &vec,
    .boot_complete = 1U,
  };

  analysis_engine_init_model_mock(&engine);
  check(engine.impl_id != 0U, "model mock init sets impl id");
  check(engine.impl_name != NULL && strcmp(engine.impl_name, "model-mock") == 0,
        "model mock init sets impl name");
  check(analysis_engine_analyze_fault_slice(&engine, &input, &result) == 0,
        "model mock analyze success");
  check(strstr(result.failure.explain_p2, "Model mock adapter pass-through") != NULL,
        "model mock writes pass-through note");
}

int main(void)
{
  test_permission_violation_classification();
  test_timeout_classification();
  test_missing_dependency_classification();
  test_invalid_input_rejected();
  test_model_adapter_init_and_dispatch();
  test_model_mock_passthrough();

  if (failures == 0) {
    printf("analysis_engine_host_tests: OK\n");
    return 0;
  }
  printf("analysis_engine_host_tests: FAIL (%d)\n", failures);
  return 1;
}
