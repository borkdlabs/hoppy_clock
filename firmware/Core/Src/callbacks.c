/*******************************************************************************
 * @file callbacks.c
 * @brief STM32 HAL callback implementations overriding weak declarations.
 *******************************************************************************
 */

/** Includes. *****************************************************************/

#include "mcu_temp_hal_adc.h"

/** Collection of user implementations into STM32 HAL (overwriting HAL). ******/

/** ADC. */

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc) {
  HAL_ADC_ConvCpltCallback_mcu_temp(hadc);
}
