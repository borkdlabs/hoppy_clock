/*******************************************************************************
 * @file run.c
 * @brief Centralized main loop run logic running in main.c.
 *******************************************************************************
 */

/** Includes. *****************************************************************/

#include "run.h"
#include "power.h"

/** Public functions. *********************************************************/

void hoppy_clock_run(void) {
  scheduler_run(); // Run every due task.
  power_idle();    // Then sleep the CPU until the next interrupt.
}
