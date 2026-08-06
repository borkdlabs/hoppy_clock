/*******************************************************************************
 * @file rtc.h
 * @brief General driver abstracting STM32 HAL: RTC.
 *******************************************************************************
 */

#ifndef HOPPY_CLOCK__RTC_H
#define HOPPY_CLOCK__RTC_H

/** Includes. *****************************************************************/

#include "stm32l4xx_hal.h"
#include <stdbool.h>

/** Defines. ******************************************************************/

/**
 * @brief Backup-register marker proving the RTC holds a user-set time.
 *
 * set_date() stamps this into RTC_BKP_DR1. The value survives any reset that
 * keeps the backup domain powered (the RTC keeps counting), and is only lost
 * when the backup domain loses power (full power loss) which causes RTC to fall
 * back to its 2000 epoch. So "marker present" == "time is valid", thus both
 * MX_RTC_Init() and rtc_is_unset() key off it.
 */
#define RTC_BKUP_SET_MARKER 0x2345u

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

/**
 * @brief Get the RTC date and time as numeric fields.
 *
 * Reads the time before the date, as the STM32 RTC requires, to unlock the
 * shadow registers correctly.
 *
 * @param year Out: year index (00-99 -> 2000-2099).
 * @param month Out: month index (1-12).
 * @param date Out: day of the month (1-31).
 * @param day Out: weekday (Monday = 1, ..., Sunday = 7).
 * @param hours Out: hour (0-23).
 * @param minutes Out: minute (0-59).
 * @param seconds Out: second (0-59).
 */
void get_date_time_fields(uint8_t *year, uint8_t *month, uint8_t *date,
                          uint8_t *day, uint8_t *hours, uint8_t *minutes,
                          uint8_t *seconds);

/**
 * @brief Whether the RTC has never been set (or lost due to a power loss).
 *
 * Keys off the RTC_BKUP_SET_MARKER backup-register stamp rather than the
 * calendar year: the marker is written when the user sets the time and cleared
 * only when the backup domain loses power (the same event that drops the RTC to
 * its 2000 epoch).
 *
 * @return true if the clock has not been set, false once a real time is set.
 */
bool rtc_is_unset(void);

#endif
