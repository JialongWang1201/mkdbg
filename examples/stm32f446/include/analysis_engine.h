#ifndef ANALYSIS_ENGINE_H
#define ANALYSIS_ENGINE_H

#include <stdint.h>

#define ANALYSIS_EVIDENCE_CAP 6U
#define ANALYSIS_HYPOTHESIS_CAP 5U
#define ANALYSIS_EXPLAIN_TEXT_MAX 112U

typedef struct AnalysisEngine AnalysisEngine;

typedef enum {
  ANALYSIS_FAILURE_ORDERING_VIOLATION = 0,
  ANALYSIS_FAILURE_MISSING_DEPENDENCY = 1,
  ANALYSIS_FAILURE_TIMEOUT = 2,
  ANALYSIS_FAILURE_PERMISSION_VIOLATION = 3,
  ANALYSIS_FAILURE_EARLY_RESOURCE_FAULT = 4,
  ANALYSIS_FAILURE_CATEGORY_COUNT
} AnalysisFailureCategory;

typedef struct {
  uint8_t fault_found;
  uint8_t driver_valid;
  uint8_t driver_id;
  uint8_t irq_rate_abnormal;
  uint8_t dma_full;
  uint8_t dma_idle_spin;
  uint8_t kdi_fail_frequent;
  uint8_t state_error_reset_loop;
  uint8_t cap_eperm;
  uint8_t cap_mpu_violation;
  uint16_t lookback_event_count;
  uint16_t irq_event_count;
  uint16_t kdi_fail_count;
  uint16_t state_loop_transitions;
  uint16_t irq_rate_per_sec;
  uint16_t irq_deferred_pending;
  uint32_t fault_ts_ms;
  uint32_t lookback_ms;
} AnalysisFaultFeatureVector;

typedef struct {
  uint32_t ts_ms;
  uint16_t boundary_flags;
  uint8_t stage_valid;
  uint8_t stage_id;
  const char *msg;
} AnalysisEvent;

typedef struct {
  uint8_t begin;
  uint8_t end;
  uint8_t stage_valid;
  uint8_t stage_id;
  uint8_t fault_events;
  uint8_t reset_events;
  uint16_t start_reason;
  uint32_t corr_id;
} AnalysisEventSlice;

typedef struct {
  const char *name;
  uint8_t confidence;
  uint8_t evidence_ids[ANALYSIS_EVIDENCE_CAP];
  uint8_t evidence_count;
  char explain_p1[ANALYSIS_EXPLAIN_TEXT_MAX];
  char explain_p2[ANALYSIS_EXPLAIN_TEXT_MAX];
} AnalysisHypothesisResult;

typedef struct {
  AnalysisFailureCategory category;
  uint8_t confidence;
  uint8_t evidence_ids[ANALYSIS_EVIDENCE_CAP];
  uint8_t evidence_count;
  char explain_p1[ANALYSIS_EXPLAIN_TEXT_MAX];
  char explain_p2[ANALYSIS_EXPLAIN_TEXT_MAX];
} AnalysisFailureResult;

typedef struct {
  const AnalysisEvent *events;
  uint8_t event_count;
  const AnalysisEventSlice *slice;
  const AnalysisFaultFeatureVector *features;
  uint8_t boot_complete;
} AnalysisInput;

typedef struct {
  AnalysisFailureResult failure;
  AnalysisHypothesisResult hypotheses[ANALYSIS_HYPOTHESIS_CAP];
  uint8_t hypothesis_count;
} AnalysisResult;

typedef int (*AnalysisModelAdapterAnalyzeFaultSliceFn)(const AnalysisInput *input,
                                                       AnalysisResult *out,
                                                       void *ctx);

typedef struct {
  AnalysisModelAdapterAnalyzeFaultSliceFn analyze_fault_slice;
  void *ctx;
  const char *name;
} AnalysisModelAdapter;

typedef int (*AnalysisEngineAnalyzeFaultSliceFn)(const AnalysisEngine *engine,
                                                 const AnalysisInput *input,
                                                 AnalysisResult *out);

struct AnalysisEngine {
  AnalysisEngineAnalyzeFaultSliceFn analyze_fault_slice;
  AnalysisModelAdapter adapter;
  const char *impl_name;
  uint8_t impl_id;
  uint8_t reserved0;
  uint8_t reserved1;
  uint8_t reserved2;
};

void analysis_engine_init_rule_based(AnalysisEngine *engine);
void analysis_engine_init_model_adapter(AnalysisEngine *engine,
                                        const AnalysisModelAdapter *adapter);
void analysis_engine_init_model_mock(AnalysisEngine *engine);
int analysis_engine_analyze_fault_slice(const AnalysisEngine *engine,
                                        const AnalysisInput *input,
                                        AnalysisResult *out);
const char *analysis_failure_category_name(AnalysisFailureCategory category);

#endif
