/*******************************************************************************
 * @file scheduler.h
 * @brief Scheduler: Manages real time scheduling via 32-bit timer peripheral.
 *******************************************************************************
 */

#ifndef HOPPY_CLOCK__SCHEDULER_H
#define HOPPY_CLOCK__SCHEDULER_H

/** Includes. *****************************************************************/

#include "stm32l4xx_hal.h"

/** STM32 port and pin configs. ***********************************************/

extern TIM_HandleTypeDef htim2;

// Timer for signals.
#define SCHEDULER_HTIM htim2

// Timer address.
#define SCHEDULER_TIM TIM2

/** Definitions. **************************************************************/

#define MAX_TASKS 10

// Maximum schedulable period. The time base is a 32-bit microsecond timer that
// wraps every 2^32 us, and is_due_u32() compares times with signed 32-bit
// subtraction. That comparison is unambiguous only while the distance between
// now and due stays strictly below half the range (< 2^31 us).
#define SCHEDULER_MAX_PERIOD_US (2147483647u)
#define SCHEDULER_MAX_PERIOD_MS (SCHEDULER_MAX_PERIOD_US / 1000u)

/** Public types. *************************************************************/

/**
 * @brief Defined type for task functions.
 */
typedef void (*task_function_t)(void);

/**
 * @brief Structure to hold task information.
 */
typedef struct {
  task_function_t task_function; // Pointer to the task function.
  uint32_t period_us;            // Task execution period in us.
  uint32_t next_execution_us;    // Next execution due in us.
} task_t;

/** Public functions. *********************************************************/

/**
 * @brief Initialize scheduler.
 */
void scheduler_init(void);

/**
 * Function to add tasks to the scheduler.
 *
 * @param task_function task_function_t to add as a task.
 * @param period_ms Task execution period in milliseconds. Must be <=
 * SCHEDULER_MAX_PERIOD_MS.
 */
void scheduler_add_task(task_function_t task_function, uint32_t period_ms);

/**
 * @brief Scheduler run function to be called in the main loop.
 */
void scheduler_run(void);

#endif
