#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>

void board_uart_write(const char *s);
int board_uart_read_char(char *out);
void board_led_on(void);
void board_led_off(void);
void board_led_toggle(void);

#endif
