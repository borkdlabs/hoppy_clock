/*******************************************************************************
 * @file button_hal_gpio.h
 * @brief User button functions: abstracting STM32 HAL: GPIO.
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

#ifndef HOPPY_CLOCK__BUTTON_HAL_GPIO_H
#define HOPPY_CLOCK__BUTTON_HAL_GPIO_H

/** Includes. *****************************************************************/

#include "stm32l4xx_hal.h"
#include <stdbool.h>

/** STM32 port and pin configs. ***********************************************/

// User button: PB4, active-low (external pull-up), pressed reads GPIO_PIN_RESET.
#define BUTTON_GPIO_PORT GPIOB
#define BUTTON_GPIO_PIN GPIO_PIN_4
#define BUTTON_ACTIVE_STATE GPIO_PIN_RESET

/** Definitions. **************************************************************/

// Level must hold steady this long (ms) before a change is accepted.
#define BUTTON_DEBOUNCE_MS 20u

// Hold threshold (ms) separating a short press from a long press.
#define BUTTON_LONG_PRESS_MS 1500u

/** Public types. *************************************************************/

/**
 * @brief Discrete button event, returned by button_get_event().
 */
typedef enum {
  BUTTON_EVENT_NONE = 0, // No new event since the last call.
  BUTTON_EVENT_SHORT,    // Released before the long-press threshold.
  BUTTON_EVENT_LONG,     // Held past BUTTON_LONG_PRESS_MS (fires once).
} button_event_t;

/** Public functions. *********************************************************/

/**
 * @brief Initialize the button tracker.
 *
 * Seeds the debounce state from the current pin level so no spurious event is
 * produced at boot.
 */
void button_init(void);

/**
 * @brief Sample and debounce the button; latch any resulting event.
 *
 * Call frequently from the main loop. Non-blocking, uses HAL_GetTick() for
 * timing so the exact call rate does not matter (poll at least every few ms).
 */
void button_update(void);

/**
 * @brief Retrieve and clear the pending button event.
 *
 * @return The latched event, or BUTTON_EVENT_NONE if none. Cleared on read.
 */
button_event_t button_get_event(void);

/**
 * @brief Current debounced pressed state.
 *
 * @return true while the button is held down (debounced), false otherwise.
 */
bool button_is_down(void);

#endif
