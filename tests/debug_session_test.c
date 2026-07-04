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
} FakeTransport;

static int fake_read(void *ctx, uint8_t *buf, int len, int timeout_ms)
{
    FakeTransport *f = ctx;
    (void)timeout_ms;
    if (f->input_pos == f->input_len) return TRANSPORT_ERR_TIMEOUT;
    size_t n = f->input_len - f->input_pos;
    if (n > (size_t)len) n = (size_t)len;
    memcpy(buf, f->input + f->input_pos, n);
    f->input_pos += n;
    return (int)n;
}

static int fake_write(void *ctx, const uint8_t *buf, int len)
{
    FakeTransport *f = ctx;
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

int main(void)
{
    test_required_cortex_registers();
    test_short_register_reply();
    test_misaligned_register_reply();
    test_short_memory_reply();
    test_immediate_stop_reply();
    puts("debug_session_test: OK");
    return 0;
}
