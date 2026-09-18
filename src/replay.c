#include "mkdbg.h"
#include "json.h"

#define TIMELINE_MAX_EVENTS 128U

typedef struct {
  int event_id;
  int age_ms;
  int ts_ms;
  char corr_id[32];
  char flags[32];
  char stage[96];
  char msg[256];
} TimelineEvent;

typedef struct {
  TimelineEvent events[TIMELINE_MAX_EVENTS];
  size_t count;
  int truncated;
} Timeline;

static int json_skip_value(JsonReader *reader, JsonTokenType first)
{
  JsonToken token;
  int depth = 0;
  if (first == JSON_TOKEN_OBJECT_BEGIN || first == JSON_TOKEN_ARRAY_BEGIN) depth = 1;
  while (depth > 0) {
    JsonTokenType type = json_reader_next(reader, &token);
    if (type == JSON_TOKEN_ERROR || type == JSON_TOKEN_EOF) return -1;
    if (type == JSON_TOKEN_OBJECT_BEGIN || type == JSON_TOKEN_ARRAY_BEGIN) depth++;
    else if (type == JSON_TOKEN_OBJECT_END || type == JSON_TOKEN_ARRAY_END) depth--;
  }
  return 0;
}

static int json_read_value(JsonReader *reader, JsonToken *token)
{
  if (json_reader_next(reader, token) != JSON_TOKEN_COLON) return -1;
  return json_reader_next(reader, token) == JSON_TOKEN_ERROR ? -1 : 0;
}

static int token_to_int(const JsonToken *token, int *out)
{
  long value;
  if (json_token_to_long(token, &value) != 0 || value < INT_MIN || value > INT_MAX)
    return -1;
  *out = (int)value;
  return 0;
}

static int timeline_event_is_fault(const TimelineEvent *ev)
{
  return strstr(ev->flags, "fault") != NULL ||
         strstr(ev->msg, "Fault") != NULL ||
         strstr(ev->msg, "HardFault") != NULL;
}

static int timeline_read_object(JsonReader *reader, Timeline *timeline)
{
  JsonToken key;
  JsonToken value;
  TimelineEvent event;
  TimelineEvent *ev = &event;
  memset(ev, 0, sizeof(*ev));
  for (;;) {
    JsonTokenType type = json_reader_next(reader, &key);
    if (type == JSON_TOKEN_OBJECT_END) break;
    if (type == JSON_TOKEN_COMMA) continue;
    if (type != JSON_TOKEN_STRING || key.truncated ||
        json_read_value(reader, &value) != 0) return -1;
    if (strcmp(key.text, "event_id") == 0) {
      if (token_to_int(&value, &ev->event_id) != 0) return -1;
    } else if (strcmp(key.text, "age_ms") == 0) {
      if (token_to_int(&value, &ev->age_ms) != 0) return -1;
    } else if (strcmp(key.text, "ts_ms") == 0) {
      if (token_to_int(&value, &ev->ts_ms) != 0) return -1;
    } else if (value.type == JSON_TOKEN_STRING) {
      if (strcmp(key.text, "msg") == 0) copy_string(ev->msg, sizeof(ev->msg), value.text);
      else if (strcmp(key.text, "corr_id") == 0) copy_string(ev->corr_id, sizeof(ev->corr_id), value.text);
      else if (strcmp(key.text, "flags") == 0) copy_string(ev->flags, sizeof(ev->flags), value.text);
      else if (strcmp(key.text, "stage") == 0) copy_string(ev->stage, sizeof(ev->stage), value.text);
      if (value.truncated) timeline->truncated = 1;
    } else if (json_skip_value(reader, value.type) != 0) {
      return -1;
    }
  }
  if (ev->msg[0] == '\0') return 0;
  if (timeline->count >= TIMELINE_MAX_EVENTS) {
    timeline->truncated = 1;
    return 0;
  }
  timeline->events[timeline->count++] = event;
  return 0;
}

static int timeline_read_array(JsonReader *reader, Timeline *timeline)
{
  JsonToken token;
  for (;;) {
    JsonTokenType type = json_reader_next(reader, &token);
    if (type == JSON_TOKEN_ARRAY_END) return 0;
    if (type == JSON_TOKEN_COMMA) continue;
    if (type == JSON_TOKEN_OBJECT_BEGIN) {
      if (timeline_read_object(reader, timeline) != 0) return -1;
    } else if (type == JSON_TOKEN_ERROR || type == JSON_TOKEN_EOF ||
               json_skip_value(reader, type) != 0) {
      return -1;
    }
  }
}

static int timeline_scan_value(JsonReader *reader, JsonTokenType first,
                               Timeline *timeline)
{
  JsonToken token;
  if (first == JSON_TOKEN_OBJECT_BEGIN) {
    for (;;) {
      JsonToken key;
      JsonToken value;
      JsonTokenType type = json_reader_next(reader, &key);
      if (type == JSON_TOKEN_OBJECT_END) return 0;
      if (type == JSON_TOKEN_COMMA) continue;
      if (type != JSON_TOKEN_STRING || key.truncated ||
          json_read_value(reader, &value) != 0) return -1;
      if (strcmp(key.text, "events") == 0 && value.type == JSON_TOKEN_ARRAY_BEGIN) {
        if (timeline_read_array(reader, timeline) != 0) return -1;
      } else if (timeline_scan_value(reader, value.type, timeline) != 0) {
        return -1;
      }
    }
  }
  if (first == JSON_TOKEN_ARRAY_BEGIN) {
    for (;;) {
      JsonTokenType type = json_reader_next(reader, &token);
      if (type == JSON_TOKEN_ARRAY_END) return 0;
      if (type == JSON_TOKEN_COMMA) continue;
      if (type == JSON_TOKEN_ERROR || type == JSON_TOKEN_EOF ||
          timeline_scan_value(reader, type, timeline) != 0) return -1;
    }
  }
  return first == JSON_TOKEN_ERROR || first == JSON_TOKEN_EOF ? -1 : 0;
}

static int load_timeline(const char *path, Timeline *timeline)
{
  FILE *file;
  JsonReader reader;
  JsonToken token;
  int result = 0;

  memset(timeline, 0, sizeof(*timeline));
  file = fopen(path, "rb");
  if (!file) return -1;
  json_reader_init(&reader, file);
  if (timeline_scan_value(&reader, json_reader_next(&reader, &token), timeline) != 0 ||
      json_reader_next(&reader, &token) != JSON_TOKEN_EOF) result = -1;
  if (fclose(file) != 0) result = -1;
  return result;
}

static int timeline_find_event_index(const Timeline *timeline, int event_id)
{
  size_t i;

  for (i = 0; i < timeline->count; i++) {
    if (timeline->events[i].event_id == event_id) {
      return (int)i;
    }
  }
  return -1;
}

static int timeline_find_fault_index(const Timeline *timeline)
{
  size_t i;

  for (i = 0; i < timeline->count; i++) {
    if (timeline_event_is_fault(&timeline->events[i])) {
      return (int)i;
    }
  }
  return -1;
}

static void print_timeline_event_text(const char *path,
                                      const TimelineEvent *ev)
{
  printf("bundle: %s\n", path);
  printf("event: %d\n", ev->event_id);
  printf("fault_anchor: %s\n", timeline_event_is_fault(ev) ? "yes" : "no");
  if (ev->age_ms != 0) printf("time: -%dms\n", ev->age_ms);
  else if (ev->ts_ms != 0) printf("time: %dms\n", ev->ts_ms);
  else printf("time: unknown\n");
  printf("stage: %s\n", ev->stage[0] ? ev->stage : "unknown");
  printf("flags: %s\n", ev->flags[0] ? ev->flags : "unknown");
  printf("corr: %s\n", ev->corr_id[0] ? ev->corr_id : "unknown");
  printf("msg: %s\n", ev->msg);
}

static void print_timeline_event_line(const TimelineEvent *ev, char marker)
{
  printf("%c %03d", marker, ev->event_id);
  if (ev->age_ms != 0) printf("  -%dms", ev->age_ms);
  else if (ev->ts_ms != 0) printf("  t=%dms", ev->ts_ms);
  else printf("  t=?");
  printf("  %s", ev->msg);
  if (ev->stage[0] != '\0') printf("  stage=%s", ev->stage);
  if (ev->flags[0] != '\0') printf("  flags=%s", ev->flags);
  if (ev->corr_id[0] != '\0') printf("  corr=%s", ev->corr_id);
  printf("\n");
}

static void print_timeline_context_text(const Timeline *timeline,
                                        int selected_index,
                                        int radius)
{
  size_t i;
  size_t start;
  size_t end;
  size_t selected = (size_t)selected_index;
  size_t r = (size_t)radius;

  if (selected_index < 0 || timeline->count == 0U) return;
  start = selected > r ? selected - r : 0U;
  end = selected + r;
  if (end >= timeline->count) end = timeline->count - 1U;

  printf("context_events: %zu\n", end - start + 1U);
  for (i = start; i <= end; i++) {
    const TimelineEvent *ev = &timeline->events[i];
    char marker = i == selected ? '>' : (timeline_event_is_fault(ev) ? '!' : ' ');
    print_timeline_event_line(ev, marker);
  }
}

static void print_timeline_text(const char *path, const Timeline *timeline)
{
  size_t i;

  printf("bundle: %s\n", path);
  printf("timeline_events: %zu", timeline->count);
  if (timeline->truncated) printf(" (truncated)");
  printf("\n");
  if (timeline->count == 0U) {
    printf("timeline: no events found\n");
    return;
  }
  for (i = 0; i < timeline->count; i++) {
    const TimelineEvent *ev = &timeline->events[i];
    char marker = timeline_event_is_fault(ev) ? '!' : ' ';
    print_timeline_event_line(ev, marker);
  }
}

static int writer_key_string(JsonWriter *writer, const char *key, const char *value)
{
  return json_writer_key(writer, key) == 0 && json_writer_string(writer, value) == 0 ? 0 : -1;
}

static int writer_key_long(JsonWriter *writer, const char *key, long value)
{
  return json_writer_key(writer, key) == 0 && json_writer_long(writer, value) == 0 ? 0 : -1;
}

static int writer_key_bool(JsonWriter *writer, const char *key, int value)
{
  return json_writer_key(writer, key) == 0 && json_writer_bool(writer, value) == 0 ? 0 : -1;
}

static int finish_json_line(JsonWriter *writer)
{
  if (json_writer_finish(writer) != 0 || fputc('\n', stdout) == EOF || fflush(stdout) != 0)
    return -1;
  return 0;
}

static int write_timeline_event_json_object(JsonWriter *writer,
                                            const TimelineEvent *ev)
{
  if (json_writer_begin_object(writer) != 0 ||
      writer_key_long(writer, "event_id", ev->event_id) != 0 ||
      writer_key_long(writer, "age_ms", ev->age_ms) != 0 ||
      writer_key_long(writer, "ts_ms", ev->ts_ms) != 0 ||
      writer_key_bool(writer, "fault_anchor", timeline_event_is_fault(ev)) != 0 ||
      writer_key_string(writer, "stage", ev->stage) != 0 ||
      writer_key_string(writer, "flags", ev->flags) != 0 ||
      writer_key_string(writer, "corr_id", ev->corr_id) != 0 ||
      writer_key_string(writer, "msg", ev->msg) != 0 ||
      json_writer_end_object(writer) != 0) return -1;
  return 0;
}

static int print_timeline_event_json(const char *path,
                                     const TimelineEvent *ev)
{
  JsonWriter writer;
  json_writer_init(&writer, stdout);
  if (json_writer_begin_object(&writer) != 0 ||
      writer_key_string(&writer, "bundle", path) != 0 ||
      json_writer_key(&writer, "event") != 0 ||
      write_timeline_event_json_object(&writer, ev) != 0 ||
      json_writer_end_object(&writer) != 0) return -1;
  return finish_json_line(&writer);
}

static int print_timeline_context_json(const char *path,
                                       const Timeline *timeline,
                                       int selected_index,
                                       int radius)
{
  size_t i;
  size_t start;
  size_t end;
  size_t selected = (size_t)selected_index;
  size_t r = (size_t)radius;

  JsonWriter writer;
  if (selected_index < 0 || timeline->count == 0U) return -1;
  start = selected > r ? selected - r : 0U;
  end = selected + r;
  if (end >= timeline->count) end = timeline->count - 1U;

  json_writer_init(&writer, stdout);
  if (json_writer_begin_object(&writer) != 0 ||
      writer_key_string(&writer, "bundle", path) != 0 ||
      writer_key_long(&writer, "selected_event_id", timeline->events[selected].event_id) != 0 ||
      writer_key_long(&writer, "context_radius", radius) != 0 ||
      writer_key_long(&writer, "event_count", (long)(end - start + 1U)) != 0 ||
      json_writer_key(&writer, "events") != 0 ||
      json_writer_begin_array(&writer) != 0) return -1;
  for (i = start; i <= end; i++) {
    if (write_timeline_event_json_object(&writer, &timeline->events[i]) != 0) return -1;
  }
  if (json_writer_end_array(&writer) != 0 || json_writer_end_object(&writer) != 0)
    return -1;
  return finish_json_line(&writer);
}

static int print_timeline_json(const char *path, const Timeline *timeline)
{
  size_t i;
  JsonWriter writer;
  json_writer_init(&writer, stdout);
  if (json_writer_begin_object(&writer) != 0 ||
      writer_key_string(&writer, "bundle", path) != 0 ||
      writer_key_long(&writer, "event_count", (long)timeline->count) != 0 ||
      writer_key_bool(&writer, "truncated", timeline->truncated) != 0 ||
      json_writer_key(&writer, "events") != 0 ||
      json_writer_begin_array(&writer) != 0) return -1;
  for (i = 0; i < timeline->count; i++) {
    if (write_timeline_event_json_object(&writer, &timeline->events[i]) != 0) return -1;
  }
  if (json_writer_end_array(&writer) != 0 || json_writer_end_object(&writer) != 0)
    return -1;
  return finish_json_line(&writer);
}

static int print_bundle_json(const BundleSummary *summary)
{
  JsonWriter writer;
  json_writer_init(&writer, stdout);
  if (json_writer_begin_object(&writer) != 0 ||
      writer_key_string(&writer, "bundle", summary->path) != 0 ||
      writer_key_long(&writer, "halt_signal", summary->halt_signal) != 0 ||
      writer_key_long(&writer, "timeout", summary->timeout) != 0 ||
      writer_key_string(&writer, "pc", summary->pc) != 0 ||
      writer_key_string(&writer, "lr", summary->lr) != 0 ||
      writer_key_string(&writer, "sp", summary->sp) != 0 ||
      writer_key_string(&writer, "cfsr", summary->cfsr) != 0 ||
      writer_key_string(&writer, "cfsr_decoded", summary->cfsr_decoded) != 0 ||
      json_writer_end_object(&writer) != 0) return -1;
  return finish_json_line(&writer);
}

static int print_diff_json(const BundleSummary *left, const BundleSummary *right)
{
  JsonWriter writer;
  json_writer_init(&writer, stdout);
  if (json_writer_begin_object(&writer) != 0 ||
      writer_key_string(&writer, "left", left->path) != 0 ||
      writer_key_string(&writer, "right", right->path) != 0 ||
      writer_key_bool(&writer, "halt_signal_changed",
                      left->halt_signal != right->halt_signal) != 0 ||
      writer_key_bool(&writer, "timeout_changed", left->timeout != right->timeout) != 0 ||
      writer_key_bool(&writer, "pc_changed", strcmp(left->pc, right->pc) != 0) != 0 ||
      writer_key_bool(&writer, "lr_changed", strcmp(left->lr, right->lr) != 0) != 0 ||
      writer_key_bool(&writer, "sp_changed", strcmp(left->sp, right->sp) != 0) != 0 ||
      writer_key_bool(&writer, "cfsr_changed", strcmp(left->cfsr, right->cfsr) != 0) != 0 ||
      writer_key_bool(&writer, "cfsr_decoded_changed",
                      strcmp(left->cfsr_decoded, right->cfsr_decoded) != 0) != 0 ||
      json_writer_end_object(&writer) != 0) return -1;
  return finish_json_line(&writer);
}

static int summary_set_string(char *out, size_t out_size, const JsonToken *value)
{
  if (value->type != JSON_TOKEN_STRING || value->truncated ||
      strlen(value->text) >= out_size) return -1;
  copy_string(out, out_size, value->text);
  return 0;
}

static int summary_scan_value(JsonReader *reader, JsonTokenType first,
                              BundleSummary *summary)
{
  JsonToken token;
  if (first == JSON_TOKEN_OBJECT_BEGIN) {
    for (;;) {
      JsonToken key;
      JsonToken value;
      JsonTokenType type = json_reader_next(reader, &key);
      if (type == JSON_TOKEN_OBJECT_END) return 0;
      if (type == JSON_TOKEN_COMMA) continue;
      if (type != JSON_TOKEN_STRING || key.truncated ||
          json_read_value(reader, &value) != 0) return -1;
      if (strcmp(key.text, "halt_signal") == 0) {
        if (token_to_int(&value, &summary->halt_signal) != 0) return -1;
      } else if (strcmp(key.text, "timeout") == 0) {
        if (token_to_int(&value, &summary->timeout) != 0) return -1;
      } else if (strcmp(key.text, "pc") == 0) {
        if (summary_set_string(summary->pc, sizeof(summary->pc), &value) != 0) return -1;
      } else if (strcmp(key.text, "lr") == 0) {
        if (summary_set_string(summary->lr, sizeof(summary->lr), &value) != 0) return -1;
      } else if (strcmp(key.text, "sp") == 0) {
        if (summary_set_string(summary->sp, sizeof(summary->sp), &value) != 0) return -1;
      } else if (strcmp(key.text, "cfsr") == 0) {
        if (summary_set_string(summary->cfsr, sizeof(summary->cfsr), &value) != 0) return -1;
      } else if (strcmp(key.text, "cfsr_decoded") == 0) {
        if (summary_set_string(summary->cfsr_decoded,
                               sizeof(summary->cfsr_decoded), &value) != 0) return -1;
      } else if (summary_scan_value(reader, value.type, summary) != 0) {
        return -1;
      }
    }
  }
  if (first == JSON_TOKEN_ARRAY_BEGIN) {
    for (;;) {
      JsonTokenType type = json_reader_next(reader, &token);
      if (type == JSON_TOKEN_ARRAY_END) return 0;
      if (type == JSON_TOKEN_COMMA) continue;
      if (type == JSON_TOKEN_ERROR || type == JSON_TOKEN_EOF ||
          summary_scan_value(reader, type, summary) != 0) return -1;
    }
  }
  return first == JSON_TOKEN_ERROR || first == JSON_TOKEN_EOF ? -1 : 0;
}

int load_bundle_summary(const char *path, BundleSummary *summary)
{
  FILE *file;
  JsonReader reader;
  JsonToken token;
  int result;

  memset(summary, 0, sizeof(*summary));
  copy_string(summary->path, sizeof(summary->path), path);
  file = fopen(path, "rb");
  if (!file) return -1;
  json_reader_init(&reader, file);
  result = summary_scan_value(&reader, json_reader_next(&reader, &token), summary);
  if (result == 0 && json_reader_next(&reader, &token) != JSON_TOKEN_EOF) result = -1;
  if (fclose(file) != 0) result = -1;
  return result;
}

int cmd_replay(const ReplayOptions *opts)
{
  BundleSummary s;
  Timeline timeline;

  if (opts->timeline) {
    if (load_timeline(opts->bundle, &timeline) != 0) {
      fprintf(stderr, "mkdbg: replay: cannot read %s\n", opts->bundle);
      return 1;
    }
    if (opts->event_id >= 0) {
      int selected_index = timeline_find_event_index(&timeline, opts->event_id);
      if (selected_index < 0) {
        fprintf(stderr, "mkdbg: replay: event %d not found\n", opts->event_id);
        return 1;
      }
      const TimelineEvent *ev = &timeline.events[selected_index];
      if (opts->json) {
        if (opts->context_radius >= 0) {
          if (print_timeline_context_json(opts->bundle, &timeline, selected_index,
                                          opts->context_radius) != 0) return 1;
        } else {
          if (print_timeline_event_json(opts->bundle, ev) != 0) return 1;
        }
      } else {
        print_timeline_event_text(opts->bundle, ev);
        if (opts->context_radius >= 0) {
          print_timeline_context_text(&timeline, selected_index,
                                      opts->context_radius);
        }
      }
      return 0;
    }
    if (opts->fault) {
      int selected_index = timeline_find_fault_index(&timeline);
      if (selected_index < 0) {
        fprintf(stderr, "mkdbg: replay: no fault event found\n");
        return 1;
      }
      const TimelineEvent *ev = &timeline.events[selected_index];
      if (opts->json) {
        if (opts->context_radius >= 0) {
          if (print_timeline_context_json(opts->bundle, &timeline, selected_index,
                                          opts->context_radius) != 0) return 1;
        } else {
          if (print_timeline_event_json(opts->bundle, ev) != 0) return 1;
        }
      } else {
        print_timeline_event_text(opts->bundle, ev);
        if (opts->context_radius >= 0) {
          print_timeline_context_text(&timeline, selected_index,
                                      opts->context_radius);
        }
      }
      return 0;
    }
    if (opts->json) {
      if (print_timeline_json(opts->bundle, &timeline) != 0) return 1;
    } else {
      print_timeline_text(opts->bundle, &timeline);
    }
    return 0;
  }

  if (load_bundle_summary(opts->bundle, &s) != 0) {
    fprintf(stderr, "mkdbg: replay: cannot read %s\n", opts->bundle);
    return 1;
  }

  if (opts->json) {
    return print_bundle_json(&s) == 0 ? 0 : 1;
  }

  printf("bundle: %s\n", s.path);
  printf("halt_signal: %d\n", s.halt_signal);
  printf("timeout: %d\n", s.timeout);
  printf("pc: %s\n", s.pc[0] ? s.pc : "unknown");
  printf("lr: %s\n", s.lr[0] ? s.lr : "unknown");
  printf("sp: %s\n", s.sp[0] ? s.sp : "unknown");
  printf("cfsr: %s\n", s.cfsr[0] ? s.cfsr : "unknown");
  if (s.cfsr_decoded[0] != '\0') {
    printf("cfsr_decoded: %s\n", s.cfsr_decoded);
  }
  return 0;
}

int cmd_diff(const DiffOptions *opts)
{
  BundleSummary a;
  BundleSummary b;
  int changed = 0;

  if (load_bundle_summary(opts->left, &a) != 0) {
    fprintf(stderr, "mkdbg: diff: cannot read %s\n", opts->left);
    return 1;
  }
  if (load_bundle_summary(opts->right, &b) != 0) {
    fprintf(stderr, "mkdbg: diff: cannot read %s\n", opts->right);
    return 1;
  }

  if (opts->json) {
    return print_diff_json(&a, &b) == 0 ? 0 : 1;
  }

  printf("left:  %s\n", a.path);
  printf("right: %s\n", b.path);
  if (a.halt_signal != b.halt_signal) {
    printf("halt_signal: %d -> %d\n", a.halt_signal, b.halt_signal);
    changed = 1;
  }
  if (a.timeout != b.timeout) {
    printf("timeout: %d -> %d\n", a.timeout, b.timeout);
    changed = 1;
  }
  if (strcmp(a.pc, b.pc) != 0) {
    printf("pc: %s -> %s\n", a.pc[0] ? a.pc : "unknown",
           b.pc[0] ? b.pc : "unknown");
    changed = 1;
  }
  if (strcmp(a.lr, b.lr) != 0) {
    printf("lr: %s -> %s\n", a.lr[0] ? a.lr : "unknown",
           b.lr[0] ? b.lr : "unknown");
    changed = 1;
  }
  if (strcmp(a.sp, b.sp) != 0) {
    printf("sp: %s -> %s\n", a.sp[0] ? a.sp : "unknown",
           b.sp[0] ? b.sp : "unknown");
    changed = 1;
  }
  if (strcmp(a.cfsr, b.cfsr) != 0) {
    printf("cfsr: %s -> %s\n", a.cfsr[0] ? a.cfsr : "unknown",
           b.cfsr[0] ? b.cfsr : "unknown");
    changed = 1;
  }
  if (strcmp(a.cfsr_decoded, b.cfsr_decoded) != 0) {
    printf("cfsr_decoded changed\n");
    changed = 1;
  }
  if (!changed) {
    printf("no summary differences\n");
  }
  return 0;
}
