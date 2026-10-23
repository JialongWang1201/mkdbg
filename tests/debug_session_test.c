#include "arch.h"
#include "debug_session.h"
#include "transport.h"
#include "uart_transport.h"
#include "wire_host.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t input[2048];
    size_t input_len;
    size_t input_pos;
    uint8_t output[2048];
    size_t output_len;
    int closed;
    size_t timeout_at;
    int timeouts_remaining;
    int interrupts;
    int fail_interrupt;
    int close_when_empty;
} FakeTransport;

static int fake_read(void *ctx, uint8_t *buf, int len, int timeout_ms)
{
    FakeTransport *f = ctx;
    (void)timeout_ms;
    if (f->input_pos == f->timeout_at && f->timeouts_remaining > 0) {
        f->timeouts_remaining--;
        return TRANSPORT_ERR_TIMEOUT;
    }
    if (f->input_pos == f->input_len)
        return f->close_when_empty ? TRANSPORT_ERR_CLOSED : TRANSPORT_ERR_TIMEOUT;
    size_t n = f->input_len - f->input_pos;
    if (n > (size_t)len) n = (size_t)len;
    memcpy(buf, f->input + f->input_pos, n);
    f->input_pos += n;
    return (int)n;
}

static int fake_write(void *ctx, const uint8_t *buf, int len)
{
    FakeTransport *f = ctx;
    if (len == 1 && buf[0] == 0x03) {
        f->interrupts++;
        if (f->fail_interrupt) return TRANSPORT_ERR_IO;
    }
    if (f->output_len + (size_t)len > sizeof(f->output)) return TRANSPORT_ERR_IO;
    memcpy(f->output + f->output_len, buf, (size_t)len);
    f->output_len += (size_t)len;
    return len;
}

static void fake_close(void *ctx)
{
    ((FakeTransport *)ctx)->closed = 1;
}

void transport_destroy(WireTransport *t)
{
    if (!t) return;
    if (t->close) t->close(t->ctx);
    free(t);
}

WireTransport *uart_transport_open(const char *port, int baud)
{
    (void)port;
    (void)baud;
    return NULL;
}

static void expect(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

static void append_packet(FakeTransport *f, const char *payload)
{
    uint8_t checksum = 0;
    for (const char *p = payload; *p; p++) checksum = (uint8_t)(checksum + (uint8_t)*p);
    int n = snprintf((char *)f->input + f->input_len,
                     sizeof(f->input) - f->input_len,
                     "+$%s#%02x", payload, checksum);
    expect(n > 0 && (size_t)n < sizeof(f->input) - f->input_len,
           "scripted packet fits");
    f->input_len += (size_t)n;
}

static DebugSession *open_fake(FakeTransport *fake, const char *arch_name)
{
    WireTransport *t = malloc(sizeof(*t));
    expect(t != NULL, "transport allocation succeeds");
    *t = (WireTransport){
        .read = fake_read,
        .write = fake_write,
        .close = fake_close,
        .ctx = fake,
    };
    DebugSession *s = debug_session_open_transport(t, mkdbg_arch_find(arch_name));
    expect(s != NULL, "session opens");
    return s;
}

static void test_declared_register_mismatch(void)
{
    FakeTransport fake = {0};
    WireTransport *t = malloc(sizeof(*t));
    expect(t != NULL, "transport allocation succeeds");
    *t = (WireTransport){
        .read = fake_read,
        .write = fake_write,
        .close = fake_close,
        .ctx = &fake,
        .register_count = 16,
    };

    expect(debug_session_open_transport(t, mkdbg_arch_find("cortex-m")) == NULL,
           "declared register mismatch rejects session");
    expect(fake.closed, "rejected transport is closed");
}

static void append_registers(char *out, size_t out_size, int count)
{
    size_t pos = 0;
    for (int i = 0; i < count; i++) {
        int n = snprintf(out + pos, out_size - pos, "%02x000000", i & 0xff);
        expect(n == 8, "register encoding fits");
        pos += 8;
    }
}

static void test_required_cortex_registers(void)
{
    FakeTransport fake = {0};
    char payload[17 * 8 + 1] = {0};
    append_registers(payload, sizeof(payload), 17);
    append_packet(&fake, payload);
    DebugSession *s = open_fake(&fake, "cortex-m");
    uint32_t regs[DEBUG_SESSION_MAX_REGS];

    expect(debug_session_read_regs(s, regs) == WIRE_OK, "core registers parse");
    expect(regs[0] == 0 && regs[16] == 16, "register values use little endian");
    expect(regs[17] == 0 && regs[49] == 0, "unavailable optional registers are zero");
    debug_session_close(s);
    expect(fake.closed, "session closes owned transport");
}

static void test_short_register_reply(void)
{
    FakeTransport fake = {0};
    char payload[16 * 8 + 1] = {0};
    append_registers(payload, sizeof(payload), 16);
    append_packet(&fake, payload);
    DebugSession *s = open_fake(&fake, "cortex-m");
    uint32_t regs[DEBUG_SESSION_MAX_REGS];

    expect(debug_session_read_regs(s, regs) == WIRE_ERR_PARSE,
           "missing required register is rejected");
    debug_session_close(s);
}

static void test_misaligned_register_reply(void)
{
    FakeTransport fake = {0};
    char payload[17 * 8 + 2] = {0};
    append_registers(payload, sizeof(payload), 17);
    strcat(payload, "f");
    append_packet(&fake, payload);
    DebugSession *s = open_fake(&fake, "cortex-m");
    uint32_t regs[DEBUG_SESSION_MAX_REGS];

    expect(debug_session_read_regs(s, regs) == WIRE_ERR_PARSE,
           "partial register is rejected");
    debug_session_close(s);
}

static void test_short_memory_reply(void)
{
    FakeTransport fake = {0};
    append_packet(&fake, "aabb");
    DebugSession *s = open_fake(&fake, "cortex-m");
    uint8_t bytes[4] = {0};

    expect(debug_session_read_mem(s, 0x20000000, sizeof(bytes), bytes) == WIRE_ERR_PARSE,
           "short memory response is rejected");
    debug_session_close(s);
}

static void test_immediate_stop_reply(void)
{
    FakeTransport fake = {0};
    append_packet(&fake, "S05");
    DebugSession *s = open_fake(&fake, "cortex-m");

    expect(debug_session_continue(s) == WIRE_OK, "immediate stop reply succeeds");
    expect(debug_session_last_signal(s) == 5, "stop signal is retained");
    debug_session_close(s);
}

static void test_delayed_and_fragmented_continue_reply(void)
{
    // Delay both before a reply and inside its checksum. Idle reads must not
    // restart the command or discard an in-progress packet.
    const size_t positions[] = {1, 4, 6, 7};
    for (size_t i = 0; i < sizeof(positions) / sizeof(positions[0]); i++) {
        FakeTransport fake = {.timeout_at = positions[i], .timeouts_remaining = 3};
        append_packet(&fake, "S05");
        DebugSession *s = open_fake(&fake, "cortex-m");
        expect(debug_session_continue(s) == WIRE_OK, "delayed stop is retained");
        expect(debug_session_last_signal(s) == 5, "only real halt sets signal");
        expect(fake.output_len == 6 && memcmp(fake.output, "$c#63+", 6) == 0,
               "idle wait does not retransmit continue");
        debug_session_close(s);
    }
}

static int request_after_idle(void *ctx)
{
    return ((FakeTransport *)ctx)->timeouts_remaining == 0;
}

static void test_continue_interrupt_and_following_transaction(void)
{
    FakeTransport fake = {.timeouts_remaining = 3};
    append_packet(&fake, "S00");
    fake.timeout_at = fake.input_len;
    append_packet(&fake, "S02");
    char registers[17 * 8 + 1] = {0};
    append_registers(registers, sizeof(registers), 17);
    append_packet(&fake, registers);
    DebugSession *s = open_fake(&fake, "cortex-m");
    expect(debug_session_continue_interruptible(s, request_after_idle, &fake) == WIRE_OK,
           "running confirmation is skipped and interrupt waits for halt");
    expect(fake.interrupts == 1, "persistent request sends Ctrl-C only once");
    expect(debug_session_last_signal(s) == 2, "interrupt halt signal is retained");
    uint32_t regs[DEBUG_SESSION_MAX_REGS];
    expect(debug_session_read_regs(s, regs) == WIRE_OK && regs[16] == 16,
           "next command has no stale run-control reply");
    debug_session_close(s);
}

static void test_continue_failures(void)
{
    const char *replies[] = {"E01", "Sx5", ""};
    const int errors[] = {WIRE_ERR_IO, WIRE_ERR_PARSE, WIRE_ERR_IO};
    for (size_t i = 0; i < 3; i++) {
        FakeTransport fake = {0};
        append_packet(&fake, replies[i]);
        DebugSession *s = open_fake(&fake, "cortex-m");
        expect(debug_session_continue(s) == errors[i], "invalid stop fails explicitly");
        expect(debug_session_last_signal(s) == 0, "failed continue does not invent a halt");
        debug_session_close(s);
    }
    FakeTransport fake = {.fail_interrupt = 1};
    append_packet(&fake, "S00");
    DebugSession *s = open_fake(&fake, "cortex-m");
    expect(debug_session_continue_interruptible(s, request_after_idle, &fake) == WIRE_ERR_IO,
           "interrupt write failure is propagated");
    debug_session_close(s);
}

static void test_continue_nak_and_disconnect(void)
{
    FakeTransport fake = {0};
    fake.input[fake.input_len++] = '-';
    append_packet(&fake, "S05");
    DebugSession *s = open_fake(&fake, "cortex-m");
    expect(debug_session_continue(s) == WIRE_OK, "NAK retries continue");
    expect(fake.output_len == 11, "NAK causes exactly one retransmission");
    debug_session_close(s);

    memset(&fake, 0, sizeof(fake));
    fake.close_when_empty = 1;
    append_packet(&fake, "S00");
    s = open_fake(&fake, "cortex-m");
    expect(debug_session_continue(s) == WIRE_ERR_IO, "disconnect aborts the running wait");
    expect(debug_session_last_signal(s) == 0, "disconnect does not invent a stop signal");
    debug_session_close(s);
}

static void test_managed_breakpoints(void)
{
    FakeTransport fake = {0}, other = {0};
    DebugSession *s = open_fake(&fake, "cortex-m");
    DebugSession *second = open_fake(&other, "cortex-m");
    append_packet(&fake, "OK");
    expect(debug_session_add_breakpoint(s, 0x8001000) == 1, "session assigns first ID");
    size_t written = fake.output_len;
    expect(debug_session_add_breakpoint(s, 0x8001000) == 1 && fake.output_len == written,
           "duplicate breakpoint does not consume a hardware comparator");
    append_packet(&other, "OK");
    expect(debug_session_add_breakpoint(second, 0x8002000) == 1,
           "another session has independent IDs");
    append_packet(&fake, "E01");
    expect(debug_session_remove_breakpoint(s, 1) == WIRE_ERR_IO &&
           debug_session_breakpoint_count(s) == 1, "failed clear keeps session record");
    append_packet(&fake, "OK");
    expect(debug_session_remove_breakpoint(s, 1) == WIRE_OK, "same ID can retry clear");
    expect(debug_session_breakpoint_at(s, 0) == NULL, "removed record is absent");
    for (int i = 0; i < DEBUG_SESSION_MAX_BREAKPOINTS; i++) {
        append_packet(&fake, "OK");
        expect(debug_session_add_breakpoint(s, 0x8002000 + (uint32_t)i * 4) == i + 2,
               "IDs are not reused after clear");
    }
    written = fake.output_len;
    expect(debug_session_add_breakpoint(s, 0x8003000) == WIRE_ERR_OVERFLOW &&
           fake.output_len == written, "full registry rejects add before target I/O");
    expect(debug_session_breakpoint_at(s, DEBUG_SESSION_MAX_BREAKPOINTS) == NULL,
           "registry view bounds are checked");
    debug_session_close(s);
    debug_session_close(second);
}

static void test_managed_watchpoints(void)
{
    FakeTransport fake = {0};
    DebugSession *s = open_fake(&fake, "cortex-m");
    append_packet(&fake, "OK");
    expect(debug_session_add_watchpoint(s, 0x20000000, WATCHPOINT_READ) == 1,
           "session records watchpoint type");
    size_t written = fake.output_len;
    expect(debug_session_add_watchpoint(s, 0x20000000, WATCHPOINT_READ) == 1 &&
           debug_session_add_watchpoint(s, 0x20000000, WATCHPOINT_WRITE) == WIRE_ERR_PARSE &&
           fake.output_len == written, "duplicate/conflicting watchpoints do not touch target");
    append_packet(&fake, "E01");
    expect(debug_session_remove_watchpoint(s, 1) == WIRE_ERR_IO &&
           debug_session_watchpoint_at(s, 0)->type == WATCHPOINT_READ,
           "failed watchpoint clear keeps its type and ID");
    expect(memcmp(fake.output + written, "$z3,20000000,1", 13) == 0,
           "watchpoint clear uses its actual type");
    append_packet(&fake, "OK");
    expect(debug_session_remove_watchpoint(s, 1) == WIRE_OK &&
           debug_session_watchpoint_count(s) == 0, "watchpoint retry removes record on OK");
    written = fake.output_len;
    expect(debug_session_remove_watchpoint(s, 1) == WIRE_ERR_PARSE && fake.output_len == written,
           "unknown watchpoint does not send a packet");
    debug_session_close(s);
}

int main(void)
{
    test_declared_register_mismatch();
    test_required_cortex_registers();
    test_short_register_reply();
    test_misaligned_register_reply();
    test_short_memory_reply();
    test_immediate_stop_reply();
    test_delayed_and_fragmented_continue_reply();
    test_continue_interrupt_and_following_transaction();
    test_continue_failures();
    test_continue_nak_and_disconnect();
    test_managed_breakpoints();
    test_managed_watchpoints();
    puts("debug_session_test: OK");
    return 0;
}
