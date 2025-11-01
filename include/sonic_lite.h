#ifndef SONIC_LITE_H
#define SONIC_LITE_H

#include <stdint.h>

#define SONIC_LITE_KEY_MAX 24U
#define SONIC_LITE_VALUE_MAX 32U
#define SONIC_LITE_DB_CAP 16U

typedef enum {
  SONIC_DB_CONFIG = 0,
  SONIC_DB_APPL = 1,
  SONIC_DB_ASIC = 2,
  SONIC_DB_COUNT = 3
} SonicLiteDb;

typedef struct {
  char key[SONIC_LITE_KEY_MAX];
  char value[SONIC_LITE_VALUE_MAX];
  uint8_t used;
} SonicLiteKv;

typedef struct {
  SonicLiteKv kv[SONIC_LITE_DB_CAP];
  uint16_t count;
} SonicLiteDbState;

typedef struct {
  SonicLiteDbState db[SONIC_DB_COUNT];
} SonicLitePlane;

typedef struct {
  SonicLitePlane running;
  SonicLitePlane candidate;
  uint8_t candidate_dirty;
} SonicLiteState;

void sonic_lite_init(SonicLiteState *state);
const char *sonic_lite_db_name(SonicLiteDb db);
int sonic_lite_db_from_name(const char *name, SonicLiteDb *out_db);
int sonic_lite_is_view_name(const char *name);

/* running-plane helpers */
uint16_t sonic_lite_count(const SonicLiteState *state, SonicLiteDb db);
int sonic_lite_get(const SonicLiteState *state, SonicLiteDb db, const char *key, const char **out_value);
int sonic_lite_list(const SonicLiteState *state,
                    SonicLiteDb db,
                    uint16_t dense_index,
                    const char **out_key,
                    const char **out_value);

/* candidate-plane helpers */
uint16_t sonic_lite_count_candidate(const SonicLiteState *state, SonicLiteDb db);
int sonic_lite_get_candidate(const SonicLiteState *state, SonicLiteDb db, const char *key, const char **out_value);
int sonic_lite_list_candidate(const SonicLiteState *state,
                              SonicLiteDb db,
                              uint16_t dense_index,
                              const char **out_key,
                              const char **out_value);

/* transaction helpers */
int sonic_lite_set(SonicLiteState *state, SonicLiteDb db, const char *key, const char *value);
int sonic_lite_commit(SonicLiteState *state);
void sonic_lite_abort(SonicLiteState *state);
uint8_t sonic_lite_candidate_dirty(const SonicLiteState *state);
uint16_t sonic_lite_diff_count(const SonicLiteState *state, SonicLiteDb db);

#endif
