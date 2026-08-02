/*******************************************************************************
 * @file alarm.c
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

/** Includes. *****************************************************************/

#include "alarm.h"

/** Definitions. **************************************************************/

#define SECONDS_PER_DAY 86400u
#define SECONDS_PER_WEEK (7u * SECONDS_PER_DAY)

// How many months ahead to search for the next valid day-of-month occurrence.
// A day like the 31st always lands within a few months, so 48 is ample.
#define MONTHLY_SEARCH_MONTHS 48u

/** Private functions. ********************************************************/

static bool is_leap(uint32_t year) {
  return ((year % 4u == 0u) && (year % 100u != 0u)) || (year % 400u == 0u);
}

static uint8_t days_in_month(uint32_t year, uint8_t month) {
  static const uint8_t days[12] = {31, 28, 31, 30, 31, 30,
                                   31, 31, 30, 31, 30, 31};
  if (month == 2u && is_leap(year)) {
    return 29u;
  }
  return days[month - 1u];
}

/**
 * @brief Days since 1970-01-01 for a proleptic-Gregorian date (Hinnant).
 *
 * Valid for the 2000..2099 range this clock uses. Enables exact second-level
 * deltas between two dates without walking the calendar day by day.
 */
static int32_t days_from_civil(uint32_t year, uint32_t month, uint32_t day) {
  year -= (month <= 2u);
  const int32_t era = (int32_t)(year / 400u);
  const uint32_t yoe = year - (uint32_t)era * 400u;
  const uint32_t doy =
      (153u * (month + (month > 2u ? -3u : 9u)) + 2u) / 5u + day - 1u;
  const uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return era * 146097 + (int32_t)doe - 719468;
}

/**
 * @brief Absolute time of a date/time as seconds since 1970-01-01.
 */
static int64_t epoch_seconds(uint32_t year, uint8_t month, uint8_t day,
                             uint8_t hour, uint8_t minute, uint8_t second) {
  int64_t days = days_from_civil(year, month, day);
  return days * (int64_t)SECONDS_PER_DAY + (int64_t)hour * 3600 +
         (int64_t)minute * 60 + (int64_t)second;
}

/**
 * @brief Seconds from now until a weekly alarm's next strictly-future fire.
 *
 * Works within the weekly cycle: 0 (exact match) rolls to a full week so the
 * result is always in [1, SECONDS_PER_WEEK]. Returns 0 if the mask is empty.
 * Also reports the weekday (1..7) the winning occurrence lands on.
 */
static uint32_t weekly_next(const alarm_record_t *a, const alarm_time_t *now,
                            uint8_t *out_weekday) {
  const uint8_t mask = a->day_sel & 0x7Fu;
  if (mask == 0u) {
    return 0u;
  }

  const uint32_t now_wk = (uint32_t)(now->weekday - 1u) * SECONDS_PER_DAY +
                          (uint32_t)now->hour * 3600u +
                          (uint32_t)now->minute * 60u + now->second;

  uint32_t best = 0u;
  uint8_t best_wd = 0u;
  for (uint8_t d = 0u; d < 7u; d++) {
    if ((mask & (uint8_t)(1u << d)) == 0u) {
      continue;
    }
    const uint32_t tgt = (uint32_t)d * SECONDS_PER_DAY +
                         (uint32_t)a->hour * 3600u + (uint32_t)a->minute * 60u +
                         a->second;
    uint32_t delta = (tgt + SECONDS_PER_WEEK - now_wk) % SECONDS_PER_WEEK;
    if (delta == 0u) {
      delta = SECONDS_PER_WEEK; // Strictly future.
    }
    if (best == 0u || delta < best) {
      best = delta;
      best_wd = (uint8_t)(d + 1u); // Back to Mon=1..Sun=7.
    }
  }

  *out_weekday = best_wd;
  return best;
}

/**
 * @brief Seconds from now until a monthly alarm's next strictly-future fire.
 *
 * Searches forward month by month for the first one whose length includes the
 * target day-of-month and whose fire time is after now. Returns 0 if the
 * day-of-month is out of range or none is found within the search window.
 */
static uint32_t monthly_next(const alarm_record_t *a, const alarm_time_t *now,
                             int64_t now_epoch) {
  const uint8_t dom = a->day_sel;
  if (dom < 1u || dom > 31u) {
    return 0u;
  }

  const uint32_t base_year = 2000u + now->year;
  const uint32_t month0 = (uint32_t)now->month - 1u; // 0-based.

  for (uint32_t k = 0u; k < MONTHLY_SEARCH_MONTHS; k++) {
    const uint32_t total = month0 + k;
    const uint32_t year = base_year + total / 12u;
    const uint8_t month = (uint8_t)(total % 12u) + 1u;
    if (dom > days_in_month(year, month)) {
      continue;
    }
    const int64_t cand =
        epoch_seconds(year, month, dom, a->hour, a->minute, a->second);
    if (cand > now_epoch) {
      return (uint32_t)(cand - now_epoch);
    }
  }
  return 0u;
}

/** Public functions. *********************************************************/

bool alarm_next(const manifest_t *m, const alarm_time_t *now,
                alarm_next_t *out) {
  const int64_t now_epoch =
      epoch_seconds(2000u + now->year, now->month, now->date, now->hour,
                    now->minute, now->second);
  bool found = false;
  uint32_t best = 0u;

  for (uint16_t i = 0u; i < m->header.alarm_count; i++) {
    const alarm_record_t *a = &m->alarms[i];
    if ((a->flags & ALARM_FLAG_ENABLED) == 0u) {
      continue;
    }

    uint32_t delta;
    uint8_t day;
    bool monthly = (a->flags & ALARM_FLAG_MONTHLY) != 0u;
    if (monthly) {
      delta = monthly_next(a, now, now_epoch);
      day = a->day_sel;
    } else {
      delta = weekly_next(a, now, &day);
    }
    if (delta == 0u) {
      continue; // Alarm never fires (empty mask / bad day-of-month).
    }

    if (!found || delta < best) {
      best = delta;
      found = true;
      out->index = i;
      out->seconds_until = delta;
      out->monthly = monthly;
      out->day = day;
      out->hour = a->hour;
      out->minute = a->minute;
      out->second = a->second;
    }
  }

  return found;
}

bool alarm_record_matches(const alarm_record_t *a, const alarm_time_t *now) {
  if ((a->flags & ALARM_FLAG_ENABLED) == 0u) {
    return false;
  }
  if (a->hour != now->hour || a->minute != now->minute) {
    return false;
  }
  if ((a->flags & ALARM_FLAG_MONTHLY) != 0u) {
    return a->day_sel == now->date;
  }
  return (a->day_sel & (uint8_t)(1u << (now->weekday - 1u))) != 0u;
}
