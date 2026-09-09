/*******************************************************************************
 * @file light.h
 * @brief LED 0 owner: renders light_seq_t transitions and the lamp idle state.
 *******************************************************************************
 * @note:
 * Single owner of the WS2812B LED. Everything that lights the LED (the lamp
 * button, alarm rings) funnels through here so nothing fights over it.
 *
 * light_play() snapshots what the strip currently shows and starts the new
 * look. light_task() renders that look's frame, applies its flicker, then
 * crossfades from the snapshot into it over fade_ms shaped by curve. The fade
 * is the incoming look's own setting and behaves the same for every effect, so
 * one knob covers both directions: fading a look in is what fades the previous
 * one out. A SOLID look settles once faded in and holds as the idle.
 *
 * The lamp is a two-state idle toggled by the button: light_lamp_toggle() plays
 * the manifest's lamp-on / lamp-off look (falling back to built-ins if none is
 * programmed). After an alarm ring the runtime calls light_lamp_reapply() to
 * return the LED to the current lamp idle.
 *******************************************************************************
 */

#ifndef HOPPY_CLOCK__LIGHT_H
#define HOPPY_CLOCK__LIGHT_H

/** Includes. *****************************************************************/

#include "manifest.h"
#include <stdbool.h>

/** Public functions. *********************************************************/

/**
 * @brief Initialize the LED to off and clear any transition. Call after
 * ws2812b_init().
 */
void light_init(void);

/**
 * @brief Scheduler task: advance the active transition and refresh the LED.
 *
 * Call periodically (a 20 ms cadence gives smooth ramps and flicker).
 */
void light_task(void);

/**
 * @brief Crossfade the strip from what it shows now into a sequence's look.
 *
 * @param seq Light look to play (copied; caller need not keep it).
 */
void light_play(const light_seq_t *seq);

/**
 * @brief Toggle the lamp and play the corresponding on/off idle look.
 */
void light_lamp_toggle(void);

/**
 * @brief Re-play the current lamp idle look (e.g. after an alarm ring ends).
 */
void light_lamp_reapply(void);

/**
 * @brief Current lamp state.
 *
 * @return true if the lamp is toggled on, false if off.
 */
bool light_lamp_is_on(void);

/**
 * @brief Whether the strip is idle (settled, no animation or warning blink).
 *
 * Only a fully faded-in SOLID look with no flicker settles; looping effects and
 * flickering looks animate forever and so never report idle.
 *
 * @return true if nothing needs rendering, so the CPU may deep-sleep.
 */
bool light_is_idle(void);

/**
 * @brief Enable/disable the "clock not set" warning blink on LED 0.
 *
 * While active, LED 0 blinks dim red regardless of the current look (it
 * overrides the lamp/alarm colour on that one pixel). Clearing it restores the
 * current lamp idle. Idempotent; drive it from the RTC-unset check.
 *
 * @param active true to show the warning blink, false to clear it.
 */
void light_set_warning(bool active);

#endif
