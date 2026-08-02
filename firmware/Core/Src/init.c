/*******************************************************************************
 * @file init.c
 * @brief Centralized init logic running in main.c.
 *******************************************************************************
 */

/** Includes. *****************************************************************/

#include "init.h"
#include "alarm_rt.h"
#include "button_hal_gpio.h"
#include "light.h"
#include "manifest.h"
#include "mcu_temp_hal_adc.h"
#include "pam8302a_hal_dac.h"
#include "scheduler.h"
#include "sound.h"
#include "usb_cmd.h"
#include "w25q128jv_hal_qspi.h"
#include "ws2812b_hal_pwm.h"

/** Definitions. **************************************************************/

// Short alarm beep: an integer number of sine cycles so it starts and ends at
// mid-scale (click-free one-shot). 47 * 32 = 1504 samples ~= 47 ms at 32 kHz.
#define BEEP_CYCLE_LEN 32u
#define BEEP_CYCLES 47u
#define BEEP_SAMPLES (BEEP_CYCLES * BEEP_CYCLE_LEN)
#define BEEP_RATE_HZ (BEEP_CYCLE_LEN * 1000u) // 1 kHz tone.

/** STM32 port and pin configs. ***********************************************/

/** Private types. ************************************************************/

typedef enum {
  STATE_SLEEP = 0,
  STATE_IDLE,
  STATE_WRITE_NVM,
  STATE_LAMP_ON,
  STATE_SOUND_ON,
  STATE_FAULT,
} hoppy_clock_state_machine_t;

/** Private variables. ********************************************************/

static hoppy_clock_state_machine_t s_state = STATE_IDLE;

// One cycle of a sine, 12-bit codes centred on mid-scale (2048 +/- ~1800).
static const uint16_t s_sine_cycle[BEEP_CYCLE_LEN] = {
    2048, 2399, 2737, 3048, 3321, 3545, 3711, 3813, 3848, 3813, 3711,
    3545, 3321, 3048, 2737, 2399, 2048, 1697, 1359, 1048, 775,  551,
    385,  283,  248,  283,  385,  551,  775,  1048, 1359, 1697};

// Alarm beep buffer, filled at init by tiling s_sine_cycle.
static uint16_t s_beep[BEEP_SAMPLES];

/** Private functions. ********************************************************/

static void state_machine(void) {
  // Poll the button here (10 ms task = fine debounce cadence).
  button_update();

  switch (button_get_event()) {
  case BUTTON_EVENT_LONG:
    // A long press shuts off a ringing alarm; otherwise it is the amp beep
    // bring-up test.
    if (alarm_rt_is_ringing()) {
      alarm_rt_cancel();
      s_state = STATE_IDLE;
      break;
    }
    amp_enable();
    amp_set_sample_rate(BEEP_RATE_HZ);
    amp_play(s_beep, BEEP_SAMPLES, false); // One-shot, auto-returns to idle.
    s_state = STATE_SOUND_ON;
    break;

  case BUTTON_EVENT_SHORT:
    // Toggle the lamp idle look; ignored while an alarm owns the LED.
    if (!alarm_rt_is_ringing()) {
      light_lamp_toggle();
    }
    break;

  default:
    break;
  }
}

/** Public functions. *********************************************************/

void hoppy_clock_init(void) {
  // MCU temperature.
  mcu_temp_init();
  mcu_temp_start();

  // Addressable LEDs.
  ws2812b_init();

  // LED startup R, G, B flash test sequence.
  ws2812b_set_colour(0, 1, 0, 0);
  ws2812b_update();
  HAL_Delay(500);
  ws2812b_set_colour(0, 0, 1, 0);
  ws2812b_update();
  HAL_Delay(500);
  ws2812b_set_colour(0, 0, 0, 1);
  ws2812b_update();
  HAL_Delay(500);
  ws2812b_set_colour(0, 0, 0, 0);
  ws2812b_update();

  // Speaker amp.
  amp_init();

  // Button.
  button_init();

  // External NOR flash, then load the persisted settings. A probe failure or a
  // blank/corrupt flash leaves manifest_load() returning empty defaults, so the
  // system still boots (just with no alarms).
  w25q_init();
  manifest_load();
  sound_init();

  // Apply the configured LED-chain length, then bring up the LED owner and
  // settle on the lamp-off idle look (a programmed ambient, if any).
  ws2812b_set_count(manifest_get()->header.led_count);
  light_init();
  light_lamp_reapply();

  // USB CDC command layer. USB itself is brought up by MX_USB_DEVICE_Init() in
  // main() before this runs.
  usb_cmd_init();

  // Alarm runtime: arm RTC Alarm A for the soonest alarm in the manifest.
  alarm_rt_init();

  // Build the alarm beep by tiling one sine cycle (starts/ends at mid-scale).
  for (uint32_t i = 0; i < BEEP_SAMPLES; i++) {
    s_beep[i] = s_sine_cycle[i % BEEP_CYCLE_LEN];
  }

  // Scheduler.
  scheduler_init(); // Initialize scheduler.
  scheduler_add_task(state_machine, 10);
  scheduler_add_task(usb_cmd_task, 10);
  scheduler_add_task(alarm_rt_task, 20);
  scheduler_add_task(sound_task, 10);
  scheduler_add_task(light_task, 20);
}
