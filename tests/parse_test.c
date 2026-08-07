#include "mkdbg.h"

static int failures;

static void expect(int condition, const char *message)
{
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", message);
    failures++;
  }
}

static void parse_bad_serial_baud(void)
{
  char *args[] = {"--baud", "115200junk"};
  SerialOptions opts;
  (void)parse_serial_args(2, args, &opts);
}

static void parse_bad_serial_delay(void)
{
  char *args[] = {"--char-delay-ms", "nan"};
  SerialOptions opts;
  (void)parse_serial_args(2, args, &opts);
}

static void parse_bad_debug_offset(void)
{
  char *args[] = {"--freertos-tcb-offset", "-2"};
  DebugOptions opts;
  (void)parse_debug_args(2, args, &opts);
}

static void expect_parse_failure(void (*parse_case)(void), const char *message)
{
  pid_t pid = fork();
  int status = 0;

  if (pid < 0) {
    expect(0, "fork parser failure test");
    return;
  }
  if (pid == 0) {
    parse_case();
    _exit(0);
  }
  expect(waitpid(pid, &status, 0) == pid, "wait for parser failure test");
  expect(WIFEXITED(status) && WEXITSTATUS(status) == 2, message);
}

int main(void)
{
  char *serial_args[] = {
      "--baud", "115200", "--char-delay-ms", "2.5"};
  char *debug_args[] = {"--freertos-tcb-offset", "-1"};
  SerialOptions serial_opts;
  DebugOptions debug_opts;
  unsigned long value = 0;

  expect(parse_serial_args(4, serial_args, &serial_opts) == 0 &&
             serial_opts.baud == 115200 && serial_opts.char_delay_ms == 2.5,
         "accept valid baud and character delay");
  expect(parse_debug_args(2, debug_args, &debug_opts) == 0 &&
             debug_opts.freertos_name_offset == -1,
         "accept documented automatic FreeRTOS offset");

  expect_parse_failure(parse_bad_serial_baud,
                       "reject baud with trailing characters");
  expect_parse_failure(parse_bad_serial_delay,
                       "reject non-finite character delay");
  expect_parse_failure(parse_bad_debug_offset,
                       "reject FreeRTOS offset below sentinel");

  expect(parse_ulong_range("0xffffffff", 0, UINT32_MAX, &value) == 0 &&
             value == UINT32_MAX,
         "accept maximum 32-bit address");
  expect(parse_ulong_range("0x100000000", 0, UINT32_MAX, &value) != 0,
         "reject overflowing 32-bit address");
  expect(parse_ulong_range("-1", 0, UINT32_MAX, &value) != 0,
         "reject negative unsigned value");
  expect(parse_ulong_range("", 0, UINT32_MAX, &value) != 0,
         "reject empty unsigned value");

  return failures == 0 ? 0 : 1;
}
