#ifndef SCENARIO_H
#define SCENARIO_H

#include <stdint.h>
#include "vm32.h"

typedef enum {
  SCENARIO_STATUS_OK = 0,
  SCENARIO_STATUS_HALTED = 1,
  SCENARIO_STATUS_DISPATCHED = 2,
  SCENARIO_STATUS_NOT_FOUND = 3,
  SCENARIO_STATUS_BAD_ARG = 4,
  SCENARIO_STATUS_LOAD_FAILED = 5,
  SCENARIO_STATUS_RUNTIME_ERROR = 6,
  SCENARIO_STATUS_VM_FAULT = 7,
  SCENARIO_STATUS_CFG_REJECTED = 8
} ScenarioStatus;

typedef struct {
  ScenarioStatus status;
  Vm32Result vm_result;
  uint32_t steps;
  uint8_t vm_fault;
  const char *name;
} ScenarioResult;

void scenario_list(void (*write)(const char *));
ScenarioStatus scenario_status_from_vm_result(Vm32Result vm_result);
ScenarioResult scenario_run(Vm32 *vm, const char *name);
const char *scenario_status_name(ScenarioStatus status);

#endif
