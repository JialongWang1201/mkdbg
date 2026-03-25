#ifndef DEPENDENCY_GRAPH_H
#define DEPENDENCY_GRAPH_H

#include <stdint.h>

#include "bringup_phase.h"
#include "kdi.h"

typedef enum {
  DEP_RESOURCE_IRQ = 1,
  DEP_RESOURCE_DMA = 2,
  DEP_RESOURCE_MEMORY = 3
} DepResourceKind;

typedef enum {
  DEP_ACTION_RESET = 1,
  DEP_ACTION_THROTTLE = 2,
  DEP_ACTION_DENY = 3
} DepHypotheticalAction;

typedef struct {
  KdiDriverId from;
  KdiDriverId to;
  const char *reason;
} DepDriverEdge;

typedef struct {
  KdiDriverId driver;
  DepResourceKind kind;
  const char *resource_id;
  const char *reason;
} DepResourceEdge;

typedef struct {
  BringupPhaseId stage;
  KdiDriverId driver;
  const char *reason;
} DepStageDriverEdge;

const DepDriverEdge *dependency_graph_driver_edges(uint32_t *out_count);
const DepResourceEdge *dependency_graph_resource_edges(uint32_t *out_count);
const DepStageDriverEdge *dependency_graph_stage_driver_edges(uint32_t *out_count);
const char *dependency_graph_resource_kind_name(DepResourceKind kind);
const char *dependency_graph_action_name(DepHypotheticalAction action);
int dependency_graph_parse_action(const char *text, DepHypotheticalAction *out_action);
uint32_t dependency_graph_stage_drivers(BringupStageId stage,
                                        KdiDriverId *out_drivers,
                                        const char **out_reasons,
                                        uint32_t out_cap);
uint32_t dependency_graph_driver_resources(KdiDriverId driver,
                                           const DepResourceEdge **out_edges,
                                           uint32_t out_cap);
uint32_t dependency_graph_driver_stages(KdiDriverId driver,
                                        BringupStageId *out_stages,
                                        BringupPhaseId *out_phases,
                                        const char **out_reasons,
                                        uint32_t out_cap);
uint32_t dependency_graph_action_impact(DepHypotheticalAction action,
                                        KdiDriverId target,
                                        KdiDriverId *out_drivers,
                                        uint8_t *out_depth,
                                        KdiDriverId *out_via,
                                        uint32_t out_cap);
uint32_t dependency_graph_reset_impact(KdiDriverId target,
                                       KdiDriverId *out_drivers,
                                       uint32_t out_cap);

#endif
