/*******************************************************************************
 * @file alarm_rt.c
 * @brief Alarm runtime: arms RTC Alarm A and drives the ring (LED ramp).
 *******************************************************************************
 * @note:
 * Ties the pure scheduling logic in alarm.h to the hardware. It keeps RTC
 * Alarm A programmed for the soonest upcoming alarm, when the alarm fires its
 * interrupt callback flags alarm_rt_notify_fired(), and the scheduler task
 * alarm_rt_task() picks a matching alarm, ramps LED 0 up to its brightness over
 * its ramp time, holds until the timeout (or a long-press cancel), then re-arms
 * the next alarm.
 *
 * Sound playback is not yet wired (the sound asset region is a later step),
 * alarm_rt_task() marks where sound_id would start/stop. Low-power sleep is
 * likewise out of scope here: the runtime assumes the CPU stays awake.
 *******************************************************************************
 */

/** Includes. *****************************************************************/

#include "alarm_rt.h"
#include "alarm.h"
#include "light.h"
#include "manifest.h"
#include "rtc.h"
#include "sound.h"

/** Private variables. ********************************************************/

// Set by the RTC Alarm A ISR callback, consumed by alarm_rt_task().
static volatile bool s_fired = false;

// Ring state. The LED look is owned by the light module; here we only track the
// auto-quiet timeout (ms, seconds scale up past 16 bits) against HAL_GetTick().
static bool s_ringing = false;
static uint32_t s_ring_start_ms = 0u;
static uint32_t s_ring_timeout_ms = 0u; // 0 = no auto-quiet (cancel only).

/** Private functions. ********************************************************/

/**
 * @brief Read the current RTC date/time into alarm_time_t field form.
 */
static void read_now(alarm_time_t *now) {
  get_date_time_fields(&now->year, &now->month, &now->date, &now->weekday,
                       &now->hour, &now->minute, &now->second);
}

/**
 * @brief End the current ring: stop sound, return the LED to the lamp idle.
 */
static void stop_ring(void) {
  s_ringing = false;
  sound_stop();         // No-op if this ring had no sound; also mutes the amp.
  light_lamp_reapply(); // Back to the current lamp on/off idle look.
}

/**
 * @brief Begin ringing for whichever enabled alarm matches now, then re-arm.
 */
static void start_ring(void) {
  alarm_time_t now;
  read_now(&now);

  const manifest_t *m = manifest_get();
  for (uint16_t i = 0u; i < m->header.alarm_count; i++) {
    const alarm_record_t *a = &m->alarms[i];
    if (!alarm_record_matches(a, &now)) {
      continue;
    }
    s_ring_timeout_ms = (uint32_t)a->timeout_s * 1000u;
    s_ring_start_ms = HAL_GetTick();
    s_ringing = true;

    // Play the alarm's light look (LED-only ring if the id is unprogrammed).
    if (a->light_id < m->header.light_count) {
      light_play(&m->lights[a->light_id]);
    }

    // Play the alarm's sound if one is stored (sound_start owns the amp),
    // fading its volume in over sound_fade_s; otherwise the ring is LED-only.
    sound_start(a->sound_id, (uint32_t)a->sound_fade_s * 1000u);
    break;
  }

  // The just-fired alarm's next occurrence rolls forward, so re-arm regardless
  // of whether a match was found (e.g. a stale interrupt).
  alarm_rt_rearm();
}

/**
 * @brief Advance the ring: honour the auto-quiet timeout.
 */
static void update_ring(void) {
  const uint32_t elapsed = HAL_GetTick() - s_ring_start_ms;
  if (s_ring_timeout_ms != 0u && elapsed >= s_ring_timeout_ms) {
    stop_ring();
  }
}

/** Public functions. *********************************************************/

void alarm_rt_init(void) {
  s_fired = false;
  s_ringing = false;
  alarm_rt_rearm();
}

void alarm_rt_rearm(void) {
  alarm_time_t now;
  read_now(&now);

  alarm_next_t next;
  if (!alarm_next(manifest_get(), &now, &next)) {
    HAL_RTC_DeactivateAlarm(&hrtc, RTC_ALARM_A);
    return;
  }

  RTC_AlarmTypeDef alarm = {0};
  alarm.AlarmTime.Hours = next.hour;
  alarm.AlarmTime.Minutes = next.minute;
  alarm.AlarmTime.Seconds = next.second;
  alarm.AlarmTime.SubSeconds = 0u;
  alarm.AlarmTime.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
  alarm.AlarmTime.StoreOperation = RTC_STOREOPERATION_RESET;
  alarm.AlarmMask = RTC_ALARMMASK_NONE; // Match hours, minutes, seconds + day.
  alarm.AlarmSubSecondMask = RTC_ALARMSUBSECONDMASK_ALL;
  alarm.AlarmDateWeekDaySel = next.monthly ? RTC_ALARMDATEWEEKDAYSEL_DATE
                                           : RTC_ALARMDATEWEEKDAYSEL_WEEKDAY;
  alarm.AlarmDateWeekDay = next.day; // Date 1..31, or weekday Mon=1..Sun=7.
  alarm.Alarm = RTC_ALARM_A;

  HAL_RTC_SetAlarm_IT(&hrtc, &alarm, RTC_FORMAT_BIN);
}

void alarm_rt_task(void) {
  if (s_fired) {
    s_fired = false;
    start_ring();
  }
  if (s_ringing) {
    update_ring();
  }
}

void alarm_rt_notify_fired(void) { s_fired = true; }

bool alarm_rt_is_ringing(void) { return s_ringing; }

void alarm_rt_cancel(void) {
  if (s_ringing) {
    stop_ring();
  }
}
