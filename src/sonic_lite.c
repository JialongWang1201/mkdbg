#include "sonic_lite.h"
#include <string.h>

static uint8_t sonic_lite_copy(char *dst, uint32_t max, const char *src)
{
  size_t len;
  if (dst == NULL || src == NULL || max == 0U) {
    return 0U;
  }
  len = strlen(src);
  if (len >= (size_t)max) {
    return 0U;
  }
  memcpy(dst, src, len + 1U);
  return 1U;
}

static SonicLiteDbState *sonic_lite_db_mut_from_plane(SonicLitePlane *plane, SonicLiteDb db)
{
  if (plane == NULL || db >= SONIC_DB_COUNT) {
    return NULL;
  }
  return &plane->db[db];
}

static const SonicLiteDbState *sonic_lite_db_ro_from_plane(const SonicLitePlane *plane, SonicLiteDb db)
{
  if (plane == NULL || db >= SONIC_DB_COUNT) {
    return NULL;
  }
  return &plane->db[db];
}

static const SonicLiteDbState *sonic_lite_db_ro_running(const SonicLiteState *state, SonicLiteDb db)
{
  if (state == NULL) {
    return NULL;
  }
  return sonic_lite_db_ro_from_plane(&state->running, db);
}

static const SonicLiteDbState *sonic_lite_db_ro_candidate(const SonicLiteState *state, SonicLiteDb db)
{
  if (state == NULL) {
    return NULL;
  }
  return sonic_lite_db_ro_from_plane(&state->candidate, db);
}

static int32_t sonic_lite_find_slot(const SonicLiteDbState *d, const char *key)
{
  if (d == NULL || key == NULL) {
    return -1;
  }
  for (uint32_t i = 0U; i < SONIC_LITE_DB_CAP; ++i) {
    if (d->kv[i].used && strcmp(d->kv[i].key, key) == 0) {
      return (int32_t)i;
    }
  }
  return -1;
}

static int sonic_lite_set_db(SonicLiteDbState *d, const char *key, const char *value)
{
  int32_t free_idx = -1;

  if (d == NULL || key == NULL || value == NULL || key[0] == '\0') {
    return -1;
  }

  for (uint32_t i = 0U; i < SONIC_LITE_DB_CAP; ++i) {
    if (d->kv[i].used) {
      if (strcmp(d->kv[i].key, key) == 0) {
        if (!sonic_lite_copy(d->kv[i].value, SONIC_LITE_VALUE_MAX, value)) {
          return -2;
        }
        return 0;
      }
    } else if (free_idx < 0) {
      free_idx = (int32_t)i;
    }
  }

  if (free_idx < 0) {
    return -3;
  }

  if (!sonic_lite_copy(d->kv[free_idx].key, SONIC_LITE_KEY_MAX, key) ||
      !sonic_lite_copy(d->kv[free_idx].value, SONIC_LITE_VALUE_MAX, value)) {
    return -2;
  }
  d->kv[free_idx].used = 1U;
  d->count++;
  return 0;
}

static int sonic_lite_get_db(const SonicLiteDbState *d, const char *key, const char **out_value)
{
  if (d == NULL || key == NULL || out_value == NULL) {
    return -1;
  }
  for (uint32_t i = 0U; i < SONIC_LITE_DB_CAP; ++i) {
    if (!d->kv[i].used) {
      continue;
    }
    if (strcmp(d->kv[i].key, key) == 0) {
      *out_value = d->kv[i].value;
      return 0;
    }
  }
  return -4;
}

static int sonic_lite_list_db(const SonicLiteDbState *d,
                              uint16_t dense_index,
                              const char **out_key,
                              const char **out_value)
{
  uint16_t seen = 0U;

  if (d == NULL || out_key == NULL || out_value == NULL) {
    return -1;
  }

  for (uint32_t i = 0U; i < SONIC_LITE_DB_CAP; ++i) {
    if (!d->kv[i].used) {
      continue;
    }
    if (seen == dense_index) {
      *out_key = d->kv[i].key;
      *out_value = d->kv[i].value;
      return 0;
    }
    seen++;
  }
  return -4;
}

static uint16_t sonic_lite_diff_count_db(const SonicLiteDbState *running, const SonicLiteDbState *candidate)
{
  uint16_t diff = 0U;
  if (running == NULL || candidate == NULL) {
    return 0U;
  }

  for (uint32_t i = 0U; i < SONIC_LITE_DB_CAP; ++i) {
    if (!running->kv[i].used) {
      continue;
    }
    int32_t cidx = sonic_lite_find_slot(candidate, running->kv[i].key);
    if (cidx < 0) {
      diff++;
      continue;
    }
    if (strcmp(candidate->kv[cidx].value, running->kv[i].value) != 0) {
      diff++;
    }
  }

  for (uint32_t i = 0U; i < SONIC_LITE_DB_CAP; ++i) {
    if (!candidate->kv[i].used) {
      continue;
    }
    if (sonic_lite_find_slot(running, candidate->kv[i].key) < 0) {
      diff++;
    }
  }
  return diff;
}

static uint8_t sonic_lite_any_diff(const SonicLiteState *state)
{
  if (state == NULL) {
    return 0U;
  }
  for (uint32_t db = 0U; db < SONIC_DB_COUNT; ++db) {
    if (sonic_lite_diff_count_db(&state->running.db[db], &state->candidate.db[db]) != 0U) {
      return 1U;
    }
  }
  return 0U;
}

const char *sonic_lite_db_name(SonicLiteDb db)
{
  switch (db) {
    case SONIC_DB_CONFIG:
      return "config";
    case SONIC_DB_APPL:
      return "appl";
    case SONIC_DB_ASIC:
      return "asic";
    default:
      return "unknown";
  }
}

int sonic_lite_db_from_name(const char *name, SonicLiteDb *out_db)
{
  if (name == NULL || out_db == NULL) {
    return 0;
  }
  if (strcmp(name, "config") == 0) {
    *out_db = SONIC_DB_CONFIG;
    return 1;
  }
  if (strcmp(name, "appl") == 0) {
    *out_db = SONIC_DB_APPL;
    return 1;
  }
  if (strcmp(name, "asic") == 0) {
    *out_db = SONIC_DB_ASIC;
    return 1;
  }
  return 0;
}

int sonic_lite_is_view_name(const char *name)
{
  if (name == NULL) {
    return 0;
  }
  return (strcmp(name, "running") == 0 || strcmp(name, "candidate") == 0) ? 1 : 0;
}

uint16_t sonic_lite_count(const SonicLiteState *state, SonicLiteDb db)
{
  const SonicLiteDbState *d = sonic_lite_db_ro_running(state, db);
  if (d == NULL) {
    return 0U;
  }
  return d->count;
}

uint16_t sonic_lite_count_candidate(const SonicLiteState *state, SonicLiteDb db)
{
  const SonicLiteDbState *d = sonic_lite_db_ro_candidate(state, db);
  if (d == NULL) {
    return 0U;
  }
  return d->count;
}

int sonic_lite_get(const SonicLiteState *state, SonicLiteDb db, const char *key, const char **out_value)
{
  return sonic_lite_get_db(sonic_lite_db_ro_running(state, db), key, out_value);
}

int sonic_lite_get_candidate(const SonicLiteState *state, SonicLiteDb db, const char *key, const char **out_value)
{
  return sonic_lite_get_db(sonic_lite_db_ro_candidate(state, db), key, out_value);
}

int sonic_lite_list(const SonicLiteState *state,
                    SonicLiteDb db,
                    uint16_t dense_index,
                    const char **out_key,
                    const char **out_value)
{
  return sonic_lite_list_db(sonic_lite_db_ro_running(state, db), dense_index, out_key, out_value);
}

int sonic_lite_list_candidate(const SonicLiteState *state,
                              SonicLiteDb db,
                              uint16_t dense_index,
                              const char **out_key,
                              const char **out_value)
{
  return sonic_lite_list_db(sonic_lite_db_ro_candidate(state, db), dense_index, out_key, out_value);
}

uint16_t sonic_lite_diff_count(const SonicLiteState *state, SonicLiteDb db)
{
  if (state == NULL || db >= SONIC_DB_COUNT) {
    return 0U;
  }
  return sonic_lite_diff_count_db(&state->running.db[db], &state->candidate.db[db]);
}

uint8_t sonic_lite_candidate_dirty(const SonicLiteState *state)
{
  if (state == NULL) {
    return 0U;
  }
  return state->candidate_dirty;
}

int sonic_lite_set(SonicLiteState *state, SonicLiteDb db, const char *key, const char *value)
{
  SonicLiteDbState *d;
  int rc;

  if (state == NULL) {
    return -1;
  }
  d = sonic_lite_db_mut_from_plane(&state->candidate, db);
  rc = sonic_lite_set_db(d, key, value);
  if (rc != 0) {
    return rc;
  }
  state->candidate_dirty = sonic_lite_any_diff(state);
  return 0;
}

int sonic_lite_commit(SonicLiteState *state)
{
  if (state == NULL) {
    return -1;
  }
  state->running = state->candidate;
  state->candidate_dirty = 0U;
  return 0;
}

void sonic_lite_abort(SonicLiteState *state)
{
  if (state == NULL) {
    return;
  }
  state->candidate = state->running;
  state->candidate_dirty = 0U;
}

void sonic_lite_init(SonicLiteState *state)
{
  if (state == NULL) {
    return;
  }
  memset(state, 0, sizeof(*state));

  /* Default bootstrap identity mirrors NOS style metadata. */
  (void)sonic_lite_set_db(&state->running.db[SONIC_DB_CONFIG], "hostname", "mkmpu");
  (void)sonic_lite_set_db(&state->running.db[SONIC_DB_CONFIG], "platform", "stm32f446");
  (void)sonic_lite_set_db(&state->running.db[SONIC_DB_CONFIG], "role", "leaf-lab");

  state->candidate = state->running;
  state->candidate_dirty = 0U;
}
