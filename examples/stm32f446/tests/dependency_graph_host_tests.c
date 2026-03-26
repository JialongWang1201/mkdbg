#include <stdio.h>

#include "dependency_graph.h"

static int failures = 0;

static void check(int cond, const char *msg)
{
  if (!cond) {
    printf("FAIL: %s\n", msg);
    failures++;
  }
}

static int find_driver_index(const KdiDriverId *drivers, uint32_t count, KdiDriverId target)
{
  for (uint32_t i = 0U; i < count; ++i) {
    if (drivers[i] == target) {
      return (int)i;
    }
  }
  return -1;
}

static void test_action_parse_and_names(void)
{
  DepHypotheticalAction action = DEP_ACTION_RESET;

  check(dependency_graph_parse_action("reset", &action) == 1 && action == DEP_ACTION_RESET,
        "parse reset action");
  check(dependency_graph_parse_action("throttle", &action) == 1 && action == DEP_ACTION_THROTTLE,
        "parse throttle action");
  check(dependency_graph_parse_action("deny", &action) == 1 && action == DEP_ACTION_DENY,
        "parse deny action");
  check(dependency_graph_parse_action("bad", &action) == 0,
        "reject invalid action");
  check(dependency_graph_parse_action(NULL, &action) == 0,
        "reject null action text");

  check(dependency_graph_action_name(DEP_ACTION_RESET) != NULL, "action name reset");
  check(dependency_graph_action_name(DEP_ACTION_THROTTLE) != NULL, "action name throttle");
  check(dependency_graph_action_name(DEP_ACTION_DENY) != NULL, "action name deny");
}

static void test_action_impact_depth_and_chain(void)
{
  KdiDriverId impacted[KDI_DRIVER_COUNT];
  uint8_t depth[KDI_DRIVER_COUNT] = {0};
  KdiDriverId via[KDI_DRIVER_COUNT];
  for (uint32_t i = 0U; i < (uint32_t)KDI_DRIVER_COUNT; ++i) {
    via[i] = KDI_DRIVER_COUNT;
  }
  uint32_t count =
    dependency_graph_action_impact(DEP_ACTION_RESET,
                                   KDI_DRIVER_DIAG,
                                   impacted,
                                   depth,
                                   via,
                                   (uint32_t)KDI_DRIVER_COUNT);
  int idx_diag;
  int idx_uart;
  int idx_vm;
  int idx_sensor;

  check(count == 4U, "diag reset impact count includes target + 3 dependents");

  idx_diag = find_driver_index(impacted, count, KDI_DRIVER_DIAG);
  idx_uart = find_driver_index(impacted, count, KDI_DRIVER_UART);
  idx_vm = find_driver_index(impacted, count, KDI_DRIVER_VM_RUNTIME);
  idx_sensor = find_driver_index(impacted, count, KDI_DRIVER_SENSOR);

  check(idx_diag >= 0, "impact includes diag target");
  check(idx_uart >= 0, "impact includes uart direct dependent");
  check(idx_vm >= 0, "impact includes vm direct dependent");
  check(idx_sensor >= 0, "impact includes sensor secondary dependent");

  if (idx_diag >= 0) {
    check(depth[(uint32_t)idx_diag] == 0U, "diag depth 0");
  }
  if (idx_uart >= 0) {
    check(depth[(uint32_t)idx_uart] == 1U, "uart depth 1");
    check(via[(uint32_t)idx_uart] == KDI_DRIVER_DIAG, "uart via diag");
  }
  if (idx_vm >= 0) {
    check(depth[(uint32_t)idx_vm] == 1U, "vm depth 1");
    check(via[(uint32_t)idx_vm] == KDI_DRIVER_DIAG, "vm via diag");
  }
  if (idx_sensor >= 0) {
    check(depth[(uint32_t)idx_sensor] == 2U, "sensor depth 2");
    check(via[(uint32_t)idx_sensor] == KDI_DRIVER_UART, "sensor via uart");
  }
}

static void test_stage_mapping_and_reset_compat(void)
{
  BringupStageId stages[BRINGUP_STAGE_COUNT];
  BringupPhaseId phases[BRINGUP_STAGE_COUNT];
  const char *reasons[BRINGUP_STAGE_COUNT];
  KdiDriverId impacted[KDI_DRIVER_COUNT];
  uint32_t stage_count =
    dependency_graph_driver_stages(KDI_DRIVER_KERNEL,
                                   stages,
                                   phases,
                                   reasons,
                                   (uint32_t)BRINGUP_STAGE_COUNT);
  uint32_t impact_count =
    dependency_graph_reset_impact(KDI_DRIVER_DIAG,
                                  impacted,
                                  (uint32_t)KDI_DRIVER_COUNT);

  check(stage_count == 1U, "kernel maps to exactly one stage");
  if (stage_count >= 1U) {
    check(stages[0] == BRINGUP_STAGE_KERNEL, "kernel stage mapping");
    check(phases[0] == BRINGUP_PHASE_KERNEL_START, "kernel phase mapping");
    check(reasons[0] != NULL, "kernel stage reason present");
  }

  check(impact_count == 3U, "reset impact remains target-excluded");
  check(find_driver_index(impacted, impact_count, KDI_DRIVER_UART) >= 0, "reset impact includes uart");
  check(find_driver_index(impacted, impact_count, KDI_DRIVER_VM_RUNTIME) >= 0, "reset impact includes vm");
  check(find_driver_index(impacted, impact_count, KDI_DRIVER_SENSOR) >= 0, "reset impact includes sensor");
}

int main(void)
{
  test_action_parse_and_names();
  test_action_impact_depth_and_chain();
  test_stage_mapping_and_reset_compat();

  if (failures == 0) {
    printf("dependency_graph_host_tests: OK\n");
    return 0;
  }
  printf("dependency_graph_host_tests: FAIL (%d)\n", failures);
  return 1;
}
