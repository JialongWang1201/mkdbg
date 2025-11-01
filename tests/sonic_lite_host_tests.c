#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sonic_lite.h"

static int failures = 0;

static void check(int cond, const char *msg)
{
  if (!cond) {
    printf("FAIL: %s\n", msg);
    failures++;
  }
}

static void test_init_defaults(void)
{
  SonicLiteState s;
  const char *v_run = NULL;
  const char *v_cand = NULL;
  sonic_lite_init(&s);

  check(sonic_lite_count(&s, SONIC_DB_CONFIG) >= 3U, "default running count");
  check(sonic_lite_count_candidate(&s, SONIC_DB_CONFIG) >= 3U, "default candidate count");
  check(sonic_lite_candidate_dirty(&s) == 0U, "default not dirty");
  check(sonic_lite_get(&s, SONIC_DB_CONFIG, "hostname", &v_run) == 0, "running hostname exists");
  check(sonic_lite_get_candidate(&s, SONIC_DB_CONFIG, "hostname", &v_cand) == 0, "candidate hostname exists");
  check(v_run != NULL && strcmp(v_run, "mkmpu") == 0, "running hostname value");
  check(v_cand != NULL && strcmp(v_cand, "mkmpu") == 0, "candidate hostname value");
}

static void test_stage_and_commit(void)
{
  SonicLiteState s;
  const char *value = NULL;
  sonic_lite_init(&s);

  check(sonic_lite_set(&s, SONIC_DB_APPL, "policy", "allow") == 0, "stage policy");
  check(sonic_lite_candidate_dirty(&s) == 1U, "dirty after stage");
  check(sonic_lite_diff_count(&s, SONIC_DB_APPL) == 1U, "diff count after stage");
  check(sonic_lite_get(&s, SONIC_DB_APPL, "policy", &value) != 0, "running unchanged before commit");
  check(sonic_lite_get_candidate(&s, SONIC_DB_APPL, "policy", &value) == 0, "candidate has staged value");
  check(value != NULL && strcmp(value, "allow") == 0, "candidate staged value");

  check(sonic_lite_commit(&s) == 0, "commit success");
  check(sonic_lite_candidate_dirty(&s) == 0U, "clean after commit");
  check(sonic_lite_diff_count(&s, SONIC_DB_APPL) == 0U, "no diff after commit");
  check(sonic_lite_get(&s, SONIC_DB_APPL, "policy", &value) == 0, "running updated after commit");
  check(value != NULL && strcmp(value, "allow") == 0, "running committed value");
}

static void test_abort(void)
{
  SonicLiteState s;
  const char *value = NULL;
  sonic_lite_init(&s);

  check(sonic_lite_set(&s, SONIC_DB_CONFIG, "hostname", "mkmpu2") == 0, "stage hostname change");
  check(sonic_lite_candidate_dirty(&s) == 1U, "dirty before abort");
  check(sonic_lite_get_candidate(&s, SONIC_DB_CONFIG, "hostname", &value) == 0, "candidate hostname staged");
  check(value != NULL && strcmp(value, "mkmpu2") == 0, "candidate staged hostname");

  sonic_lite_abort(&s);
  check(sonic_lite_candidate_dirty(&s) == 0U, "clean after abort");
  check(sonic_lite_get_candidate(&s, SONIC_DB_CONFIG, "hostname", &value) == 0, "candidate hostname restored");
  check(value != NULL && strcmp(value, "mkmpu") == 0, "candidate reverted hostname");
}

static void test_list_candidate(void)
{
  SonicLiteState s;
  const char *k = NULL;
  const char *v = NULL;
  sonic_lite_init(&s);

  check(sonic_lite_set(&s, SONIC_DB_ASIC, "port1", "up") == 0, "stage asic port1");
  check(sonic_lite_set(&s, SONIC_DB_ASIC, "port2", "down") == 0, "stage asic port2");
  check(sonic_lite_count(&s, SONIC_DB_ASIC) == 0U, "running asic still empty");
  check(sonic_lite_count_candidate(&s, SONIC_DB_ASIC) == 2U, "candidate asic count");

  check(sonic_lite_list_candidate(&s, SONIC_DB_ASIC, 0, &k, &v) == 0, "list cand index0");
  check(k != NULL && strcmp(k, "port1") == 0, "list cand key0");
  check(v != NULL && strcmp(v, "up") == 0, "list cand val0");
}

static void test_db_full(void)
{
  SonicLiteState s;
  char key[SONIC_LITE_KEY_MAX];
  sonic_lite_init(&s);

  for (uint32_t i = 0U; i < SONIC_LITE_DB_CAP; ++i) {
    snprintf(key, sizeof(key), "k%lu", (unsigned long)i);
    check(sonic_lite_set(&s, SONIC_DB_APPL, key, "v") == 0, "fill candidate db");
  }
  check(sonic_lite_set(&s, SONIC_DB_APPL, "overflow", "v") == -3, "db full code");
}

int main(void)
{
  test_init_defaults();
  test_stage_and_commit();
  test_abort();
  test_list_candidate();
  test_db_full();

  if (failures == 0) {
    printf("sonic_lite_host_tests: OK\n");
    return 0;
  }
  printf("sonic_lite_host_tests: FAIL (%d)\n", failures);
  return 1;
}
