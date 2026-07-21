/*******************************************************************************
 * @file mcu_temp_hal_adc.h
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

#ifndef HOPPY_CLOCK__MCU_TEMP_HAL_ADC_H
#define HOPPY_CLOCK__MCU_TEMP_HAL_ADC_H

/** Includes. *****************************************************************/

#include "stm32l4xx_hal.h"

/** STM32 port and pin configs. ***********************************************/

extern ADC_HandleTypeDef hadc1;

// ADC.
#define MCU_TEMP_HADC hadc1

/** Definitions. **************************************************************/

// ADC resolution used for the conversions.
#define MCU_TEMP_ADC_RESOLUTION ADC_RESOLUTION_12B

// Sampling time for the internal channels. Both the temperature sensor and
// VREFINT require a long minimum sampling time.
#define MCU_TEMP_ADC_SAMPLINGTIME ADC_SAMPLETIME_640CYCLES_5

/** User implementations into STM32 HAL (overwrite weak HAL functions). *******/

void HAL_ADC_ConvCpltCallback_mcu_temp(ADC_HandleTypeDef *hadc);

/** Public functions. *********************************************************/

/**
 * @brief Initialize the MCU temperature sense ADC.
 *
 * Runs the ADC single-ended self-calibration sequence. Must be called once,
 * after the ADC peripheral is initialized (MX_ADC1_Init) and before the first
 * call to mcu_temp_start(). Calibration requires the ADC to be disabled, so this
 * must not be called while a conversion is in progress.
 *
 * @return HAL_OK on success, HAL error status otherwise.
 */
HAL_StatusTypeDef mcu_temp_init(void);

/**
 * @brief Launch a non-blocking VREFINT + temperature conversion via DMA.
 *
 * Kicks a single 2-rank regular sequence into the DMA buffer and return. The
 * result is cached by the transfer-complete interrupt and read back via
 * get_mcu_temp(). Intended to be called periodically from the scheduler.
 *
 * @return HAL_OK if the conversion was launched, HAL_BUSY if the previous one is
 * still in flight, HAL error status otherwise.
 */
HAL_StatusTypeDef mcu_temp_start(void);

/**
 * @brief Get the most recent cached STM32L432KC internal temperature.
 *
 * Returns the value cached by the last completed mcu_temp_start() conversion.
 *
 * @return STM32L432KC internal temperature in degrees Celsius as float.
 * @retval == 0 -> no sample taken yet.
 */
float get_mcu_temp(void);

#endif
