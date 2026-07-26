/*******************************************************************************
 * @file scheduler.c
 * @brief Scheduler: Manages real time scheduling via 32-bit timer peripheral.
 ******************************************************************************/

/** Includes. *****************************************************************/

#include "scheduler.h"
#include "stm32l4xx.h"

/** Private variables. ********************************************************/

static task_t tasks[MAX_TASKS];
static uint8_t num_tasks = 0;

/** Private functions. ********************************************************/

/**
 * @brief Atomic get current timer peripheral value.
 *
 * @return Current time.
 */
static uint32_t time_us(void) {
  return SCHEDULER_TIM->CNT; // Atomic 32-bit timer.
}

/**
 * @brief Wrap-safe "now >= due" using signed subtraction on uint32_t.
 *
 * @param now Current tick/tick.
 * @param due Execution due time/tick.
 *
 * @return Wrap-safe due time as int32_t.
 */
static int is_due_u32(uint32_t const now, uint32_t const due) {
  return (int32_t)(now - due) >= 0;
}

/** Public functions. *********************************************************/

void scheduler_init(void) {
  // TODO: DEV NOTE: Timer initialization completed by scheduler (TIM owner).
  // __HAL_TIM_ENABLE(&SCHEDULER_HTIM);
  // HAL_TIM_Base_Init(&SCHEDULER_HTIM);
  HAL_TIM_Base_Start(&SCHEDULER_HTIM);

  num_tasks = 0;
}

void scheduler_add_task(const task_function_t task_function,
                        const uint32_t period_ms) {
  uint32_t primask = __get_PRIMASK();
  __disable_irq();

  // Add task if allowed.
  if (num_tasks < MAX_TASKS) {
    tasks[num_tasks].task_function = task_function;
    tasks[num_tasks].period_us = period_ms * 1000u; // ms to us.
    uint32_t const now = time_us();
    tasks[num_tasks].next_execution_us = now + tasks[num_tasks].period_us;
    num_tasks++;
  } else {
    // TODO: Handle error (log, assert, etc).
  }

  // If scheduler_add_task() is called from contexts where IRQs are already
  // disabled, restore the prior state.
  if (!primask) {
    __enable_irq();
  }
}

void scheduler_run(void) {
  uint32_t const now = time_us();

  for (uint8_t i = 0; i < num_tasks; i++) {
    if (is_due_u32(now, tasks[i].next_execution_us)) {
      tasks[i].task_function();

      // Catch-up: advance next_execution_us so it is strictly > now.
      // This preserves phase and skips any missed slots.
      uint32_t const per = tasks[i].period_us;
      uint32_t due = tasks[i].next_execution_us;

      do {
        due += per;
      } while (is_due_u32(now, due));
      tasks[i].next_execution_us = due;
    }
  }
}
