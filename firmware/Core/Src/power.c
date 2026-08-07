/*******************************************************************************
 * @file power.c
 * @brief Low-power management for the cooperative main loop.
 *******************************************************************************
 */

/** Includes. *****************************************************************/

#include "power.h"
#include "alarm_rt.h"
#include "button_hal_gpio.h"
#include "light.h"
#include "sound.h"
#include "stm32l4xx_hal.h"
#include "usb_device.h"

/** Definitions. **************************************************************/

// After waking from STOP2 the USB pull-up is re-asserted, but a host needs time
// to enumerate before dev_state reaches CONFIGURED. Stay awake until this tick
// so we don't deep-sleep (and detach the pull-up) mid-enumeration. Armed on
// each wake, a real host reaches CONFIGURED well within the window and then
// holds us awake on its own.
#define POWER_WAKE_HOLD_MS 3000u

/** STM32 external references. ************************************************/

// Re-run on wake from STOP2 (all PLLs are lost, clock falls back to MSI).
// SystemClock_Config re-locks the main PLL (SYSCLK), PeriphCommonClock_Config
// re-locks PLLSAI1, which feeds the USB and ADC clocks.
extern void SystemClock_Config(void);
extern void PeriphCommonClock_Config(void);
extern USBD_HandleTypeDef hUsbDeviceFS;
extern PCD_HandleTypeDef hpcd_USB_FS;

/** Private variables. ********************************************************/

static uint32_t s_awake_hold_until = 0u;

/** Private functions. ********************************************************/

/**
 * @brief Whether it is safe to enter STOP2 (nothing needs the clocks running).
 *
 * False while an alarm rings, a sound plays or is being written, the button is
 * held, the LED is animating or showing the clock-unset warning, or a USB host
 * session is configured (STOP2 suspends USB and would drop an upload/config).
 */
static bool deep_sleep_ok(void) {
  if (alarm_rt_is_ringing() || sound_is_playing() || sound_is_writing()) {
    return false;
  }
  if (button_is_down() || !light_is_idle()) {
    return false;
  }
  // Post-wake grace: stay awake (WFI) so a host that reconnected while we slept
  // can finish enumerating. Signed compare tolerates the tick wrap.
  if ((int32_t)(HAL_GetTick() - s_awake_hold_until) < 0) {
    return false;
  }
  // A live USB host session stays awake; battery/unplugged/suspended do not
  // reach CONFIGURED, so they still deep-sleep.
  if (hUsbDeviceFS.dev_state == USBD_STATE_CONFIGURED) {
    return false;
  }
  return true;
}

/** Public functions. *********************************************************/

void power_idle(void) {
  if (!deep_sleep_ok()) {
    // Light sleep: gate the core until the next interrupt, but keep clocks and
    // peripherals live so USB / audio / LED / button stay fully responsive.
    __WFI();
    return;
  }

  // Drop the USB D+ pull-up before sleeping. STOP2 leaves the pull-up asserted
  // but the peripheral unclocked, so a host that gets plugged in while we sleep
  // sees a device, tries to enumerate, gets no response, and reports an
  // "unrecognized USB device" (plug-in is not a wake source, only the button or
  // an alarm wakes us). Presenting as detached avoids that failed enumeration.
  // DevConnect on wake re-asserts the pull-up so the host enumerates cleanly.
  HAL_PCD_DevDisconnect(&hpcd_USB_FS);

  // Deep sleep (STOP2): clocks off, SRAM retained (~uA). Wakes on the RTC alarm
  // (the next armed alarm) or a button press (EXTI4). SysTick is off during
  // STOP2 and all PLLs are lost, so re-lock the 80 MHz system clock (SYSCLK)
  // and PLLSAI1 (USB + ADC), then resume the HAL tick on wake before returning
  // to the loop. Restoring PLLSAI1 lets USB enumerate after a button/alarm
  // wake.
  HAL_SuspendTick();
  HAL_PWREx_EnterSTOP2Mode(PWR_STOPENTRY_WFI);
  SystemClock_Config();
  PeriphCommonClock_Config();
  HAL_ResumeTick();

  // Back awake with the USB clock restored: re-assert the pull-up so a host
  // (plugged in while system slept, or still attached) can enumerate now, and
  // hold off the next deep sleep long enough for that enumeration to complete.
  HAL_PCD_DevConnect(&hpcd_USB_FS);
  s_awake_hold_until = HAL_GetTick() + POWER_WAKE_HOLD_MS;
}
