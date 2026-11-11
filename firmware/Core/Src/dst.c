/*******************************************************************************
 * @file dst.c
 * @brief Automatic daylight saving time: one-hour RTC shifts on a yearly rule.
 *******************************************************************************
 * @note:
 * The RTC keeps local wall-clock time, so twice a year it has to jump an hour.
 * The host hands over the rule with the time (SET_TIME): the two changeovers,
 * each as "the Nth <weekday> of <month> at <hour>", plus whether daylight time
 * is in effect right now.
 *
 * Everything lives in the RTC backup domain, next to the time it describes, so
 * it shares the time's lifetime (kept across resets, lost with the time on a
 * full power loss) and costs nothing in the flash manifest:
 *
 *   RTC_BKP_DR2      The packed rule (0 = no automatic adjustment).
 *   RTC_CR BKP bit   "Daylight time is in effect", i.e. the hour is added.
 *
 * RTC Alarm B is kept armed for the next changeover so it wakes the board from
 * STOP2. dst_service() then moves the clock with the RTC's add/subtract-1-hour
 * operation. It compares the clock against the rule rather than trusting the
 * alarm, so it is safe to call at any time and a changeover the alarm missed is
 * caught up at boot.
 *******************************************************************************
 */

/** Includes. *****************************************************************/

#include "dst.h"
#include "rtc.h"

/** Definitions. **************************************************************/

// Backup register holding the packed rule (DR1 is the time-set marker).
#define DST_BKP_RULE RTC_BKP_DR2

// Packed edge: month [3:0] | week [6:4] | weekday [9:7] | hour [14:10].
#define DST_EDGE_BITS 15u
#define DST_EDGE_MASK ((1u << DST_EDGE_BITS) - 1u)

#define SECONDS_PER_HOUR 3600u
#define SECONDS_PER_DAY 86400u

// The wire layout is the struct itself: eight single bytes, no padding.
_Static_assert(sizeof(dst_rule_t) == 8u, "dst_rule_t must be 8 bytes");

/** Private types. ************************************************************/

/**
 * @brief The RTC reading the rule is compared against.
 */
typedef struct {
  uint8_t year; // 00..99 -> 2000..2099.
  uint8_t month;
  uint8_t date;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
} dst_now_t;

/** Private functions. ********************************************************/

static uint32_t pack_edge(const dst_edge_t *e) {
  return (uint32_t)e->month | ((uint32_t)e->week << 4) |
         ((uint32_t)e->weekday << 7) | ((uint32_t)e->hour << 10);
}

static void unpack_edge(uint32_t bits, dst_edge_t *e) {
  e->month = (uint8_t)(bits & 0x0Fu);
  e->week = (uint8_t)((bits >> 4) & 0x07u);
  e->weekday = (uint8_t)((bits >> 7) & 0x07u);
  e->hour = (uint8_t)((bits >> 10) & 0x1Fu);
}

static bool edge_valid(const dst_edge_t *e) {
  return e->month >= 1u && e->month <= 12u && e->week >= 1u &&
         e->week <= DST_WEEK_LAST && e->weekday >= 1u && e->weekday <= 7u;
}

static void load_rule(dst_rule_t *rule) {
  const uint32_t bits = HAL_RTCEx_BKUPRead(&hrtc, DST_BKP_RULE);
  unpack_edge(bits & DST_EDGE_MASK, &rule->start);
  unpack_edge((bits >> DST_EDGE_BITS) & DST_EDGE_MASK, &rule->end);
}

static void read_now(dst_now_t *now) {
  uint8_t weekday; // Unused: the rule works from the date.
  get_date_time_fields(&now->year, &now->month, &now->date, &weekday,
                       &now->hour, &now->minute, &now->second);
}

/**
 * @brief Days in a month, for the 2000..2099 range (every 4th year is leap).
 */
static uint8_t days_in_month(uint8_t year, uint8_t month) {
  static const uint8_t days[12] = {31, 28, 31, 30, 31, 30,
                                   31, 31, 30, 31, 30, 31};
  return (month == 2u && (year % 4u) == 0u) ? 29u : days[month - 1u];
}

/**
 * @brief Weekday (Mon = 1 .. Sun = 7) of a date in 2000..2099 (Sakamoto).
 */
static uint8_t weekday_of(uint8_t year, uint8_t month, uint8_t date) {
  static const uint8_t offset[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  uint32_t y = 2000u + year;
  if (month < 3u) {
    y -= 1u;
  }
  const uint32_t sun0 =
      (y + y / 4u - y / 100u + y / 400u + offset[month - 1u] + date) % 7u;
  return (sun0 == 0u) ? 7u : (uint8_t)sun0;
}

/**
 * @brief Day of the month an edge falls on in a given year.
 */
static uint8_t edge_date(const dst_edge_t *e, uint8_t year) {
  const uint8_t first = weekday_of(year, e->month, 1u);
  uint8_t date = (uint8_t)(1u + (uint8_t)(e->weekday + 7u - first) % 7u +
                           (e->week - 1u) * 7u);
  // "Last" is asked for as a fifth occurrence, step back if there is none.
  if (date > days_in_month(year, e->month)) {
    date -= 7u;
  }
  return date;
}

/**
 * @brief Order a moment within its year: later moments give larger keys.
 *
 * Not a count of real seconds (every month is given 32 days) but it preserves
 * order, and one hour is still exactly 3600 apart while the day does not
 * change, which the hour limits on the rule guarantee.
 */
static uint32_t year_key(uint8_t month, uint8_t date, uint8_t hour,
                         uint8_t minute, uint8_t second) {
  return ((uint32_t)month * 32u + date) * SECONDS_PER_DAY +
         (uint32_t)hour * SECONDS_PER_HOUR + (uint32_t)minute * 60u + second;
}

static uint32_t edge_key(const dst_edge_t *e, uint8_t year) {
  return year_key(e->month, edge_date(e, year), e->hour, 0u, 0u);
}

static uint32_t now_key(const dst_now_t *now) {
  return year_key(now->month, now->date, now->hour, now->minute, now->second);
}

/**
 * @brief Whether daylight time should be in effect at an RTC reading.
 *
 * The reading is in whichever frame the clock is currently running (active:
 * daylight time, otherwise standard time), and the two edges are brought into
 * that same frame before comparing: the start is defined in standard time and
 * the end in daylight time, an hour apart from the other frame. Carrying the
 * frame is what tells the two passes through the repeated hour apart.
 */
static bool in_effect(const dst_rule_t *rule, const dst_now_t *now,
                      bool active) {
  const uint32_t start =
      edge_key(&rule->start, now->year) + (active ? SECONDS_PER_HOUR : 0u);
  const uint32_t end =
      edge_key(&rule->end, now->year) - (active ? 0u : SECONDS_PER_HOUR);
  const uint32_t key = now_key(now);

  if (start < end) {
    return key >= start && key < end; // Northern: daylight time mid-year.
  }
  return key >= start || key < end; // Southern: it spans the new year.
}

/**
 * @brief Record whether the clock is on daylight time (RTC_CR BKP bit).
 */
static void set_active(bool active) {
  if (active) {
    HAL_RTC_DST_SetStoreOperation(&hrtc);
  } else {
    HAL_RTC_DST_ClearStoreOperation(&hrtc);
  }
}

static bool is_active(void) {
  return HAL_RTC_DST_ReadStoreOperation(&hrtc) != 0u;
}

/**
 * @brief Arm RTC Alarm B for the next changeover after now.
 *
 * Alarm B matches the day of the month and the time, not the month, so it can
 * also fire in a month with no changeover. dst_service() finds nothing due and
 * arms it again, which costs one brief wake a month at most.
 */
static void arm_next(const dst_rule_t *rule, const dst_now_t *now,
                     bool active) {
  // On daylight time the next change is the end, otherwise the start. Either
  // way it is read in the frame the clock is running in now.
  const dst_edge_t *next = active ? &rule->end : &rule->start;

  uint8_t date = edge_date(next, now->year);
  if (now_key(now) >= year_key(next->month, date, next->hour, 0u, 0u)) {
    date = edge_date(next, (uint8_t)(now->year + 1u)); // This year's is past.
  }

  RTC_AlarmTypeDef alarm = {0};
  alarm.AlarmTime.Hours = next->hour;
  alarm.AlarmTime.Minutes = 0u;
  alarm.AlarmTime.Seconds = 0u;
  alarm.AlarmTime.SubSeconds = 0u;
  alarm.AlarmTime.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
  alarm.AlarmTime.StoreOperation = RTC_STOREOPERATION_RESET;
  alarm.AlarmMask = RTC_ALARMMASK_NONE;
  alarm.AlarmSubSecondMask = RTC_ALARMSUBSECONDMASK_ALL;
  alarm.AlarmDateWeekDaySel = RTC_ALARMDATEWEEKDAYSEL_DATE;
  alarm.AlarmDateWeekDay = date;
  alarm.Alarm = RTC_ALARM_B;

  HAL_RTC_SetAlarm_IT(&hrtc, &alarm, RTC_FORMAT_BIN);
}

/**
 * @brief Move the calendar one hour and record the new frame.
 */
static void shift_clock(bool forward) {
  if (forward) {
    HAL_RTC_DST_Add1Hour(&hrtc);
  } else {
    HAL_RTC_DST_Sub1Hour(&hrtc);
  }
  set_active(forward);

  // Reads come from shadow registers that trail the calendar by a couple of
  // RTC clock periods. Wait for them to catch up, so that whoever reads the
  // time next (the alarm re-arm) sees the shifted hour, not the old one.
  __HAL_RTC_WRITEPROTECTION_DISABLE(&hrtc);
  (void)HAL_RTC_WaitForSynchro(&hrtc);
  __HAL_RTC_WRITEPROTECTION_ENABLE(&hrtc);
}

/** Public functions. *********************************************************/

bool dst_rule_valid(const dst_rule_t *rule) {
  if (rule->start.month == 0u) {
    return true; // Off.
  }
  // The hour limits keep each jump within one day (see dst_rule_t). Distinct
  // months keep the two edges from overlapping.
  return edge_valid(&rule->start) && edge_valid(&rule->end) &&
         rule->start.hour <= 22u && rule->end.hour >= 1u &&
         rule->end.hour <= 23u && rule->start.month != rule->end.month;
}

void dst_configure(const dst_rule_t *rule, bool active) {
  if (rule->start.month == 0u) {
    HAL_RTCEx_BKUPWrite(&hrtc, DST_BKP_RULE, 0u);
    set_active(false);
    HAL_RTC_DeactivateAlarm(&hrtc, RTC_ALARM_B);
    return;
  }

  HAL_RTCEx_BKUPWrite(&hrtc, DST_BKP_RULE,
                      pack_edge(&rule->start) |
                          (pack_edge(&rule->end) << DST_EDGE_BITS));

  // The time just set is right by definition, so settle the frame from the
  // rule without moving the clock. The host's flag only matters in the hour
  // that runs twice.
  dst_now_t now;
  read_now(&now);
  active = in_effect(rule, &now, active);
  set_active(active);
  arm_next(rule, &now, active);
}

bool dst_get(dst_rule_t *rule) {
  load_rule(rule);
  return rule->start.month != 0u && is_active();
}

dst_step_t dst_service(void) {
  dst_rule_t rule;
  load_rule(&rule);

  // The stored rule was validated before it was written, but the backup domain
  // is re-checked here so a corrupt register cannot index out of range.
  if (rtc_is_unset() || rule.start.month == 0u || !dst_rule_valid(&rule)) {
    HAL_RTC_DeactivateAlarm(&hrtc, RTC_ALARM_B);
    return DST_STEP_NONE;
  }

  dst_now_t now;
  read_now(&now);

  dst_step_t step = DST_STEP_NONE;
  bool active = is_active();
  if (in_effect(&rule, &now, active) != active) {
    active = !active;
    shift_clock(active);
    step = active ? DST_STEP_FORWARD : DST_STEP_BACK;
    read_now(&now);
  }

  arm_next(&rule, &now, active);
  return step;
}
