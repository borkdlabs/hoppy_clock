/*******************************************************************************
 * @file rtc.h
 * @brief General driver abstracting STM32 HAL: RTC.
 *******************************************************************************
 */

#ifndef MOMENTUM__RTC_H
#define MOMENTUM__RTC_H

/** Includes. *****************************************************************/

#include "stm32l4xx_hal.h"

/** STM32 port and pin configs. ***********************************************/

extern RTC_HandleTypeDef hrtc;

/** Public functions. *********************************************************/

/**
 * @brief Set RTC date.
 *
 * @param year Year index (00-99 -> 2000-2099).
 * @param month Month index (1-12).
 * @param date Day of the month number (1-31).
 * @param day Weekday number (Monday = 1, Tuesday = 2, ..., Sunday = 7).
 */
void set_date(uint8_t year, uint8_t month, uint8_t date, uint8_t day);

/**
 * @brief Set the RTC time.
 *
 * @param hours Hour value (0-23).
 * @param minutes Minute value (0-59).
 * @param seconds Second value (0-59).
 */
void set_time(uint8_t hours, uint8_t minutes, uint8_t seconds);

/**
 * @brief Get RTC date.
 *
 * @param time Character based time value.
 * @param date Character based date value.
 */
void get_time_date(char *time, char *date);

#endif
