#ifndef MKDBG_H
#define MKDBG_H

#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#else
#define _DEFAULT_SOURCE
#endif

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MKDBG_NATIVE_VERSION "0.1.0"
#define CONFIG_NAME ".mkdbg.toml"
#define STATE_DIR_NAME ".mkdbg"
#define INCIDENTS_DIR_NAME "incidents"
#define INCIDENT_CURRENT_NAME "current"
#define INCIDENT_META_NAME "incident.json"
#define MAX_REPOS 16
#define MAX_NAME 128
#define MAX_VALUE 1024
#define MAX_ATTACH_BREAKPOINTS 16
#define MAX_ATTACH_COMMANDS 16
#define MAX_U32_TEXT 16
#define SERIAL_BUF_SIZE 256
#define DEFAULT_BAUD 115200

/* ---- Data types ---- */

typedef struct {
  char name[MAX_NAME];
  char preset[MAX_NAME];
  char path[MAX_VALUE];
  char port[MAX_VALUE];
  char build_cmd[MAX_VALUE];
  char flash_cmd[MAX_VALUE];
  char hil_cmd[MAX_VALUE];
  char snapshot_cmd[MAX_VALUE];
  char attach_cmd[MAX_VALUE];
  char elf_path[MAX_VALUE];
  char snapshot_output[MAX_VALUE];
  char openocd_cfg[MAX_VALUE];
  char openocd_server_cmd[MAX_VALUE];
  char gdb[MAX_VALUE];
  char gdb_target[MAX_VALUE];
} RepoConfig;

typedef struct {
  int version;
  char default_repo[MAX_NAME];
  RepoConfig repos[MAX_REPOS];
  size_t repo_count;
} MkdbgConfig;

typedef struct {
  const char *preset;
  const char *name;
  const char *port;
  int force;
} InitOptions;

typedef struct {
  const char *repo;
  const char *target;
  const char *port;
} DoctorOptions;

typedef struct {
  const char *name;
  const char *path;
  const char *preset;
  const char *port;
  const char *build_cmd;
  const char *flash_cmd;
  const char *hil_cmd;
  const char *snapshot_cmd;
  const char *attach_cmd;
  const char *elf_path;
  const char *snapshot_output;
  const char *openocd_cfg;
  const char *openocd_server_cmd;
  const char *gdb;
  const char *gdb_target;
  int make_default;
} RepoAddOptions;

typedef struct {
  const char *name;
} NameCommandOptions;

typedef struct {
  const char *repo;
  const char *target;
  const char *name;
  const char *port;
} IncidentOpenOptions;

typedef struct {
  int json;
} IncidentStatusOptions;

typedef struct {
  const char *repo;
  const char *target;
  const char *port;
  const char *source_log;
  const char *output;
  int json;
  int dry_run;
} CaptureBundleOptions;

typedef struct {
  const char *repo;
  const char *target;
  const char *port;
  const char *bundle_json;
  const char *source_log;
  const char *auto_refresh_s;
  const char *width;
  const char *height;
  int render_once;
  int dry_run;
} WatchOptions;

typedef struct {
  const char *repo;
  const char *target;
  const char *port;
  const char *baud;
  const char *breakpoints[MAX_ATTACH_BREAKPOINTS];
  size_t breakpoint_count;
  const char *gdb_commands[MAX_ATTACH_COMMANDS];
  size_t gdb_command_count;
  double server_wait_s;
  int batch;
  int dry_run;
} AttachOptions;

typedef struct {
  const char *repo;
  const char *target;
  const char *port;
  const char *address;
  const char *value;
  int dry_run;
} ProbeOptions;

typedef struct {
  const char *repo;
  const char *target;
  const char *port;
  char **command;
  int command_argc;
  int dry_run;
} RunOptions;

typedef struct {
  const char *repo;
  const char *target;
  const char *port;
  int dry_run;
} ActionOptions;

typedef struct {
  const char *repo;
  const char *target;
  const char *branch_name;
  const char *path;
  int dry_run;
} GitOptions;

typedef struct {
  const char *repo;
  const char *target;
  const char *port;
  int baud;
  double char_delay_ms;
  const char *message;
  int dry_run;
} SerialOptions;

typedef struct {
  const char *repo;
  const char *target;
  const char *port;
  int baud;
  int dry_run;
} DashboardOptions;

typedef struct {
  const char *port;
  int         baud;
  const char *elf_path;
  const char *arch;  /* --arch name, e.g. "cortex-m"; NULL defaults to "cortex-m" */
} DebugOptions;

typedef struct {
  char id[MAX_NAME];
  char name[MAX_NAME];
  char status[MAX_NAME];
  char repo[MAX_NAME];
  char port[MAX_VALUE];
  long opened_at;
} IncidentMetadata;

/* ---- wire probe (mkdbg_wire.c) ---- */

#define WIRE_MAX_FRAMES   8
#define WIRE_REG_HEX_LEN 12   /* "0x" + 8 hex digits + NUL */
#define WIRE_NREGS       17

typedef struct {
  int  halt_signal;                          /* 0 = no halt / timeout  */
  int  timeout;                              /* 1 = no response        */
  char regs[WIRE_NREGS][WIRE_REG_HEX_LEN];  /* "0x%08x" per reg       */
  char cfsr[WIRE_REG_HEX_LEN];
  char cfsr_decoded[256];
  char stack_frames[WIRE_MAX_FRAMES][WIRE_REG_HEX_LEN];
  int  nframes;
  char timestamp[32];
} WireCrashReport;

/* Blocking: run wire-host --dump, parse JSON into *report.
 * Returns 0 on success (halt_signal > 0), 1 on timeout, -1 on error. */
int wire_probe_dump(const char *port, const char *baud,
                    WireCrashReport *report);

/* Non-blocking start: fork wire-host --dump, returns subprocess pid
 * and pipe fd for stdout.  Returns -1 on fork error. */
pid_t wire_probe_start(const char *port, const char *baud, int *pipe_fd_out);

/* Non-blocking poll: try to read from pipe_fd; if subprocess has exited
 * and all output was collected, parse JSON into *report and return 1.
 * Returns 0 if still running, -1 on error. */
int wire_probe_poll(pid_t pid, int pipe_fd, WireCrashReport *report);

/* ---- util.c ---- */
void die(const char *fmt, ...);
void trim_in_place(char *s);
void copy_string(char *dst, size_t dst_size, const char *src);
void append_string(char *dst, size_t dst_size, const char *src);
void replace_all(char *dst, size_t dst_size, const char *src,
                 const char *needle, const char *replacement);
void format_u32_hex(const char *input, const char *label, char *out, size_t out_size);
const char *path_basename(const char *path);
void path_dirname(const char *path, char *out, size_t out_size);
void join_path(const char *a, const char *b, char *out, size_t out_size);
void resolve_path(const char *base, const char *raw, char *out, size_t out_size);
int path_exists(const char *path);
int path_executable(const char *path);
int ensure_dir(const char *path);
void print_check(int ok, const char *label, const char *detail, int *failed);
int command_program(const char *command, char *out, size_t out_size);
int search_path(const char *program);
int command_available(const char *command);

/* ---- process.c ---- */
void print_shell_arg(FILE *f, const char *arg);
int run_process(char *const argv[], const char *cwd, int dry_run);
void print_command_label(const char *label, char *const argv[]);
void sleep_seconds(double seconds);
int wait_status_to_rc(int status);
pid_t spawn_process(char *const argv[], const char *cwd);
int wait_for_pid(pid_t pid);
int try_reap_pid(pid_t pid, int *rc);
void terminate_pid(pid_t pid);

/* ---- config.c ---- */
const RepoConfig *find_repo_const(const MkdbgConfig *config, const char *name);
RepoConfig *find_repo_mut(MkdbgConfig *config, const char *name);
void repo_set_defaults(RepoConfig *repo, const char *preset, const char *repo_path);
void write_config_value(FILE *f, const char *key, const char *value);
void render_repo(FILE *f, const RepoConfig *repo);
int save_config_file(const char *config_path, const MkdbgConfig *config);
int parse_quoted_value(const char *value, char *out, size_t out_size);
void repo_assign_key(RepoConfig *repo, const char *key, const char *value);
int load_config_file(const char *config_path, MkdbgConfig *config);
int find_config_upward(char *out, size_t out_size);
void resolve_repo_root(const char *config_path, const RepoConfig *repo, char *out, size_t out_size);
void resolve_repo_file(const char *config_path, const RepoConfig *repo, const char *raw, char *out, size_t out_size);
void state_root_from_config(const char *config_path, char *out, size_t out_size);
void incidents_root_from_config(const char *config_path, char *out, size_t out_size);
void current_incident_path_from_config(const char *config_path, char *out, size_t out_size);
void incident_meta_path(const char *incident_dir, char *out, size_t out_size);
int resolve_repo_name(const MkdbgConfig *config, const char *repo, const char *target, const char **out_name);

/* ---- incident.c ---- */
void sanitize_slug(const char *input, char *out, size_t out_size);
int load_current_incident_id(const char *config_path, char *out, size_t out_size);
int load_incident_metadata(const char *meta_path, IncidentMetadata *meta);
int write_incident_metadata(const char *meta_path, const IncidentMetadata *meta, long closed_at);
int load_current_incident_dir(const char *config_path, char *out, size_t out_size);
int cmd_incident_open(const IncidentOpenOptions *opts);
int cmd_incident_status(const IncidentStatusOptions *opts);
int cmd_incident_close(void);

/* ---- parse.c ---- */
int parse_init_args(int argc, char **argv, InitOptions *opts);
int parse_doctor_args(int argc, char **argv, DoctorOptions *opts);
int parse_repo_add_args(int argc, char **argv, RepoAddOptions *opts);
int parse_name_command_args(int argc, char **argv, NameCommandOptions *opts, const char *label);
int parse_incident_open_args(int argc, char **argv, IncidentOpenOptions *opts);
int parse_incident_status_args(int argc, char **argv, IncidentStatusOptions *opts);
int parse_capture_bundle_args(int argc, char **argv, CaptureBundleOptions *opts);
int parse_watch_args(int argc, char **argv, WatchOptions *opts);
int parse_attach_args(int argc, char **argv, AttachOptions *opts);
int parse_probe_args(int argc, char **argv, ProbeOptions *opts);
int parse_run_args(int argc, char **argv, RunOptions *opts);
int parse_action_args(int argc, char **argv, ActionOptions *opts);
int parse_git_args(int argc, char **argv, GitOptions *opts);
int parse_serial_args(int argc, char **argv, SerialOptions *opts);
int parse_debug_args(int argc, char **argv, DebugOptions *opts);

/* ---- git.c ---- */
void git_resolve_repo_root(const GitOptions *opts,
                           char *config_path,
                           size_t config_path_size,
                           char *repo_root,
                           size_t repo_root_size);
int cmd_git_status(const GitOptions *opts);
int cmd_git_rev(const GitOptions *opts);
int cmd_git_new_branch(const GitOptions *opts);
int cmd_git_worktree(const GitOptions *opts);
int cmd_git_push_current(const GitOptions *opts);

/* ---- probe.c ---- */
int cmd_probe_action(const ProbeOptions *opts, const char *command);
int cmd_probe_flash(const ProbeOptions *opts);
int cmd_probe_read32(const ProbeOptions *opts);
int cmd_probe_write32(const ProbeOptions *opts);

/* ---- action.c ---- */
void build_action_context(const char *config_path,
                          const char *repo_name,
                          const RepoConfig *repo,
                          const char *port,
                          char *repo_root,
                          size_t repo_root_size,
                          char *elf_path,
                          size_t elf_path_size,
                          char *openocd_cfg,
                          size_t openocd_cfg_size,
                          char *snapshot_output,
                          size_t snapshot_output_size,
                          char *gdb_target,
                          size_t gdb_target_size);
void format_action_command(const char *template,
                           const char *repo_name,
                           const char *repo_root,
                           const char *port,
                           const char *elf_path,
                           const char *openocd_cfg,
                           const char *snapshot_output,
                           const char *gdb_target,
                           char *out,
                           size_t out_size);
int cmd_run(const RunOptions *opts);
int cmd_configured_action(const ActionOptions *opts, const char *field, int needs_port);

/* ---- dashboard.c ---- */
int parse_dashboard_args(int argc, char **argv, DashboardOptions *opts);
int cmd_dashboard(const DashboardOptions *opts);

/* ---- serial.c ---- */
speed_t baud_to_speed(int baud);
const char *resolve_serial_port(const char *config_path,
                                const MkdbgConfig *config,
                                const SerialOptions *opts);
int cmd_serial_tail(const SerialOptions *opts);
int cmd_serial_send(const SerialOptions *opts);

/* ---- core.c ---- */
void init_default_repo_name(char *out, size_t out_size);
int cmd_init(const InitOptions *opts);
int cmd_doctor(const DoctorOptions *opts);
void apply_repo_add_overrides(RepoConfig *repo, const RepoAddOptions *opts);
int cmd_repo_add(const RepoAddOptions *opts);
int cmd_repo_list(void);
int cmd_repo_use(const NameCommandOptions *opts);

/* ---- launcher.c ---- */
int cmd_capture_bundle(const CaptureBundleOptions *opts);
int cmd_watch(const WatchOptions *opts);
int cmd_attach(const AttachOptions *opts);

/* ---- debug_cli.c ---- */
int cmd_debug(const DebugOptions *opts);

/* seam analyze subcommand — argv already shifted past "seam" */
int mkdbg_cmd_seam(int argc, char *argv[]);

#endif /* MKDBG_H */
