/*******************************************************************************
 * @file power.h
 * @brief Low-power management for the cooperative main loop.
 *******************************************************************************
 * @note:
 * power_idle() is called once per super-loop pass, after the scheduler has run
 * every due task, and picks the deepest safe sleep:
 *
 *  - Busy (alarm ringing, sound, LED animating/warning, button held, or a live
 *    USB host session): ARM Sleep via __WFI() -- gate the core but keep clocks
 *    and peripherals running so everything stays responsive.
 *  - Fully idle: STOP2 -- clocks off, SRAM retained (~uA), the deep battery-mode
 *    sleep. Wakes on the RTC alarm (next alarm, EXTI line 18) or a button press
 *    (PB4 EXTI4), then re-locks the 80 MHz clock (SystemClock_Config).
 *
 * USB is only suspended in STOP2 while unplugged/idle (not CONFIGURED), so an
 * active upload/config is never cut off. Plugging a host into a deep-sleeping
 * unit enumerates after the next wake (a button press or alarm).
 *******************************************************************************
 */

#ifndef HOPPY_CLOCK__POWER_H
#define HOPPY_CLOCK__POWER_H

/** Public functions. *********************************************************/

/**
 * @brief Sleep the CPU until the next interrupt (call when the loop is idle).
 *
 * Enters ARM Sleep mode via WFI. Returns as soon as any enabled interrupt is
 * pending (or fires), so no scheduled work is missed beyond a sub-tick delay.
 */
void power_idle(void);

#endif
