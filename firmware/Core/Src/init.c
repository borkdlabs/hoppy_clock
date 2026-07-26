/*******************************************************************************
 * @file init.c
 * @brief Centralized init logic running in main.c.
 *******************************************************************************
 */

/** Includes. *****************************************************************/

#include "init.h"
#include "button_hal_gpio.h"
#include "mcu_temp_hal_adc.h"
#include "pam8302a_hal_dac.h"
#include "scheduler.h"
#include "ws2812b_hal_pwm.h"

/** STM32 port and pin configs. ***********************************************/

/** Private types. ************************************************************/

/** Private functions. ********************************************************/

static void state_machine(void) {
  // TODO
}

/** Public functions. *********************************************************/

void hoppy_clock_init(void) {
  // MCU temperature.
  mcu_temp_init();
  mcu_temp_start();

  // Addressable LEDs.
  ws2812b_init();

  // Speaker amp.
  amp_init();

  // Button.
  button_init();

  // Scheduler.
  scheduler_init(); // Initialize scheduler.
  scheduler_add_task(state_machine, 10);
}
