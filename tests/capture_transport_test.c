#include "capture_transport.h"
#include "transport.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    const uint8_t *read_data;
    int read_len;
    int read_pos;
    int closed;
} FakeCtx;

static int fake_read(void *ctx, uint8_t *buf, int len, int timeout_ms)
{
    FakeCtx *fake = ctx;
    (void)timeout_ms;
    if (fake->read_pos == fake->read_len) return TRANSPORT_ERR_TIMEOUT;
    int n = fake->read_len - fake->read_pos;
    if (n > len) n = len;
    memcpy(buf, fake->read_data + fake->read_pos, (size_t)n);
    fake->read_pos += n;
    return n;
}

static int fake_write(void *ctx, const uint8_t *buf, int len)
{
    (void)ctx;
    (void)buf;
    return len;
}

static void fake_close(void *ctx)
{
    ((FakeCtx *)ctx)->closed = 1;
}

void transport_destroy(WireTransport *transport)
{
    if (!transport) return;
    if (transport->close) transport->close(transport->ctx);
    free(transport);
}

static void expect(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

static WireTransport *make_fake(FakeCtx *ctx)
{
    WireTransport *transport = calloc(1, sizeof(*transport));
    expect(transport != NULL, "fake transport allocation succeeds");
    *transport = (WireTransport){
        .read = fake_read,
        .write = fake_write,
        .close = fake_close,
        .ctx = ctx,
        .capabilities = TRANSPORT_CAP_FPU_REGS,
        .register_count = 50,
    };
    return transport;
}

static void test_capture_round_trip(void)
{
    char path[] = "/tmp/mkdbg-capture-XXXXXX";
    int fd = mkstemp(path);
    expect(fd >= 0, "temporary capture path is created");
    close(fd);

    static const uint8_t read_data[] = {'+', '$', 'O', 'K', '#', '9', 'a'};
    FakeCtx fake = {.read_data = read_data, .read_len = (int)sizeof(read_data)};
    WireTransport *capture = capture_transport_wrap(make_fake(&fake), path, "cortex-m");
    expect(capture != NULL, "capture transport opens");
    static const uint8_t request[] = "$?#3f";
    uint8_t response[sizeof(read_data)];
    expect(capture->write(capture->ctx, request, sizeof(request) - 1) ==
           (int)sizeof(request) - 1, "capture records write");
    expect(capture->read(capture->ctx, response, sizeof(response), 20) ==
           (int)sizeof(response), "capture records read");
    transport_destroy(capture);
    expect(fake.closed, "capture closes wrapped transport");

    WireTransport *replay = capture_replay_transport_open(path, "cortex-m");
    expect(replay != NULL, "capture reopens for replay");
    expect(replay->capabilities == TRANSPORT_CAP_FPU_REGS,
           "capabilities survive replay");
    expect(replay->register_count == 50, "register count survives replay");
    expect(replay->write(replay->ctx, request, sizeof(request) - 1) ==
           (int)sizeof(request) - 1, "replay accepts matching write");
    memset(response, 0, sizeof(response));
    expect(replay->read(replay->ctx, response, 2, 20) == 2,
           "replay supports partial reads");
    expect(replay->read(replay->ctx, response + 2, sizeof(response) - 2, 20) ==
           (int)sizeof(response) - 2, "replay returns remaining bytes");
    expect(memcmp(response, read_data, sizeof(response)) == 0,
           "replay preserves response bytes");
    transport_destroy(replay);

    replay = capture_replay_transport_open(path, "riscv32");
    expect(replay == NULL, "architecture mismatch is rejected");
    unlink(path);
}

static void test_replay_detects_protocol_drift(void)
{
    char path[] = "/tmp/mkdbg-capture-XXXXXX";
    int fd = mkstemp(path);
    expect(fd >= 0, "temporary capture path is created");
    close(fd);
    FakeCtx fake = {0};
    WireTransport *capture = capture_transport_wrap(make_fake(&fake), path, "cortex-m");
    static const uint8_t request[] = "$g#67";
    expect(capture->write(capture->ctx, request, sizeof(request) - 1) > 0,
           "capture records protocol request");
    transport_destroy(capture);

    WireTransport *replay = capture_replay_transport_open(path, "cortex-m");
    static const uint8_t changed[] = "$?#3f";
    expect(replay->write(replay->ctx, changed, sizeof(changed) - 1) == TRANSPORT_ERR_IO,
           "replay rejects protocol drift");
    transport_destroy(replay);
    unlink(path);
}

int main(void)
{
    test_capture_round_trip();
    test_replay_detects_protocol_drift();
    puts("capture_transport_test: OK");
    return 0;
}
