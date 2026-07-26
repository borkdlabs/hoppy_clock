/*******************************************************************************
 * @file button_hal_gpio.c
 * @brief User button (PB4): debounce + short/long press detection.
 *******************************************************************************
 * @note:
 * Poll-based: call button_update() frequently from the scheduler loop (uses
 * timestamps with HAL_GetTick(), so an exact call rate is not required).
 * Retrieve results with button_get_event(). Edge interrupt is left free as a
 * Stop-mode wake source and is not used here.
 *
 * A press shorter than BUTTON_LONG_PRESS_MS yields BUTTON_EVENT_SHORT on
 * release; holding past that threshold yields BUTTON_EVENT_LONG once, while
 * still held (no short is then emitted on release).
 *******************************************************************************
 */

/** Includes. *****************************************************************/

#include "button_hal_gpio.h"

/** Private variables. ********************************************************/

static bool s_raw_last;          // Last raw sample (true == pressed).
static uint32_t s_raw_change_ms; // Tick when the raw level last changed.
static bool s_debounced;         // Debounced pressed state.
static uint32_t s_press_ms;      // Tick when the debounced press began.
static bool s_long_fired;        // Long event already emitted this press.
static button_event_t s_event;   // Latch, cleared with button_get_event().

/** Private functions. ********************************************************/

static bool button_raw_pressed(void) {
  return HAL_GPIO_ReadPin(BUTTON_GPIO_PORT, BUTTON_GPIO_PIN) ==
         BUTTON_ACTIVE_STATE;
}

/** Public functions. *********************************************************/

void button_init(void) {
  const bool pressed = button_raw_pressed();
  s_raw_last = pressed;
  s_debounced = pressed;
  s_raw_change_ms = HAL_GetTick();
  s_press_ms = HAL_GetTick();
  // If held at boot, suppress the long event until the button is released and
  // pressed again.
  s_long_fired = pressed;
  s_event = BUTTON_EVENT_NONE;
}

void button_update(void) {
  const bool raw = button_raw_pressed();
  const uint32_t now = HAL_GetTick();

  if (raw != s_raw_last) {
    // Level moved; restart the debounce window.
    s_raw_last = raw;
    s_raw_change_ms = now;
  } else if (raw != s_debounced &&
             (now - s_raw_change_ms) >= BUTTON_DEBOUNCE_MS) {
    // Level held steady long enough: accept the new debounced state.
    s_debounced = raw;
    if (s_debounced) {
      // Press edge.
      s_press_ms = now;
      s_long_fired = false;
    } else if (!s_long_fired) {
      // Release edge before the long threshold -> short press.
      s_event = BUTTON_EVENT_SHORT;
    }
  }

  // While held, emit the long event once the threshold is crossed.
  if (s_debounced && !s_long_fired &&
      (now - s_press_ms) >= BUTTON_LONG_PRESS_MS) {
    s_long_fired = true;
    s_event = BUTTON_EVENT_LONG;
  }
}

button_event_t button_get_event(void) {
  const button_event_t event = s_event;
  s_event = BUTTON_EVENT_NONE;
  return event;
}

bool button_is_down(void) { return s_debounced; }
