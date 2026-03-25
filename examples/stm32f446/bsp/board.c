#include "board.h"
#include "build_info.h"
#include "stm32f4xx.h"
#include "FreeRTOS.h"
#include "task.h"

// Nucleo-F446RE default: LED on PA5, USART2 on PA2/PA3 (ST-LINK VCP).
// Override BOARD_UART_PORT at build time to switch UART: 1/2/3.
#ifndef BOARD_UART_PORT
#define BOARD_UART_PORT 2
#endif

#define LED_GPIO GPIOA
#define LED_PIN  (1U << 5)

#define UART_AF 7U

#if BOARD_UART_PORT == 1
#define UART_INSTANCE USART1
#define UART_LABEL "USART1"
#define UART_GPIO GPIOA
#define UART_TX_PIN_NUM 9U
#define UART_RX_PIN_NUM 10U
#define UART_GPIO_ENR RCC_AHB1ENR_GPIOAEN
#define UART_APB1_ENR 0U
#define UART_APB2_ENR RCC_APB2ENR_USART1EN
#elif BOARD_UART_PORT == 2
#define UART_INSTANCE USART2
#define UART_LABEL "USART2"
#define UART_GPIO GPIOA
#define UART_TX_PIN_NUM 2U
#define UART_RX_PIN_NUM 3U
#define UART_GPIO_ENR RCC_AHB1ENR_GPIOAEN
#define UART_APB1_ENR RCC_APB1ENR_USART2EN
#define UART_APB2_ENR 0U
#elif BOARD_UART_PORT == 3
#define UART_INSTANCE USART3
#define UART_LABEL "USART3"
#define UART_GPIO GPIOB
#define UART_TX_PIN_NUM 10U
#define UART_RX_PIN_NUM 11U
#define UART_GPIO_ENR RCC_AHB1ENR_GPIOBEN
#define UART_APB1_ENR RCC_APB1ENR_USART3EN
#define UART_APB2_ENR 0U
#else
#error "BOARD_UART_PORT must be 1, 2, or 3"
#endif

#define UART_TX_PIN (1U << UART_TX_PIN_NUM)
#define UART_RX_PIN (1U << UART_RX_PIN_NUM)

#define UART_BAUD 115200U

static void adc_start_conversion(void)
{
  ADC1->CR2 |= ADC_CR2_SWSTART;
  while ((ADC1->SR & ADC_SR_EOC) == 0) {
  }
}

static void gpio_set_af(GPIO_TypeDef *gpio, uint32_t pin_num, uint32_t af)
{
  uint32_t shift = (pin_num % 8U) * 4U;
  uint32_t idx = pin_num / 8U;
  gpio->MODER &= ~(3U << (pin_num * 2U));
  gpio->MODER |= (2U << (pin_num * 2U));
  gpio->AFR[idx] &= ~(0xFU << shift);
  gpio->AFR[idx] |= (af << shift);
}

static void uart_write_char(char c)
{
  while ((UART_INSTANCE->SR & USART_SR_TXE) == 0) {
  }
  UART_INSTANCE->DR = (uint16_t)c;
}

const char *board_name(void)
{
  return BUILD_INFO_BOARD_NAME;
}

uint32_t board_uart_port_number(void)
{
  return (uint32_t)BOARD_UART_PORT;
}

const char *board_uart_port_label(void)
{
  return UART_LABEL;
}

void board_uart_write(const char *s)
{
  while (s && *s) {
    if (*s == '\n') {
      uart_write_char('\r');
    }
    uart_write_char(*s++);
  }
}

int board_uart_read_char(char *out)
{
  if (out == NULL) {
    return 0;
  }
  if ((UART_INSTANCE->SR & USART_SR_RXNE) == 0U) {
    return 0;
  }
  *out = (char)(UART_INSTANCE->DR & 0xFFU);
  return 1;
}

void board_clock_init(void)
{
  // Use HSI (16 MHz) and PLL to 84 MHz SYSCLK.
  RCC->CR |= RCC_CR_HSION;
  while ((RCC->CR & RCC_CR_HSIRDY) == 0) {
  }

  RCC->PLLCFGR = (16U << RCC_PLLCFGR_PLLM_Pos) |
                 (336U << RCC_PLLCFGR_PLLN_Pos) |
                 (1U << RCC_PLLCFGR_PLLP_Pos) | // PLLP = 4
                 (RCC_PLLCFGR_PLLSRC_HSI) |
                 (7U << RCC_PLLCFGR_PLLQ_Pos);

  RCC->CR |= RCC_CR_PLLON;
  while ((RCC->CR & RCC_CR_PLLRDY) == 0) {
  }

  FLASH->ACR = FLASH_ACR_ICEN | FLASH_ACR_DCEN | FLASH_ACR_LATENCY_2WS;

  RCC->CFGR = RCC_CFGR_HPRE_DIV1 |
              RCC_CFGR_PPRE1_DIV2 |
              RCC_CFGR_PPRE2_DIV1;

  RCC->CFGR |= RCC_CFGR_SW_PLL;
  while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) {
  }

  SystemCoreClockUpdate();
}

void board_gpio_init(void)
{
  RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
  RCC->AHB1ENR |= UART_GPIO_ENR;

  // LED (PA5) output.
  LED_GPIO->MODER &= ~(GPIO_MODER_MODER5);
  LED_GPIO->MODER |= GPIO_MODER_MODER5_0;
  LED_GPIO->OTYPER &= ~GPIO_OTYPER_OT5;
  LED_GPIO->OSPEEDR |= GPIO_OSPEEDER_OSPEEDR5_0;
  LED_GPIO->PUPDR &= ~GPIO_PUPDR_PUPDR5;

  // USART TX/RX alternate function.
  gpio_set_af(UART_GPIO, UART_TX_PIN_NUM, UART_AF);
  gpio_set_af(UART_GPIO, UART_RX_PIN_NUM, UART_AF);
}

void board_uart_init(void)
{
  if (UART_APB1_ENR != 0U) {
    RCC->APB1ENR |= UART_APB1_ENR;
  }
  if (UART_APB2_ENR != 0U) {
    RCC->APB2ENR |= UART_APB2_ENR;
  }

  UART_INSTANCE->CR1 = 0;
  UART_INSTANCE->CR2 = 0;
  UART_INSTANCE->CR3 = 0;

  // PCLK1 = SystemCoreClock/2, PCLK2 = SystemCoreClock.
#if BOARD_UART_PORT == 1
  uint32_t pclk = SystemCoreClock;
#else
  uint32_t pclk = SystemCoreClock / 2U;
#endif
  UART_INSTANCE->BRR = (pclk + (UART_BAUD / 2U)) / UART_BAUD;

  UART_INSTANCE->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
}

void board_adc_init(void)
{
  RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;

  ADC->CCR |= ADC_CCR_TSVREFE;
  ADC1->CR2 = ADC_CR2_ADON;

  // Long sample time for internal temperature sensor (channel 16).
  ADC1->SMPR1 &= ~ADC_SMPR1_SMP16;
  ADC1->SMPR1 |= ADC_SMPR1_SMP16_2 | ADC_SMPR1_SMP16_1 | ADC_SMPR1_SMP16_0;

  board_delay_ms(10);
}

uint16_t board_adc_read_temp_raw(void)
{
  ADC1->SQR1 = 0;
  ADC1->SQR3 = 16U;
  adc_start_conversion();
  return (uint16_t)ADC1->DR;
}

uint32_t board_reset_flags_read(void)
{
  return RCC->CSR;
}

void board_reset_flags_clear(void)
{
  RCC->CSR |= RCC_CSR_RMVF;
}

void board_system_reset(void)
{
  __DSB();
  NVIC_SystemReset();
}

void board_led_toggle(void)
{
  LED_GPIO->ODR ^= LED_PIN;
}

void board_led_on(void)
{
  LED_GPIO->ODR |= LED_PIN;
}

void board_led_off(void)
{
  LED_GPIO->ODR &= ~LED_PIN;
}

void board_delay_ms(uint32_t ms)
{
  // Rough busy-wait delay.
  const uint32_t cycles_per_ms = SystemCoreClock / 8000U;
  for (uint32_t i = 0; i < ms; ++i) {
    for (uint32_t j = 0; j < cycles_per_ms; ++j) {
      __NOP();
    }
  }
}

void vApplicationMallocFailedHook(void)
{
  board_uart_write("MallocFailed\r\n");
  for (;;) {
  }
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
  (void)xTask;
  board_uart_write("StackOverflow ");
  if (pcTaskName) {
    board_uart_write(pcTaskName);
  }
  board_uart_write("\r\n");
  for (;;) {
  }
}
