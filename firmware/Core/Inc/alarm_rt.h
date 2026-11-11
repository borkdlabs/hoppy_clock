/*******************************************************************************
 * @file alarm_rt.h
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

#ifndef HOPPY_CLOCK__ALARM_RT_H
#define HOPPY_CLOCK__ALARM_RT_H

/** Includes. *****************************************************************/

#include <stdbool.h>

/** Public functions. *********************************************************/

/**
 * @brief Initialize the runtime and arm the next alarm from the manifest.
 *
 * Call after manifest_load() and the LED/RTC peripherals are up.
 */
void alarm_rt_init(void);

/**
 * @brief Scheduler task: service a fired alarm and drive the ring ramp.
 *
 * Call periodically (a 10-50 ms cadence is fine, it governs ramp smoothness).
 */
void alarm_rt_task(void);

/**
 * @brief Recompute the soonest alarm and reprogram RTC Alarm A.
 *
 * Call whenever the inputs change: the wall-clock time (SET_TIME) or the alarm
 * table (config commit). If no alarm is scheduled, Alarm A is deactivated.
 */
void alarm_rt_rearm(void);

/**
 * @brief Flag that an RTC alarm has fired (called from the ISR callbacks).
 *
 * Alarm A is the next alarm, Alarm B the next daylight saving changeover. Both
 * are serviced by the same pass of alarm_rt_task().
 */
void alarm_rt_notify_fired(void);

/**
 * @brief Whether Alarm A has fired but alarm_rt_task() has not serviced it yet.
 *
 * The power manager checks this with interrupts masked before deep sleep, so a
 * fire that lands between the task's last pass and the sleep is not slept on.
 *
 * @return true while a fire is waiting for alarm_rt_task().
 */
bool alarm_rt_is_pending(void);

/**
 * @brief Whether an alarm is currently ringing.
 *
 * @return true while ringing (LED ramp active), false otherwise.
 */
bool alarm_rt_is_ringing(void);

/**
 * @brief Stop the current ring immediately (e.g. long-press). No-op if idle.
 */
void alarm_rt_cancel(void);

#endif
