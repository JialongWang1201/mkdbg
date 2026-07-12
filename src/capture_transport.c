#include "capture_transport.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CAPTURE_VERSION 1U
#define CAPTURE_ARCH_SIZE 32U
#define CAPTURE_MAX_RECORD (1024U * 1024U)

static const uint8_t capture_magic[8] = {'M','K','D','B','G','C','A','P'};

typedef struct {
    WireTransport *inner;
    FILE *file;
} CaptureCtx;

typedef struct {
    FILE *file;
    uint8_t *leftover;
    uint32_t leftover_len;
    uint32_t leftover_pos;
    int32_t leftover_status;
} ReplayCtx;

static int write_all(FILE *file, const void *data, size_t len)
{
    return fwrite(data, 1, len, file) == len ? 0 : -1;
}

static int read_all(FILE *file, void *data, size_t len)
{
    return fread(data, 1, len, file) == len ? 0 : -1;
}

static int write_u32(FILE *file, uint32_t value)
{
    uint8_t bytes[4] = {
        (uint8_t)value, (uint8_t)(value >> 8),
        (uint8_t)(value >> 16), (uint8_t)(value >> 24),
    };
    return write_all(file, bytes, sizeof(bytes));
}

static int write_u64(FILE *file, uint64_t value)
{
    uint8_t bytes[8];
    for (unsigned i = 0; i < 8; i++) bytes[i] = (uint8_t)(value >> (i * 8));
    return write_all(file, bytes, sizeof(bytes));
}

static int read_u32(FILE *file, uint32_t *value)
{
    uint8_t bytes[4];
    if (read_all(file, bytes, sizeof(bytes)) != 0) return -1;
    *value = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
             ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
    return 0;
}

static int read_u64(FILE *file, uint64_t *value)
{
    uint8_t bytes[8];
    if (read_all(file, bytes, sizeof(bytes)) != 0) return -1;
    *value = 0;
    for (unsigned i = 0; i < 8; i++) *value |= (uint64_t)bytes[i] << (i * 8);
    return 0;
}

static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int write_record(FILE *file, uint8_t direction, int32_t status,
                        const uint8_t *data, uint32_t len)
{
    uint8_t reserved[3] = {0};
    if (write_all(file, &direction, 1) != 0 ||
        write_all(file, reserved, sizeof(reserved)) != 0 ||
        write_u32(file, (uint32_t)status) != 0 ||
        write_u64(file, monotonic_ns()) != 0 ||
        write_u32(file, len) != 0 ||
        (len != 0 && write_all(file, data, len) != 0)) return -1;
    return fflush(file) == 0 ? 0 : -1;
}

static int capture_read(void *ctx, uint8_t *buf, int len, int timeout_ms)
{
    CaptureCtx *capture = ctx;
    int result = capture->inner->read(capture->inner->ctx, buf, len, timeout_ms);
    uint32_t payload_len = result > 0 ? (uint32_t)result : 0;
    if (write_record(capture->file, 'R', result, buf, payload_len) != 0)
        return TRANSPORT_ERR_IO;
    return result;
}

static int capture_write(void *ctx, const uint8_t *buf, int len)
{
    CaptureCtx *capture = ctx;
    int result = capture->inner->write(capture->inner->ctx, buf, len);
    uint32_t payload_len = len > 0 ? (uint32_t)len : 0;
    if (write_record(capture->file, 'W', result, buf, payload_len) != 0)
        return TRANSPORT_ERR_IO;
    return result;
}

static void capture_close(void *ctx)
{
    CaptureCtx *capture = ctx;
    transport_destroy(capture->inner);
    fclose(capture->file);
    free(capture);
}

WireTransport *capture_transport_wrap(WireTransport *inner, const char *path,
                                      const char *arch_name)
{
    if (!inner || !path || !arch_name) {
        transport_destroy(inner);
        return NULL;
    }
    FILE *file = fopen(path, "wb");
    if (!file) {
        fprintf(stderr, "mkdbg: cannot create debug capture %s: %s\n",
                path, strerror(errno));
        transport_destroy(inner);
        return NULL;
    }
    char arch[CAPTURE_ARCH_SIZE] = {0};
    snprintf(arch, sizeof(arch), "%s", arch_name);
    if (write_all(file, capture_magic, sizeof(capture_magic)) != 0 ||
        write_u32(file, CAPTURE_VERSION) != 0 ||
        write_all(file, arch, sizeof(arch)) != 0 ||
        write_u32(file, inner->capabilities) != 0 ||
        write_u32(file, (uint32_t)inner->register_count) != 0) {
        fclose(file);
        transport_destroy(inner);
        return NULL;
    }

    CaptureCtx *ctx = calloc(1, sizeof(*ctx));
    WireTransport *transport = calloc(1, sizeof(*transport));
    if (!ctx || !transport) {
        free(ctx);
        free(transport);
        fclose(file);
        transport_destroy(inner);
        return NULL;
    }
    ctx->inner = inner;
    ctx->file = file;
    *transport = (WireTransport){
        .read = capture_read,
        .write = capture_write,
        .close = capture_close,
        .ctx = ctx,
        .capabilities = inner->capabilities,
        .register_count = inner->register_count,
    };
    return transport;
}

static int replay_next_record(ReplayCtx *replay, uint8_t expected_direction,
                              int32_t *status, uint8_t **data, uint32_t *len)
{
    uint8_t direction;
    uint8_t reserved[3];
    uint32_t raw_status;
    uint64_t timestamp;
    if (read_all(replay->file, &direction, 1) != 0 ||
        read_all(replay->file, reserved, sizeof(reserved)) != 0 ||
        read_u32(replay->file, &raw_status) != 0 ||
        read_u64(replay->file, &timestamp) != 0 ||
        read_u32(replay->file, len) != 0 || direction != expected_direction ||
        *len > CAPTURE_MAX_RECORD) return -1;
    (void)timestamp;
    *status = (int32_t)raw_status;
    *data = NULL;
    if (*len != 0) {
        *data = malloc(*len);
        if (!*data || read_all(replay->file, *data, *len) != 0) {
            free(*data);
            *data = NULL;
            return -1;
        }
    }
    return 0;
}

static int replay_read(void *ctx, uint8_t *buf, int len, int timeout_ms)
{
    ReplayCtx *replay = ctx;
    (void)timeout_ms;
    if (replay->leftover_pos == replay->leftover_len) {
        free(replay->leftover);
        replay->leftover = NULL;
        replay->leftover_pos = 0;
        replay->leftover_len = 0;
        if (replay_next_record(replay, 'R', &replay->leftover_status,
                               &replay->leftover, &replay->leftover_len) != 0)
            return TRANSPORT_ERR_CLOSED;
        if (replay->leftover_status <= 0) return replay->leftover_status;
    }
    uint32_t available = replay->leftover_len - replay->leftover_pos;
    uint32_t n = available < (uint32_t)len ? available : (uint32_t)len;
    memcpy(buf, replay->leftover + replay->leftover_pos, n);
    replay->leftover_pos += n;
    return (int)n;
}

static int replay_write(void *ctx, const uint8_t *buf, int len)
{
    ReplayCtx *replay = ctx;
    int32_t status;
    uint8_t *expected = NULL;
    uint32_t expected_len = 0;
    if (replay_next_record(replay, 'W', &status, &expected, &expected_len) != 0)
        return TRANSPORT_ERR_IO;
    int matches = expected_len == (uint32_t)len &&
                  (expected_len == 0 || memcmp(expected, buf, expected_len) == 0);
    free(expected);
    return matches ? status : TRANSPORT_ERR_IO;
}

static void replay_close(void *ctx)
{
    ReplayCtx *replay = ctx;
    free(replay->leftover);
    fclose(replay->file);
    free(replay);
}

WireTransport *capture_replay_transport_open(const char *path,
                                             const char *expected_arch)
{
    if (!path || !expected_arch) return NULL;
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    uint8_t magic[sizeof(capture_magic)];
    uint32_t version, capabilities, register_count;
    char arch[CAPTURE_ARCH_SIZE];
    if (read_all(file, magic, sizeof(magic)) != 0 ||
        memcmp(magic, capture_magic, sizeof(magic)) != 0 ||
        read_u32(file, &version) != 0 || version != CAPTURE_VERSION ||
        read_all(file, arch, sizeof(arch)) != 0 ||
        memchr(arch, '\0', sizeof(arch)) == NULL ||
        strcmp(arch, expected_arch) != 0 ||
        read_u32(file, &capabilities) != 0 ||
        read_u32(file, &register_count) != 0) {
        fclose(file);
        return NULL;
    }
    ReplayCtx *ctx = calloc(1, sizeof(*ctx));
    WireTransport *transport = calloc(1, sizeof(*transport));
    if (!ctx || !transport) {
        free(ctx);
        free(transport);
        fclose(file);
        return NULL;
    }
    ctx->file = file;
    *transport = (WireTransport){
        .read = replay_read,
        .write = replay_write,
        .close = replay_close,
        .ctx = ctx,
        .capabilities = capabilities,
        .register_count = (int)register_count,
    };
    return transport;
}
