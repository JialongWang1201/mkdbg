#ifndef VM32_SCENARIOS_H
#define VM32_SCENARIOS_H

#include <stdint.h>
#include "vm32.h"

#define VM32_SCENARIO_FLAG_NO_YIELD (1U << 0)
#define VM32_SCENARIO_FLAG_CRITICAL (1U << 1)

typedef struct {
  const char *name;
  const char *desc;
  const uint8_t *program;
  uint32_t size;
  uint32_t entry;
  uint32_t max_steps;
  uint32_t flags;
  void (*host_start)(void);
} Vm32Scenario;

const Vm32Scenario *vm32_scenario_find(const char *name);
void vm32_scenario_list(void (*write)(const char *));
Vm32Result vm32_scenario_load(Vm32 *vm, const Vm32Scenario *scenario);

#endif
