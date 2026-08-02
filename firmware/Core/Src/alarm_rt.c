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
#include "manifest.h"
#include "pam8302a_hal_dac.h"
#include "rtc.h"
#include "sound.h"
#include "ws2812b_hal_pwm.h"

/** Private variables. ********************************************************/

// Set by the RTC Alarm A ISR callback, consumed by alarm_rt_task().
static volatile bool s_fired = false;

// Ring state. Durations are held in milliseconds (ramp/timeout seconds scale up
// past 16 bits) and timed against HAL_GetTick().
static bool s_ringing = false;
static uint32_t s_ring_start_ms = 0u;
static uint32_t s_ring_ramp_ms = 0u;    // 0 = jump straight to full brightness.
static uint32_t s_ring_timeout_ms = 0u; // 0 = no auto-quiet (cancel only).
static uint8_t s_ring_brightness = 0u;  // Target LED level 0..255.
static uint8_t s_last_level = 0u;       // Last level pushed to the LED.
static bool s_ring_has_sound = false;   // Amp was enabled for this ring.

/** Private functions. ********************************************************/

/**
 * @brief Read the current RTC date/time into alarm_time_t field form.
 */
static void read_now(alarm_time_t *now) {
  get_date_time_fields(&now->year, &now->month, &now->date, &now->weekday,
                       &now->hour, &now->minute, &now->second);
}

/**
 * @brief Drive LED 0 to a white level (only touches hardware on change).
 */
static void led_set_level(uint8_t level) {
  if (level == s_last_level) {
    return;
  }
  s_last_level = level;
  ws2812b_set_colour(0u, level, level, level);
  ws2812b_update();
}

/**
 * @brief End the current ring: LED off, mark idle.
 */
static void stop_ring(void) {
  s_ringing = false;
  led_set_level(0u);
  if (s_ring_has_sound) {
    sound_stop();
    amp_disable();
    s_ring_has_sound = false;
  }
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
    s_ring_brightness = a->brightness_max;
    s_ring_ramp_ms = (uint32_t)a->ramp_time_s * 1000u;
    s_ring_timeout_ms = (uint32_t)a->timeout_s * 1000u;
    s_ring_start_ms = HAL_GetTick();
    s_last_level = 0u;
    s_ringing = true;

    // Play the alarm's sound if one is stored, otherwise ring LED-only.
    sound_entry_t se;
    if (sound_get_info(a->sound_id, &se)) {
      amp_enable();
      s_ring_has_sound = sound_start(a->sound_id);
      if (!s_ring_has_sound) {
        amp_disable(); // Stream failed to start, undo the enable.
      }
    }
    break;
  }

  // The just-fired alarm's next occurrence rolls forward, so re-arm regardless
  // of whether a match was found (e.g. a stale interrupt).
  alarm_rt_rearm();
}

/**
 * @brief Advance the ring: update the brightness ramp, honour the timeout.
 */
static void update_ring(void) {
  const uint32_t elapsed = HAL_GetTick() - s_ring_start_ms;

  if (s_ring_timeout_ms != 0u && elapsed >= s_ring_timeout_ms) {
    stop_ring();
    return;
  }

  uint8_t level;
  if (s_ring_ramp_ms == 0u || elapsed >= s_ring_ramp_ms) {
    level = s_ring_brightness;
  } else {
    level = (uint8_t)((uint32_t)s_ring_brightness * elapsed / s_ring_ramp_ms);
  }
  led_set_level(level);
}

/** Public functions. *********************************************************/

void alarm_rt_init(void) {
  s_fired = false;
  s_ringing = false;
  s_last_level = 0u;
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
