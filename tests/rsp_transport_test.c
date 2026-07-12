#include "rsp_transport.h"
#include "transport.h"
#include "wire_host.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const uint8_t *input;
    size_t input_len;
    size_t input_pos;
    uint8_t output[256];
    size_t output_len;
} FakeTransport;

static int fake_read(void *ctx, uint8_t *buf, int len, int timeout_ms)
{
    FakeTransport *f = ctx;
    (void)timeout_ms;
    if (f->input_pos == f->input_len) return TRANSPORT_ERR_TIMEOUT;
    size_t available = f->input_len - f->input_pos;
    size_t n = available < (size_t)len ? available : (size_t)len;
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

static void expect(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

static WireTransport make_transport(FakeTransport *fake)
{
    WireTransport t = {
        .read = fake_read,
        .write = fake_write,
        .close = NULL,
        .ctx = fake,
    };
    return t;
}

static void test_send_packet(void)
{
    FakeTransport fake = {0};
    WireTransport t = make_transport(&fake);
    expect(rsp_send_packet_t(&t, "g") == WIRE_OK, "send packet succeeds");
    expect(fake.output_len == 5, "packet has expected size");
    expect(memcmp(fake.output, "$g#67", 5) == 0, "packet checksum is correct");
}

static void test_transaction(void)
{
    static const uint8_t input[] = "+$OK#9a";
    FakeTransport fake = {.input = input, .input_len = sizeof(input) - 1};
    WireTransport t = make_transport(&fake);
    char response[16];

    expect(rsp_transaction_t(&t, "?", response, sizeof(response)) == WIRE_OK,
           "transaction succeeds");
    expect(strcmp(response, "OK") == 0, "transaction returns payload");
    expect(fake.output_len == 6, "transaction writes request and response ACK");
    expect(memcmp(fake.output, "$?#3f+", 6) == 0, "transaction output is framed");
}

static void test_bad_checksum(void)
{
    static const uint8_t input[] = "+$OK#00$OK#00$OK#00";
    FakeTransport fake = {.input = input, .input_len = sizeof(input) - 1};
    WireTransport t = make_transport(&fake);
    char response[16];

    expect(rsp_transaction_t(&t, "?", response, sizeof(response)) == WIRE_ERR_CHECKSUM,
           "bad checksums exhaust retries");
}

static void test_response_overflow(void)
{
    static const uint8_t input[] = "+$abcd#8a";
    FakeTransport fake = {.input = input, .input_len = sizeof(input) - 1};
    WireTransport t = make_transport(&fake);
    char response[4];

    expect(rsp_transaction_t(&t, "?", response, sizeof(response)) == WIRE_ERR_OVERFLOW,
           "oversized response is rejected");
}

int main(void)
{
    test_send_packet();
    test_transaction();
    test_bad_checksum();
    test_response_overflow();
    puts("rsp_transport_test: OK");
    return 0;
}
