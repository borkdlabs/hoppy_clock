/*******************************************************************************
 * @file dst.h
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

#ifndef HOPPY_CLOCK__DST_H
#define HOPPY_CLOCK__DST_H

/** Includes. *****************************************************************/

#include <stdbool.h>
#include <stdint.h>

/** Definitions. **************************************************************/

// dst_edge_t.week value meaning "the last <weekday> of the month".
#define DST_WEEK_LAST 5u

/** Public types. *************************************************************/

/**
 * @brief One changeover: the Nth weekday of a month, at a local hour.
 */
typedef struct {
  uint8_t month;   // 1..12. A start month of 0 turns the whole rule off.
  uint8_t week;    // 1..4 = Nth occurrence in the month, DST_WEEK_LAST = last.
  uint8_t weekday; // 1..7 (Mon..Sun), as the RTC counts.
  uint8_t hour;    // Local hour the clock reads when it changes, on the hour.
} dst_edge_t;

/**
 * @brief A yearly daylight saving rule (8 bytes, the SET_TIME wire layout).
 *
 * start is when the clock goes forward, read in standard time: start.hour:00
 * becomes start.hour + 1:00 (hour 0..22). end is when it goes back, read in
 * daylight time: end.hour:00 becomes end.hour - 1:00 (hour 1..23). The limits
 * keep both jumps inside one day, which is all the RTC's one-hour operation
 * supports. A start month after the end month is a southern-hemisphere rule,
 * with daylight time running across the new year.
 */
typedef struct {
  dst_edge_t start;
  dst_edge_t end;
} dst_rule_t;

/**
 * @brief What dst_service() did to the clock.
 */
typedef enum {
  DST_STEP_NONE = 0, // Clock untouched.
  DST_STEP_FORWARD,  // Went forward one hour (an hour was skipped).
  DST_STEP_BACK,     // Went back one hour (an hour now runs a second time).
} dst_step_t;

/** Public functions. *********************************************************/

/**
 * @brief Whether a rule is one this firmware can apply.
 *
 * A rule with start.month == 0 (automatic adjustment off) is valid.
 *
 * @param rule Rule to check.
 *
 * @return true if every field is in range, false otherwise.
 */
bool dst_rule_valid(const dst_rule_t *rule);

/**
 * @brief Store a rule for the time that has just been set, and arm for it.
 *
 * Call straight after writing the RTC. The clock itself is never moved here:
 * the time just set is taken as correct, and only the "daylight time is in
 * effect" state is recorded against it.
 *
 * @param rule Rule to store, already checked with dst_rule_valid().
 * @param active Whether the time just set is daylight time. Only decides the
 * hour that runs twice, anywhere else the rule and the date settle it.
 */
void dst_configure(const dst_rule_t *rule, bool active);

/**
 * @brief Read back the stored rule and whether daylight time is in effect.
 *
 * @param rule Out: the stored rule (start.month == 0 if adjustment is off).
 *
 * @return true while the clock is running on daylight time.
 */
bool dst_get(dst_rule_t *rule);

/**
 * @brief Move the clock if a changeover is due, then arm for the next one.
 *
 * Does nothing (and disarms Alarm B) while the clock is unset or no rule is
 * stored.
 *
 * @return Which way the clock was moved, if at all.
 */
dst_step_t dst_service(void);

#endif
