/* tests/seam_host_stub.c — host-build stub for seam_agent.h port functions
 *
 * Provides the ring storage (SEAM_IMPLEMENT) and no-op port functions so
 * that src/vm32.c and src/kdi.c can be compiled and tested on the host
 * without a real MCU or UART.
 */
#define SEAM_IMPLEMENT
#include "seam_agent.h"

uint32_t seam_port_tick(void)            { return 0; }
uint16_t seam_port_board_id(void)        { return 0; }
void     seam_port_write_block(const uint8_t *buf, size_t len)
{
    (void)buf;
    (void)len;
}
