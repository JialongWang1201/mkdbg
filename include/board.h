#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>

void board_clock_init(void);
void board_gpio_init(void);
void board_uart_init(void);
void board_led_toggle(void);
void board_led_on(void);
void board_led_off(void);
void board_delay_ms(uint32_t ms);
void board_uart_write(const char *s);
int board_uart_read_char(char *out);
void board_adc_init(void);
uint16_t board_adc_read_temp_raw(void);
uint32_t board_reset_flags_read(void);
void board_reset_flags_clear(void);
void board_system_reset(void);

#endif
