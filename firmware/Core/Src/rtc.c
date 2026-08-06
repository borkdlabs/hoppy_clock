/*******************************************************************************
 * @file rtc.c
 * @brief General driver abstracting STM32 HAL: RTC.
 *******************************************************************************
 */

/** Includes. *****************************************************************/

#include <stdio.h>

#include "rtc.h"

/** Public functions. *********************************************************/

void set_date(uint8_t year, uint8_t month, uint8_t date, uint8_t day) {
  RTC_DateTypeDef sDate = {0};
  sDate.WeekDay = day;
  sDate.Month = month;
  sDate.Date = date;
  sDate.Year = year;
  if (HAL_RTC_SetDate(&hrtc, &sDate, RTC_FORMAT_BIN) != HAL_OK) {
    // TODO: Error handler.
  }

  // Stamp the "time is valid" marker so a reset that keeps the backup domain
  // powered won't be mistaken for an unset clock (see rtc_is_unset).
  HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR1, RTC_BKUP_SET_MARKER);
}

void set_time(uint8_t hours, uint8_t minutes, uint8_t seconds) {
  RTC_TimeTypeDef sTime = {0};
  sTime.Hours = hours;
  sTime.Minutes = minutes;
  sTime.Seconds = seconds;
  if (HAL_RTC_SetTime(&hrtc, &sTime, RTC_FORMAT_BIN) != HAL_OK) {
    // TODO: Error handler.
  }
}

void get_time_date(char *time, char *date) {
  RTC_DateTypeDef gDate;
  RTC_TimeTypeDef gTime;

  // Get the date and time.
  HAL_RTC_GetTime(&hrtc, &gTime, RTC_FORMAT_BIN);
  HAL_RTC_GetDate(&hrtc, &gDate, RTC_FORMAT_BIN);

  // Format date and time.
  snprintf(time, 9, "%02d:%02d:%02d", gTime.Hours, gTime.Minutes,
           gTime.Seconds);
  snprintf(date, 11, "%02d-%02d-%04d", gDate.Date, gDate.Month,
           2000 + gDate.Year);
}

void get_date_time_fields(uint8_t *year, uint8_t *month, uint8_t *date,
                          uint8_t *day, uint8_t *hours, uint8_t *minutes,
                          uint8_t *seconds) {
  RTC_TimeTypeDef gTime;
  RTC_DateTypeDef gDate;

  // Read time first, then date (unlocks the RTC shadow registers).
  HAL_RTC_GetTime(&hrtc, &gTime, RTC_FORMAT_BIN);
  HAL_RTC_GetDate(&hrtc, &gDate, RTC_FORMAT_BIN);

  *year = gDate.Year;
  *month = gDate.Month;
  *date = gDate.Date;
  *day = gDate.WeekDay;
  *hours = gTime.Hours;
  *minutes = gTime.Minutes;
  *seconds = gTime.Seconds;
}

bool rtc_is_unset(void) {
  // The marker is present iff set_date() has run and the backup domain has kept
  // power since. Full power loss clears it and drops the RTC to the 2000 epoch.
  return HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR1) != RTC_BKUP_SET_MARKER;
}
