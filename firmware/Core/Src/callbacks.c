/*******************************************************************************
 * @file callbacks.c
 * @brief STM32 HAL callback implementations overriding weak declarations.
 *******************************************************************************
 */

/** Includes. *****************************************************************/

#include "alarm_rt.h"
#include "mcu_temp_hal_adc.h"
#include "pam8302a_hal_dac.h"
#include "stm32l4xx_hal.h"

/** Collection of user implementations into STM32 HAL (overwriting HAL). ******/

/** RTC. */

void HAL_RTC_AlarmAEventCallback(RTC_HandleTypeDef *hrtc) {
  (void)hrtc;
  alarm_rt_notify_fired();
}

/** ADC. */

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc) {
  HAL_ADC_ConvCpltCallback_mcu_temp(hadc);
}

/** DAC. */

void HAL_DAC_ConvHalfCpltCallbackCh1(DAC_HandleTypeDef *hdac) {
  HAL_DAC_ConvHalfCpltCallbackCh1_pam8302a(hdac);
}

void HAL_DAC_ConvCpltCallbackCh1(DAC_HandleTypeDef *hdac) {
  HAL_DAC_ConvCpltCallbackCh1_pam8302a(hdac);
}

void HAL_DAC_ErrorCallbackCh1(DAC_HandleTypeDef *hdac) {
  HAL_DAC_ErrorCallbackCh1_pam8302a(hdac);
}
