#include "mkdbg.h"

static int failures;

static void expect(int condition, const char *message)
{
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", message);
    failures++;
  }
}

static void init_meta(IncidentMetadata *meta, const char *id)
{
  memset(meta, 0, sizeof(*meta));
  copy_string(meta->id, sizeof(meta->id), id);
  copy_string(meta->name, sizeof(meta->name), "fault \"alpha\"");
  copy_string(meta->status, sizeof(meta->status), "open");
  copy_string(meta->repo, sizeof(meta->repo), "demo");
  copy_string(meta->port, sizeof(meta->port), "/dev/test\\port");
  meta->opened_at = 100;
}

int main(void)
{
  char dir[] = "/tmp/mkdbg-incident-XXXXXX";
  char current[PATH_MAX];
  char closing[PATH_MAX];
  char metadata[PATH_MAX];
  IncidentMetadata meta;
  IncidentMetadata loaded;

  expect(mkdtemp(dir) != NULL, "create incident test directory");
  snprintf(current, sizeof(current), "%s/current", dir);
  snprintf(closing, sizeof(closing), "%s/current.closing", dir);
  snprintf(metadata, sizeof(metadata), "%s/incident.json", dir);

  init_meta(&meta, "100-fault");
  expect(write_incident_metadata(metadata, &meta, 0) == 0,
         "atomically write metadata");
  expect(load_incident_metadata(metadata, &loaded) == 0,
         "read atomically written metadata");
  expect(strcmp(loaded.name, meta.name) == 0 && strcmp(loaded.port, meta.port) == 0,
         "round-trip escaped metadata strings");
  expect(write_current_incident_id(current, meta.id) == 0,
         "atomically write current marker");

  expect(mkdir(closing, 0700) == 0, "create rename failure fixture");
  expect(close_incident_state(current, metadata, &meta, 200) != 0,
         "preserve active marker when close transition fails");
  expect(access(current, F_OK) == 0, "current marker remains retryable");
  expect(rmdir(closing) == 0, "remove rename failure fixture");
  expect(close_incident_state(current, metadata, &meta, 200) == 0,
         "retry close after marker transition failure");
  expect(access(current, F_OK) != 0 && access(closing, F_OK) != 0,
         "remove markers after successful close");
  expect(load_incident_metadata(metadata, &loaded) == 0 &&
         strcmp(loaded.status, "closed") == 0 && loaded.closed_at == 200,
         "persist closed metadata atomically");

  init_meta(&meta, "101-fault");
  expect(write_current_incident_id(current, meta.id) == 0,
         "write marker for metadata failure fixture");
  expect(unlink(metadata) == 0 && mkdir(metadata, 0700) == 0,
         "create metadata publish failure fixture");
  expect(close_incident_state(current, metadata, &meta, 300) != 0,
         "retain closing marker when metadata publish fails");
  expect(access(current, F_OK) != 0 && access(closing, F_OK) == 0,
         "record retryable closing state");
  expect(rmdir(metadata) == 0, "remove metadata failure fixture");
  expect(close_incident_state(current, metadata, &meta, 999) == 0,
         "retry close from closing marker");
  expect(load_incident_metadata(metadata, &loaded) == 0 && loaded.closed_at == 300,
         "preserve original close timestamp across retry");

  expect(unlink(metadata) == 0, "remove metadata fixture");
  expect(rmdir(dir) == 0, "remove incident test directory");
  return failures == 0 ? 0 : 1;
}
