/**
 *******************************************************************************
 * @file:  ws2812b_hal_pwm.c
 * @brief: WS2812B functions: abstracting STM32 HAL: PWM.
 *******************************************************************************
 * @note:
 * Based on Phil's Lab "STM32 + RGB LEDs Firmware Tutorial (TIM + DMA) - Phil's
 * Lab #136" (https://youtu.be/MqbJTj0Cw6o).
 *******************************************************************************
 */

/** Includes. *****************************************************************/

#include "ws2812b_hal_pwm.h"

/** Public variables. *********************************************************/

ws2812b_led_data_t led_data[LED_COUNT_MAX];
uint16_t dma_buffer[WS2812B_DMA_BUF_LEN];
volatile uint8_t dma_complete_flag;

/** Private variables. ********************************************************/

// Active LEDs clocked out per update (1..LED_COUNT_MAX).
static uint16_t s_led_count = LED_COUNT_DEFAULT;

/** User implementations into STM32 HAL (overwrite weak HAL functions). *******/

void HAL_TIM_PWM_PulseFinishedCallback(TIM_HandleTypeDef *htim) {
  if (htim->Instance != WS2812B_TIM_INSTANCE) {
    return;
  }

  ws2812b_callback();
}

/** Public functions. *********************************************************/

HAL_StatusTypeDef ws2812b_init(void) {
  // Initialize PWM timer.
  const HAL_StatusTypeDef hal_status = HAL_TIM_PWM_Init(&WS2812B_TIM);

  // Clear DMA buffer.
  for (uint16_t buffer_i = 0; buffer_i < WS2812B_DMA_BUF_LEN; buffer_i++) {
    dma_buffer[buffer_i] = 0;
  }

  // Set DMA ready flag.
  dma_complete_flag = 1;

  return hal_status;
}

void ws2812b_set_colour(const uint8_t index, const uint8_t r, const uint8_t g,
                        const uint8_t b) {
  led_data[index].colour.r = r;
  led_data[index].colour.g = g;
  led_data[index].colour.b = b;
}

HAL_StatusTypeDef ws2812b_update(void) {
  // Check if previous DMA transfer has been completed.
  if (!dma_complete_flag) {
    return HAL_BUSY;
  }

  uint16_t buffer_i = 0;

  // For each active LED.
  for (uint16_t led = 0; led < s_led_count; led++) {

    // Loop through all 24 data bits.
    for (uint8_t bits = 0; bits < WS2812B_BITS_PER_LED; bits++, buffer_i++) {

      // Calculate total bit shift.
      const uint8_t byte = bits / 8 * 8; // Calculate byte offset from all bits.
      const uint8_t bit = 7 - bits % 8;  // Calculate bit offset from all bits.
      const uint8_t bit_index = byte + bit; // Calculate total bit shift.

      // Update DMA buffer to match LED data.
      if (led_data[led].data >> bit_index & 0x01)   // If bit is set.
        dma_buffer[buffer_i] = WS2812B_HI_VAL_DUTY; // High.
      else
        dma_buffer[buffer_i] = WS2812B_LO_VAL_DUTY; // Low.
    }
  }

  // Reset (latch) slots, placed right after the active data so the transfer
  // length tracks the current LED count (a shrunk count can't clock stale bits).
  for (uint16_t r = 0; r < WS2812B_RST_VAL_PERIODS; r++, buffer_i++) {
    dma_buffer[buffer_i] = 0;
  }

  // Attempt DMA transfer of exactly the active data + reset.
  const HAL_StatusTypeDef hal_status =
      HAL_TIM_PWM_Start_DMA(&WS2812B_TIM, WS2812B_TIM_CHANNEL,
                            (const uint32_t *)dma_buffer, buffer_i);
  // Note: Here the dma_buffer is uint16_t as DMA is configured 16-bit memory to
  // 16-bit peripheral. HAL_TIM_PWM_Start_DMA() expects 32-bit since DMA can do
  // up to word (32-bit) transactions. In this case, only the lower 16 bits of
  // each array element will be used.

  // If DMA transfer started successfully.
  if (hal_status == HAL_OK) {
    dma_complete_flag = 0; // Clear DMA flag.
  }

  return hal_status;
}

void ws2812b_callback(void) {
  HAL_TIM_PWM_Stop_DMA(&WS2812B_TIM, WS2812B_TIM_CHANNEL);
  dma_complete_flag = 1;
}

void ws2812b_set_count(uint16_t count) {
  if (count < 1u) {
    count = 1u;
  } else if (count > LED_COUNT_MAX) {
    count = LED_COUNT_MAX;
  }
  s_led_count = count;
}

uint16_t ws2812b_get_count(void) { return s_led_count; }
