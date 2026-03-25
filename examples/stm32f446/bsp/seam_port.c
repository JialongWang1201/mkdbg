/* bsp/seam_port.c — Board-specific port functions for seam_agent.h
 *
 * Defines the shared ring buffer storage (SEAM_IMPLEMENT) and implements
 * the two port functions seam_agent.h requires:
 *   seam_port_tick()            — DWT cycle counter (always-on, fault-safe)
 *   seam_port_write_block()     — Blocking UART TX, safe from fault handlers
 *
 * Call seam_port_init() once at startup (from main, before RTOS starts).
 *
 * SEAM_IMPLEMENT must be defined in exactly one translation unit.
 * All other files that call seam_emit() include seam_agent.h without it.
 */
#define SEAM_IMPLEMENT
#include "seam_agent.h"
#include "stm32f4xx.h"
#include "build_info.h"

/* ── Board ID: low 16 bits of git SHA fragment from build_info.h ─────────── */
uint16_t seam_port_board_id(void)
{
    return (uint16_t)(BUILD_INFO_ID & 0xFFFFu);
}

/* ── seam_port_init: enable DWT cycle counter ───────────────────────────── */
void seam_port_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT       = 0U;
    DWT->CTRL        |= DWT_CTRL_CYCCNTENA_Msk;
}

/* ── Tick: DWT cycle counter — runs regardless of RTOS/scheduler state ─── */
uint32_t seam_port_tick(void)
{
    return DWT->CYCCNT;
}

/* ── Write block: busy-wait UART TX — safe in HardFault/MemManage context ─
 *
 * Uses the same UART instance as board_uart (UART_INSTANCE via stm32f4xx.h).
 * Busy-wait is intentional: from a fault handler the RTOS is frozen and
 * DMA/interrupt-driven TX cannot be relied on.
 */
#ifndef BOARD_UART_PORT
#define BOARD_UART_PORT 2
#endif

#if BOARD_UART_PORT == 1
#define SEAM_UART USART1
#elif BOARD_UART_PORT == 2
#define SEAM_UART USART2
#else
#define SEAM_UART USART3
#endif

void seam_port_write_block(const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        while ((SEAM_UART->SR & USART_SR_TXE) == 0U) {}
        SEAM_UART->DR = (uint16_t)buf[i];
    }
    /* Wait for last byte to fully shift out before returning */
    while ((SEAM_UART->SR & USART_SR_TC) == 0U) {}
}
