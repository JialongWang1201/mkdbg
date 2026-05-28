#include "mkdbg.h"

#define REPLAY_TEXT_MAX 65536U
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

static int read_file_text(const char *path, char *buf, size_t buf_size)
{
  FILE *f;
  size_t n;

  if (buf_size == 0U) return -1;
  f = fopen(path, "rb");
  if (f == NULL) return -1;
  n = fread(buf, 1U, buf_size - 1U, f);
  if (ferror(f)) {
    fclose(f);
    return -1;
  }
  buf[n] = '\0';
  fclose(f);
  return 0;
}

static const char *json_key(const char *buf, const char *key)
{
  char needle[128];
  snprintf(needle, sizeof(needle), "\"%s\":", key);
  return strstr(buf, needle);
}

static const char *json_array_key(const char *buf, const char *key)
{
  const char *p = buf;

  while ((p = json_key(p, key)) != NULL) {
    const char *q = strchr(p, ':');
    if (q == NULL) return NULL;
    q++;
    while (*q != '\0' && isspace((unsigned char)*q)) q++;
    if (*q == '[') return q;
    p = q;
  }
  return NULL;
}

static const char *json_next_array_key(const char *buf, const char *key,
                                       const char *after)
{
  if (after == NULL) return json_array_key(buf, key);
  return json_array_key(after, key);
}

static const char *matching_delim(const char *open, char left, char right)
{
  const char *p = open;
  int depth = 0;
  int in_string = 0;
  int escape = 0;

  if (open == NULL || *open != left) return NULL;
  for (; *p != '\0'; p++) {
    if (in_string) {
      if (escape) {
        escape = 0;
      } else if (*p == '\\') {
        escape = 1;
      } else if (*p == '"') {
        in_string = 0;
      }
      continue;
    }
    if (*p == '"') {
      in_string = 1;
    } else if (*p == left) {
      depth++;
    } else if (*p == right) {
      depth--;
      if (depth == 0) return p;
    }
  }
  return NULL;
}

static void json_string_value(const char *buf, const char *key,
                              char *out, size_t out_size)
{
  const char *p = json_key(buf, key);
  size_t i = 0U;

  if (out_size == 0U) return;
  out[0] = '\0';
  if (p == NULL) return;
  p = strchr(p, ':');
  if (p == NULL) return;
  p++;
  while (*p != '\0' && isspace((unsigned char)*p)) p++;
  if (*p != '"') return;
  p++;
  while (*p != '\0' && *p != '"' && i + 1U < out_size) {
    if (*p == '\\' && p[1] != '\0') p++;
    out[i++] = *p++;
  }
  out[i] = '\0';
}

static int json_int_value(const char *buf, const char *key)
{
  const char *p = json_key(buf, key);
  if (p == NULL) return 0;
  p = strchr(p, ':');
  if (p == NULL) return 0;
  p++;
  while (*p != '\0' && isspace((unsigned char)*p)) p++;
  return atoi(p);
}

static void print_json_string(const char *s)
{
  putchar('"');
  for (; *s != '\0'; s++) {
    unsigned char c = (unsigned char)*s;
    if (*s == '"' || *s == '\\') {
      putchar('\\');
      putchar(*s);
    } else if (c < 0x20U) {
      printf("\\u%04x", c);
    } else {
      putchar(*s);
    }
  }
  putchar('"');
}

static int timeline_event_is_fault(const TimelineEvent *ev)
{
  return strstr(ev->flags, "fault") != NULL ||
         strstr(ev->msg, "Fault") != NULL ||
         strstr(ev->msg, "HardFault") != NULL;
}

static void timeline_add_object(Timeline *tl, const char *start, const char *end)
{
  char obj[2048];
  size_t len;
  TimelineEvent *ev;

  if (start == NULL || end == NULL || end <= start) return;
  if (tl->count >= TIMELINE_MAX_EVENTS) {
    tl->truncated = 1;
    return;
  }
  len = (size_t)(end - start + 1);
  if (len >= sizeof(obj)) len = sizeof(obj) - 1U;
  memcpy(obj, start, len);
  obj[len] = '\0';

  ev = &tl->events[tl->count];
  memset(ev, 0, sizeof(*ev));
  json_string_value(obj, "msg", ev->msg, sizeof(ev->msg));
  if (ev->msg[0] == '\0') return;

  ev->event_id = json_int_value(obj, "event_id");
  ev->age_ms = json_int_value(obj, "age_ms");
  ev->ts_ms = json_int_value(obj, "ts_ms");
  json_string_value(obj, "corr_id", ev->corr_id, sizeof(ev->corr_id));
  json_string_value(obj, "flags", ev->flags, sizeof(ev->flags));
  json_string_value(obj, "stage", ev->stage, sizeof(ev->stage));
  tl->count++;
}

static void timeline_parse_array(Timeline *tl, const char *array_start)
{
  const char *array_end = matching_delim(array_start, '[', ']');
  const char *p = array_start;

  if (array_end == NULL) return;
  while ((p = strchr(p, '{')) != NULL && p < array_end) {
    const char *obj_end = matching_delim(p, '{', '}');
    if (obj_end == NULL || obj_end > array_end) return;
    timeline_add_object(tl, p, obj_end);
    p = obj_end + 1;
  }
}

static int load_timeline(const char *path, Timeline *timeline)
{
  char buf[REPLAY_TEXT_MAX];
  const char *array;
  const char *cursor = NULL;

  memset(timeline, 0, sizeof(*timeline));
  if (read_file_text(path, buf, sizeof(buf)) != 0) {
    return -1;
  }

  while ((array = json_next_array_key(buf, "events", cursor)) != NULL) {
    timeline_parse_array(timeline, array);
    cursor = array + 1;
  }
  return 0;
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

static void print_timeline_event_json(const char *path,
                                      const TimelineEvent *ev)
{
  printf("{\"bundle\":");
  print_json_string(path);
  printf(",\"event\":{\"event_id\":%d,\"age_ms\":%d,\"ts_ms\":%d,"
         "\"fault_anchor\":%s,",
         ev->event_id, ev->age_ms, ev->ts_ms,
         timeline_event_is_fault(ev) ? "true" : "false");
  printf("\"stage\":");
  print_json_string(ev->stage);
  printf(",\"flags\":");
  print_json_string(ev->flags);
  printf(",\"corr_id\":");
  print_json_string(ev->corr_id);
  printf(",\"msg\":");
  print_json_string(ev->msg);
  printf("}}\n");
}

static void print_timeline_json(const char *path, const Timeline *timeline)
{
  size_t i;

  printf("{\"bundle\":");
  print_json_string(path);
  printf(",\"event_count\":%zu,\"truncated\":%s,\"events\":[",
         timeline->count, timeline->truncated ? "true" : "false");
  for (i = 0; i < timeline->count; i++) {
    const TimelineEvent *ev = &timeline->events[i];
    if (i != 0U) printf(",");
    printf("{\"event_id\":%d,\"age_ms\":%d,\"ts_ms\":%d,"
           "\"fault_anchor\":%s,",
           ev->event_id, ev->age_ms, ev->ts_ms,
           timeline_event_is_fault(ev) ? "true" : "false");
    printf("\"stage\":");
    print_json_string(ev->stage);
    printf(",\"flags\":");
    print_json_string(ev->flags);
    printf(",\"corr_id\":");
    print_json_string(ev->corr_id);
    printf(",\"msg\":");
    print_json_string(ev->msg);
    printf("}");
  }
  printf("]}\n");
}

int load_bundle_summary(const char *path, BundleSummary *summary)
{
  char buf[REPLAY_TEXT_MAX];

  memset(summary, 0, sizeof(*summary));
  copy_string(summary->path, sizeof(summary->path), path);
  if (read_file_text(path, buf, sizeof(buf)) != 0) {
    return -1;
  }

  summary->halt_signal = json_int_value(buf, "halt_signal");
  summary->timeout = json_int_value(buf, "timeout");
  json_string_value(buf, "pc", summary->pc, sizeof(summary->pc));
  json_string_value(buf, "lr", summary->lr, sizeof(summary->lr));
  json_string_value(buf, "sp", summary->sp, sizeof(summary->sp));
  json_string_value(buf, "cfsr", summary->cfsr, sizeof(summary->cfsr));
  json_string_value(buf, "cfsr_decoded", summary->cfsr_decoded,
                    sizeof(summary->cfsr_decoded));
  return 0;
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
        print_timeline_event_json(opts->bundle, ev);
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
        print_timeline_event_json(opts->bundle, ev);
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
      print_timeline_json(opts->bundle, &timeline);
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
    printf("{\"bundle\":\"%s\",\"halt_signal\":%d,\"timeout\":%d,"
           "\"pc\":\"%s\",\"lr\":\"%s\",\"sp\":\"%s\",\"cfsr\":\"%s\"}\n",
           s.path, s.halt_signal, s.timeout, s.pc, s.lr, s.sp, s.cfsr);
    return 0;
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
    printf("{\"left\":\"%s\",\"right\":\"%s\",", a.path, b.path);
    printf("\"halt_signal_changed\":%s,",
           a.halt_signal != b.halt_signal ? "true" : "false");
    printf("\"timeout_changed\":%s,",
           a.timeout != b.timeout ? "true" : "false");
    printf("\"pc_changed\":%s,",
           strcmp(a.pc, b.pc) != 0 ? "true" : "false");
    printf("\"cfsr_changed\":%s}\n",
           strcmp(a.cfsr, b.cfsr) != 0 ? "true" : "false");
    return 0;
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
