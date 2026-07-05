#include "probe_bridge.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hex_nibble(uint8_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int read_packet(ProbeHandle *handle, char *payload, size_t payload_size)
{
    size_t len = 0;
    int started = 0;
    int checksum_bytes = 0;
    uint8_t checksum[2] = {0};
    for (int attempts = 0; attempts < 20; attempts++) {
        uint8_t buf[256];
        int n = probe_read(handle, buf, sizeof(buf), 500);
        if (n == -2) continue;
        if (n <= 0) return -1;
        for (int i = 0; i < n; i++) {
            uint8_t byte = buf[i];
            if (!started) {
                if (byte == '$') started = 1;
                continue;
            }
            if (checksum_bytes != 0) {
                checksum[checksum_bytes - 1] = byte;
                checksum_bytes++;
                if (checksum_bytes == 3) {
                    int hi = hex_nibble(checksum[0]);
                    int lo = hex_nibble(checksum[1]);
                    uint8_t sum = 0;
                    if (hi < 0 || lo < 0) return -1;
                    for (size_t j = 0; j < len; j++) sum += (uint8_t)payload[j];
                    if (sum != (uint8_t)((hi << 4) | lo)) return -1;
                    payload[len] = '\0';
                    return (int)len;
                }
            } else if (byte == '#') {
                checksum_bytes = 1;
            } else {
                if (len + 1 >= payload_size) return -1;
                payload[len++] = (char)byte;
            }
        }
    }
    return -1;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s PROBE_INDEX CHIP\n", argv[0]);
        return 2;
    }
    char *end = NULL;
    long index = strtol(argv[1], &end, 10);
    if (!end || *end != '\0' || index < 0 || index > INT32_MAX) {
        fprintf(stderr, "invalid probe index: %s\n", argv[1]);
        return 2;
    }

    ProbeInfo probes[16];
    int count = probe_list(probes, 16);
    if (count <= index) {
        fprintf(stderr, "probe %ld unavailable; detected %d\n", index, count);
        return 1;
    }
    ProbeHandle *handle = probe_open((int)index, argv[2]);
    if (!handle) return 1;

    ProbeCapabilities caps;
    if (probe_get_capabilities(handle, &caps) != 0 ||
        (caps.register_count != 17 && caps.register_count != 50)) {
        fprintf(stderr, "invalid target capabilities\n");
        probe_close(handle);
        return 1;
    }

    static const uint8_t request[] = "$g#67";
    if (probe_write(handle, request, sizeof(request) - 1) !=
        (int)sizeof(request) - 1) {
        fprintf(stderr, "register request failed\n");
        probe_close(handle);
        return 1;
    }
    char payload[50 * 8 + 1];
    int payload_len = read_packet(handle, payload, sizeof(payload));
    if (payload_len != (int)caps.register_count * 8) {
        fprintf(stderr, "register payload mismatch: got %d, expected %u\n",
                payload_len, caps.register_count * 8);
        probe_close(handle);
        return 1;
    }

    printf("probe_hardware_test: OK probe=%ld chip=%s registers=%u fpu=%s\n",
           index, argv[2], caps.register_count,
           (caps.flags & PROBE_CAP_FPU_REGS) ? "yes" : "no");
    probe_close(handle);
    return 0;
}
