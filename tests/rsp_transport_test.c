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
    int end_status;
} FakeTransport;

static int fake_read(void *ctx, uint8_t *buf, int len, int timeout_ms)
{
    FakeTransport *f = ctx;
    (void)timeout_ms;
    if (f->input_pos == f->input_len)
        return f->end_status ? f->end_status : TRANSPORT_ERR_TIMEOUT;
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

static void test_unsolicited_stop_transactions(void)
{
    static const struct {
        const char *input;
        const char *command;
        const char *response;
        const char *output;
        const char *label;
    } cases[] = {
        {"$S05#b8+$abcd#8a", "g", "abcd", "$g#67++",
         "stop before command ACK does not retransmit the command"},
        {"+$S05#b8$abcd#8a", "g", "abcd", "$g#67++",
         "stop after command ACK is not returned as the register response"},
        {"$T05thread:1;0:deadbeef;#9c+$abcd#8a", "g", "abcd", "$g#67++",
         "extended stop notification is consumed as one complete packet"},
        {"+$T05thread:1;0:deadbeef;#9c$abcd#8a", "g", "abcd", "$g#67++",
         "extended stop after ACK does not overflow a small response buffer"},
        {"$S05#b8+$S02#b5$abcd#8a", "g", "abcd", "$g#67+++",
         "stops on both sides of command ACK preserve response alignment"},
        {"$S05#00$S05#b8+$abcd#8a", "g", "abcd", "$g#67-++",
         "corrupt stop requests response retransmission without resending command"},
        {"-$S05#b8+$abcd#8a", "g", "abcd", "$g#67$g#67++",
         "only an explicit command NAK retransmits the command"},
        {"noise$S05#b8+$abcd#8a", "g", "abcd", "$g#67++",
         "noise before ACK does not retransmit the command"},
        {"+$S05#b8", "?", "S05", "$?#3f+",
         "halt query still returns its requested stop reply"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        FakeTransport fake = {
            .input = (const uint8_t *)cases[i].input,
            .input_len = strlen(cases[i].input),
        };
        WireTransport t = make_transport(&fake);
        char response[16];
        expect(rsp_transaction_t(&t, cases[i].command, response,
                                  sizeof(response)) == WIRE_OK, cases[i].label);
        expect(strcmp(response, cases[i].response) == 0, cases[i].label);
        expect(fake.input_pos == fake.input_len, cases[i].label);
        expect(fake.output_len == strlen(cases[i].output) &&
               memcmp(fake.output, cases[i].output, fake.output_len) == 0,
               cases[i].label);
    }
}

static void test_ack_errors_do_not_retransmit(void)
{
    static const struct {
        const char *input;
        int result;
        const char *output;
        const char *label;
    } cases[] = {
        {"$S0#83", WIRE_ERR_PARSE, "$g#67+", "malformed stop is rejected"},
        {"$OK#9a", WIRE_ERR_PARSE, "$g#67+", "unexpected pre-ACK response is rejected"},
        {"$S05#", WIRE_ERR_IO, "$g#67", "disconnect inside a stop packet is propagated"},
        {"---", WIRE_ERR_CHECKSUM, "$g#67$g#67$g#67", "command NAK retry limit is preserved"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        FakeTransport fake = {
            .input = (const uint8_t *)cases[i].input,
            .input_len = strlen(cases[i].input),
            .end_status = TRANSPORT_ERR_CLOSED,
        };
        WireTransport t = make_transport(&fake);
        char response[16];
        expect(rsp_transaction_t(&t, "g", response, sizeof(response)) == cases[i].result,
               cases[i].label);
        expect(fake.output_len == strlen(cases[i].output) &&
               memcmp(fake.output, cases[i].output, fake.output_len) == 0,
               cases[i].label);
    }
}

static void test_continue_ignores_pre_ack_stop(void)
{
    static const uint8_t input[] = "$S05#b8+$S00#b3$S02#b5";
    FakeTransport fake = {.input = input, .input_len = sizeof(input) - 1};
    WireTransport t = make_transport(&fake);
    char response[16];

    expect(rsp_continue_t(&t, response, sizeof(response), NULL, NULL) == WIRE_OK,
           "continue accepts an old stop while waiting for command ACK");
    expect(strcmp(response, "S02") == 0,
           "continue waits for its new stop rather than returning an old halt");
    expect(fake.output_len == 8 && memcmp(fake.output, "$c#63+++", 8) == 0,
           "continue is sent once and all stop packets are acknowledged");
}

int main(void)
{
    test_send_packet();
    test_transaction();
    test_bad_checksum();
    test_response_overflow();
    test_unsolicited_stop_transactions();
    test_ack_errors_do_not_retransmit();
    test_continue_ignores_pre_ack_stop();
    puts("rsp_transport_test: OK");
    return 0;
}
