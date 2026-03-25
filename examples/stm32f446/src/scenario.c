#include "scenario.h"
#include "vm32_scenarios.h"
#include "FreeRTOS.h"
#include "task.h"

static Vm32Result scenario_run_loop(Vm32 *vm, uint32_t max_steps, uint8_t allow_yield, uint32_t *steps)
{
  Vm32Result res = VM32_OK;
  uint32_t i = 0U;

  for (; i < max_steps; ++i) {
    res = vm32_step(vm);
    if (res != VM32_OK) {
      break;
    }
    if (allow_yield && (i & 0x3FFU) == 0x3FFU) {
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }

  if (steps != NULL) {
    if (res == VM32_OK) {
      *steps = max_steps;
    } else {
      *steps = i + 1U;
    }
  }
  return res;
}

ScenarioStatus scenario_status_from_vm_result(Vm32Result res)
{
  switch (res) {
    case VM32_OK:
      return SCENARIO_STATUS_OK;
    case VM32_ERR_HALT:
      return SCENARIO_STATUS_HALTED;
    case VM32_ERR_STACK:
    case VM32_ERR_MEM:
    case VM32_ERR_POLICY:
      return SCENARIO_STATUS_VM_FAULT;
    default:
      return SCENARIO_STATUS_RUNTIME_ERROR;
  }
}

void scenario_list(void (*write)(const char *))
{
  vm32_scenario_list(write);
}

ScenarioResult scenario_run(Vm32 *vm, const char *name)
{
  ScenarioResult out;
  Vm32CfgReport cfg;
  out.status = SCENARIO_STATUS_BAD_ARG;
  out.vm_result = VM32_ERR_MEM;
  out.steps = 0U;
  out.vm_fault = 0U;
  out.name = name;

  if (vm == NULL || name == NULL) {
    return out;
  }

  const Vm32Scenario *scenario = vm32_scenario_find(name);
  if (scenario == NULL) {
    out.status = SCENARIO_STATUS_NOT_FOUND;
    return out;
  }

  vm32_reset(vm);
  out.vm_result = vm32_scenario_load(vm, scenario);
  if (out.vm_result != VM32_OK) {
    out.status = SCENARIO_STATUS_LOAD_FAILED;
    out.vm_fault = (out.vm_result == VM32_ERR_STACK ||
                    out.vm_result == VM32_ERR_MEM ||
                    out.vm_result == VM32_ERR_POLICY) ? 1U : 0U;
    return out;
  }

  if (scenario->host_start != NULL) {
    scenario->host_start();
  }

  if (scenario->program == NULL || scenario->size == 0U) {
    out.status = SCENARIO_STATUS_DISPATCHED;
    out.vm_result = VM32_OK;
    return out;
  }

  if (scenario->entry >= scenario->size) {
    out.status = SCENARIO_STATUS_CFG_REJECTED;
    out.vm_result = VM32_ERR_CFG;
    out.vm_fault = 0U;
    return out;
  }

  out.vm_result = vm32_verify_bounded_cfg(vm, scenario->entry, scenario->size - scenario->entry, &cfg);
  if (out.vm_result != VM32_OK) {
    out.status = SCENARIO_STATUS_CFG_REJECTED;
    out.vm_fault = 0U;
    return out;
  }

  vm->pc = scenario->entry % VM32_MEM_SIZE;
  if ((scenario->flags & VM32_SCENARIO_FLAG_CRITICAL) != 0U) {
    taskENTER_CRITICAL();
  }
  uint32_t run_max = cfg.max_steps;
  if (scenario->max_steps != 0U && scenario->max_steps < run_max) {
    run_max = scenario->max_steps;
  }
  out.vm_result = scenario_run_loop(
    vm,
    run_max,
    (scenario->flags & VM32_SCENARIO_FLAG_NO_YIELD) == 0U,
    &out.steps
  );
  if ((scenario->flags & VM32_SCENARIO_FLAG_CRITICAL) != 0U) {
    taskEXIT_CRITICAL();
  }

  out.status = scenario_status_from_vm_result(out.vm_result);
  out.vm_fault = (out.vm_result == VM32_ERR_STACK ||
                  out.vm_result == VM32_ERR_MEM ||
                  out.vm_result == VM32_ERR_POLICY) ? 1U : 0U;
  return out;
}

const char *scenario_status_name(ScenarioStatus status)
{
  switch (status) {
    case SCENARIO_STATUS_OK:
      return "ok";
    case SCENARIO_STATUS_HALTED:
      return "halted";
    case SCENARIO_STATUS_DISPATCHED:
      return "dispatched";
    case SCENARIO_STATUS_NOT_FOUND:
      return "not_found";
    case SCENARIO_STATUS_BAD_ARG:
      return "bad_arg";
    case SCENARIO_STATUS_LOAD_FAILED:
      return "load_failed";
    case SCENARIO_STATUS_RUNTIME_ERROR:
      return "runtime_error";
    case SCENARIO_STATUS_VM_FAULT:
      return "vm_fault";
    case SCENARIO_STATUS_CFG_REJECTED:
      return "cfg_rejected";
    default:
      return "unknown";
  }
}
