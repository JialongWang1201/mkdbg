/* bsp/wire_port.c — Board-specific UART hooks for wire GDB RSP stub
 *
 * Implements the two hooks wire.h requires:
 *   wire_uart_write(buf, len)   — blocking UART TX (same UART as seam / CLI)
 *   wire_uart_read(byte)        — blocking UART RX, returns 0 on success
 *
 * wire uses the debug UART (BOARD_UART_PORT, same instance as the serial
 * console).  While a GDB session is active the debug loop in wire_rsp.c
 * holds the CPU; normal console I/O is suspended until GDB sends 'c'.
 *
 * Call wire_init(ram_start, ram_end) once at startup (from main, before
 * the RTOS scheduler starts) to register the RAM bounds and install the
 * fault handlers that catch HardFault / MemManage / BusFault / UsageFault.
 *
 * SPDX-License-Identifier: MIT
 */

#include "wire.h"
#include "stm32f4xx.h"

#ifndef BOARD_UART_PORT
#  define BOARD_UART_PORT 2
#endif

#if BOARD_UART_PORT == 1
#  define WIRE_UART USART1
#elif BOARD_UART_PORT == 2
#  define WIRE_UART USART2
#else
#  define WIRE_UART USART3
#endif

/* ── wire_uart_write: blocking TX — safe from fault handler context ───────── */

void wire_uart_write(const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        while ((WIRE_UART->SR & USART_SR_TXE) == 0U) {}
        WIRE_UART->DR = (uint16_t)buf[i];
    }
    while ((WIRE_UART->SR & USART_SR_TC) == 0U) {}
}

/* ── wire_uart_read: blocking RX — returns 0 when a byte is available ─────── */

int wire_uart_read(uint8_t *byte)
{
    while ((WIRE_UART->SR & USART_SR_RXNE) == 0U) {}
    *byte = (uint8_t)(WIRE_UART->DR & 0xFFU);
    return 0;
}
