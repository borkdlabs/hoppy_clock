/*******************************************************************************
 * @file alarm.h
 * @brief Alarm scheduling logic: next-fire computation and match testing.
 *******************************************************************************
 * @note:
 * Pure calendar logic over the manifest's alarm table, independent of the RTC
 * hardware and power management. Two operations:
 *
 *   alarm_next():
 *     Given the current date/time, find the soonest strictly-future alarm. The
 *     RTC-arming layer uses the returned day/time to program RTC Alarm A.
 *   alarm_record_matches()
 *     On wake, test whether an alarm is firing now (minute resolution, to
 *     tolerate wake latency).
 *
 * Weekly alarms recur on a weekday mask.
 * Monthly alarms recur on a day-of-month.
 *******************************************************************************
 */

#ifndef HOPPY_CLOCK__ALARM_H
#define HOPPY_CLOCK__ALARM_H

/** Includes. *****************************************************************/

#include "manifest.h"
#include <stdbool.h>
#include <stdint.h>

/** Public types. *************************************************************/

/**
 * @brief A wall-clock date/time in the RTC's field representation.
 *
 * weekday is Monday = 1 .. Sunday = 7, matching the STM32 RTC and the
 * ALARM_DAY_* mask (bit0 = Monday).
 */
typedef struct {
  uint8_t year;    // 00..99 -> 2000..2099.
  uint8_t month;   // 1..12.
  uint8_t date;    // Day of month, 1..31.
  uint8_t weekday; // 1..7 (Mon..Sun).
  uint8_t hour;    // 0..23.
  uint8_t minute;  // 0..59.
  uint8_t second;  // 0..59.
} alarm_time_t;

/**
 * @brief Result of alarm_next(): the soonest upcoming alarm and how to arm it.
 */
typedef struct {
  uint16_t index;         // Winning alarm's index in the table.
  uint32_t seconds_until; // Seconds from "now" until it fires (>= 1).
  bool monthly;           // true: arm on day-of-month, false: on weekday.
  uint8_t day;            // Monthly: day-of-month 1..31. Weekly: weekday 1..7.
  uint8_t hour;           // Fire time.
  uint8_t minute;
  uint8_t second;
} alarm_next_t;

/** Public functions. *********************************************************/

/**
 * @brief Find the soonest strictly-future alarm relative to now.
 *
 * Scans all enabled alarms, computing each one's next occurrence (weekly by
 * weekday mask, monthly by day-of-month, both rolling forward past now), and
 * returns the nearest. Disabled alarms, empty weekday masks, and out-of-range
 * days-of-month are ignored.
 *
 * @param m Manifest holding the alarm table.
 * @param now Current date/time.
 * @param out Filled with the winning alarm when true is returned.
 *
 * @return true if at least one alarm is scheduled, false if none.
 */
bool alarm_next(const manifest_t *m, const alarm_time_t *now,
                alarm_next_t *out);

/**
 * @brief Test whether an alarm is firing at the given time (minute resolution).
 *
 * Matches an enabled alarm's day selection (weekday mask or day-of-month) and
 * hour:minute against now. Seconds are ignored so a small wake latency across a
 * second boundary still registers as a match.
 *
 * @param a Alarm record.
 * @param now Current date/time.
 *
 * @return true if the alarm is enabled and its day + hour:minute match now.
 */
bool alarm_record_matches(const alarm_record_t *a, const alarm_time_t *now);

#endif
