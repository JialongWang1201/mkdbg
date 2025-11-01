#include "dependency_graph.h"

#include <string.h>

#define BRINGUP_MANIFEST_INCLUDE_DEPENDENCY_SECTION
#include "bringup_manifest_gen.h"

const DepDriverEdge *dependency_graph_driver_edges(uint32_t *out_count)
{
  if (out_count != NULL) {
    *out_count = BRINGUP_MANIFEST_DEP_DRIVER_EDGE_COUNT;
  }
  return bringup_manifest_dep_driver_edges;
}

const DepResourceEdge *dependency_graph_resource_edges(uint32_t *out_count)
{
  if (out_count != NULL) {
    *out_count = BRINGUP_MANIFEST_DEP_RESOURCE_EDGE_COUNT;
  }
  return bringup_manifest_dep_resource_edges;
}

const DepStageDriverEdge *dependency_graph_stage_driver_edges(uint32_t *out_count)
{
  if (out_count != NULL) {
    *out_count = BRINGUP_MANIFEST_DEP_STAGE_DRIVER_EDGE_COUNT;
  }
  return bringup_manifest_dep_stage_driver_edges;
}

const char *dependency_graph_resource_kind_name(DepResourceKind kind)
{
  switch (kind) {
    case DEP_RESOURCE_IRQ:
      return "irq";
    case DEP_RESOURCE_DMA:
      return "dma";
    case DEP_RESOURCE_MEMORY:
      return "memory";
    default:
      return "unknown";
  }
}

const char *dependency_graph_action_name(DepHypotheticalAction action)
{
  switch (action) {
    case DEP_ACTION_RESET:
      return "reset";
    case DEP_ACTION_THROTTLE:
      return "throttle";
    case DEP_ACTION_DENY:
      return "deny";
    default:
      return "unknown";
  }
}

int dependency_graph_parse_action(const char *text, DepHypotheticalAction *out_action)
{
  if (text == NULL || out_action == NULL) {
    return 0;
  }
  if (strcmp(text, "reset") == 0) {
    *out_action = DEP_ACTION_RESET;
    return 1;
  }
  if (strcmp(text, "throttle") == 0) {
    *out_action = DEP_ACTION_THROTTLE;
    return 1;
  }
  if (strcmp(text, "deny") == 0) {
    *out_action = DEP_ACTION_DENY;
    return 1;
  }
  return 0;
}

uint32_t dependency_graph_stage_drivers(BringupStageId stage,
                                        KdiDriverId *out_drivers,
                                        const char **out_reasons,
                                        uint32_t out_cap)
{
  uint8_t seen[KDI_DRIVER_COUNT] = {0};
  uint32_t out_count = 0U;

  for (uint32_t i = 0U; i < BRINGUP_MANIFEST_DEP_STAGE_DRIVER_EDGE_COUNT; ++i) {
    const DepStageDriverEdge *edge = &bringup_manifest_dep_stage_driver_edges[i];
    KdiDriverId driver = edge->driver;

    if (bringup_stage_from_phase(edge->stage) != stage) {
      continue;
    }
    if ((uint32_t)driver >= (uint32_t)KDI_DRIVER_COUNT) {
      continue;
    }
    if (seen[(uint32_t)driver] != 0U) {
      continue;
    }
    seen[(uint32_t)driver] = 1U;

    if (out_drivers != NULL && out_count < out_cap) {
      out_drivers[out_count] = driver;
    }
    if (out_reasons != NULL && out_count < out_cap) {
      out_reasons[out_count] = edge->reason;
    }
    out_count++;
  }
  return out_count;
}

uint32_t dependency_graph_driver_stages(KdiDriverId driver,
                                        BringupStageId *out_stages,
                                        BringupPhaseId *out_phases,
                                        const char **out_reasons,
                                        uint32_t out_cap)
{
  uint8_t seen[BRINGUP_STAGE_COUNT] = {0};
  uint32_t out_count = 0U;

  if ((uint32_t)driver >= (uint32_t)KDI_DRIVER_COUNT) {
    return 0U;
  }

  for (uint32_t i = 0U; i < BRINGUP_MANIFEST_DEP_STAGE_DRIVER_EDGE_COUNT; ++i) {
    const DepStageDriverEdge *edge = &bringup_manifest_dep_stage_driver_edges[i];
    BringupStageId stage;

    if (edge->driver != driver) {
      continue;
    }

    stage = bringup_stage_from_phase(edge->stage);
    if ((uint32_t)stage >= (uint32_t)BRINGUP_STAGE_COUNT) {
      continue;
    }
    if (seen[(uint32_t)stage] != 0U) {
      continue;
    }
    seen[(uint32_t)stage] = 1U;

    if (out_stages != NULL && out_count < out_cap) {
      out_stages[out_count] = stage;
    }
    if (out_phases != NULL && out_count < out_cap) {
      out_phases[out_count] = edge->stage;
    }
    if (out_reasons != NULL && out_count < out_cap) {
      out_reasons[out_count] = edge->reason;
    }
    out_count++;
  }

  return out_count;
}

uint32_t dependency_graph_action_impact(DepHypotheticalAction action,
                                        KdiDriverId target,
                                        KdiDriverId *out_drivers,
                                        uint8_t *out_depth,
                                        KdiDriverId *out_via,
                                        uint32_t out_cap)
{
  uint8_t seen[KDI_DRIVER_COUNT] = {0};
  uint8_t depth[KDI_DRIVER_COUNT];
  KdiDriverId via[KDI_DRIVER_COUNT];
  KdiDriverId queue[KDI_DRIVER_COUNT];
  uint32_t q_head = 0U;
  uint32_t q_tail = 0U;
  uint32_t edge_count = 0U;
  uint32_t out_count = 0U;
  const DepDriverEdge *edges = dependency_graph_driver_edges(&edge_count);

  if ((uint32_t)target >= (uint32_t)KDI_DRIVER_COUNT) {
    return 0U;
  }
  if (action != DEP_ACTION_RESET &&
      action != DEP_ACTION_THROTTLE &&
      action != DEP_ACTION_DENY) {
    return 0U;
  }

  for (uint32_t i = 0U; i < (uint32_t)KDI_DRIVER_COUNT; ++i) {
    depth[i] = 0xFFU;
    via[i] = KDI_DRIVER_COUNT;
  }

  seen[(uint32_t)target] = 1U;
  depth[(uint32_t)target] = 0U;
  via[(uint32_t)target] = KDI_DRIVER_COUNT;
  queue[q_tail++] = target;

  while (q_head < q_tail) {
    KdiDriverId current = queue[q_head++];
    for (uint32_t i = 0U; i < edge_count; ++i) {
      const DepDriverEdge *e = &edges[i];
      KdiDriverId dependent;

      if (e->to != current) {
        continue;
      }
      dependent = e->from;
      if ((uint32_t)dependent >= (uint32_t)KDI_DRIVER_COUNT) {
        continue;
      }
      if (seen[(uint32_t)dependent] != 0U) {
        continue;
      }
      seen[(uint32_t)dependent] = 1U;
      depth[(uint32_t)dependent] = (uint8_t)(depth[(uint32_t)current] + 1U);
      via[(uint32_t)dependent] = current;
      queue[q_tail++] = dependent;
    }
  }

  for (uint32_t level = 0U; level <= (uint32_t)KDI_DRIVER_COUNT; ++level) {
    for (uint32_t i = 0U; i < (uint32_t)KDI_DRIVER_COUNT; ++i) {
      if (seen[i] == 0U || depth[i] != level) {
        continue;
      }
      if (out_drivers != NULL && out_count < out_cap) {
        out_drivers[out_count] = (KdiDriverId)i;
      }
      if (out_depth != NULL && out_count < out_cap) {
        out_depth[out_count] = depth[i];
      }
      if (out_via != NULL && out_count < out_cap) {
        out_via[out_count] = via[i];
      }
      out_count++;
    }
  }

  return out_count;
}

uint32_t dependency_graph_driver_resources(KdiDriverId driver,
                                           const DepResourceEdge **out_edges,
                                           uint32_t out_cap)
{
  uint32_t out_count = 0U;

  if ((uint32_t)driver >= (uint32_t)KDI_DRIVER_COUNT) {
    return 0U;
  }

  for (uint32_t i = 0U; i < BRINGUP_MANIFEST_DEP_RESOURCE_EDGE_COUNT; ++i) {
    if (bringup_manifest_dep_resource_edges[i].driver != driver) {
      continue;
    }
    if (out_edges != NULL && out_count < out_cap) {
      out_edges[out_count] = &bringup_manifest_dep_resource_edges[i];
    }
    out_count++;
  }
  return out_count;
}

uint32_t dependency_graph_reset_impact(KdiDriverId target,
                                       KdiDriverId *out_drivers,
                                       uint32_t out_cap)
{
  KdiDriverId impacted[KDI_DRIVER_COUNT];
  uint8_t depth[KDI_DRIVER_COUNT];
  uint32_t impacted_count =
    dependency_graph_action_impact(DEP_ACTION_RESET,
                                   target,
                                   impacted,
                                   depth,
                                   NULL,
                                   (uint32_t)KDI_DRIVER_COUNT);
  uint32_t out_count = 0U;

  for (uint32_t i = 0U; i < impacted_count && i < (uint32_t)KDI_DRIVER_COUNT; ++i) {
    if (depth[i] == 0U) {
      continue;
    }
    if (out_drivers != NULL && out_count < out_cap && i < (uint32_t)KDI_DRIVER_COUNT) {
      out_drivers[out_count] = impacted[i];
    }
    out_count++;
  }
  return out_count;
}
