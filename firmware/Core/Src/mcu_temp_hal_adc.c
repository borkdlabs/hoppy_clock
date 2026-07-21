/*******************************************************************************
 * @file mcu_temp_hal_adc.c
 * @brief STM32L432KC internal temperature sense via ADC1_IN17.
 *******************************************************************************
 * @note:
 * VREFINT and the temperature sensor are sampled together as a 2-rank regular
 * sequence driven by DMA (one-shot, normal mode). mcu_temp_start() kicks a
 * conversion and returns immediately; the DMA transfer-complete interrupt
 * (HAL_ADC_ConvCpltCallback) converts the raw counts to degrees C and caches
 * the result. get_mcu_temp() returns the cached value without touching the ADC,
 * so no caller ever blocks on a conversion.
 *******************************************************************************
 */

/** Includes. *****************************************************************/

#include "mcu_temp_hal_adc.h"
#include <math.h>

/** Private variables. ********************************************************/

// DMA (WORD) target for the 2-rank regular sequence. Rank order is set in
// MX_ADC1_Init(): s_adc_raw[0] = VREFINT, s_adc_raw[1] = TEMPSENSOR.
static uint32_t s_adc_raw[2];

// Latest temperature in degrees C, computed in the conversion-complete ISR. 0
// until the first sample lands.
static volatile float s_mcu_temp_c = 0;

/** User implementations into STM32 HAL (overwrite weak HAL functions). *******/

void HAL_ADC_ConvCpltCallback_mcu_temp(ADC_HandleTypeDef *hadc) {
  if (hadc->Instance != MCU_TEMP_HADC.Instance) {
    return;
  }
  // Get actual VDDA from VREFINT, then convert the temperature count using the
  // factory calibration.
  const uint32_t vdda_mv =
      __HAL_ADC_CALC_VREFANALOG_VOLTAGE(s_adc_raw[0], MCU_TEMP_ADC_RESOLUTION);
  s_mcu_temp_c = (float)__HAL_ADC_CALC_TEMPERATURE(vdda_mv, s_adc_raw[1],
                                                   MCU_TEMP_ADC_RESOLUTION);
}

/** Public functions. *********************************************************/

HAL_StatusTypeDef mcu_temp_init(void) {
  // Run ADC single-ended self-calibration.
  return HAL_ADCEx_Calibration_Start(&MCU_TEMP_HADC, ADC_SINGLE_ENDED);
}

HAL_StatusTypeDef mcu_temp_start(void) {
  return HAL_ADC_Start_DMA(&MCU_TEMP_HADC, s_adc_raw, 2);
}

float get_mcu_temp(void) { return s_mcu_temp_c; }
