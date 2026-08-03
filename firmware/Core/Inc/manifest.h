/*******************************************************************************
 * @file manifest.h
 * @brief Persisted clock settings (alarms + light looks) in W25Q NOR flash.
 *******************************************************************************
 * @note:
 * Firmware is fixed; the user-editable settings live in NOR flash and are
 * written over USB at runtime.
 *
 * The image is a fixed-size struct in one 4 KB sector, kept in two slots (A/B)
 * for power-safe updates. A save writes the idle slot, bumps a monotonic
 * sequence number, and writes the CRC last; on boot the valid slot with the
 * higher sequence number wins. A blank or corrupt pair yields empty defaults.
 *
 *   Header (22 B): magic | version | alarm_count | light_count |
 *                  lamp_on_light | lamp_off_light | led_count | reserved |
 *                  seq_no | crc32
 *   Alarms[MANIFEST_MAX_ALARMS] x 12 B
 *   Lights[MANIFEST_MAX_LIGHTS] x 12 B
 *
 * A light_seq_t is a strip-aware parametric "look": a procedural effect (solid
 * fade, rainbow, sweep, breathe) rendered across all LEDs. Alarms and the two
 * lamp idle states reference one by id. crc32 is the reflected CRC-32 (poly
 * 0xEDB88320, init/final 0xFFFFFFFF, == zlib.crc32) over the image except
 * crc32.
 *******************************************************************************
 */

#ifndef HOPPY_CLOCK__MANIFEST_H
#define HOPPY_CLOCK__MANIFEST_H

/** Includes. *****************************************************************/

#include "stm32l4xx_hal.h"
#include <stdbool.h>
#include <stdint.h>

/** Definitions. **************************************************************/

#define MANIFEST_MAGIC 0x50504F48u // "HOPP" (little-endian on the wire).
#define MANIFEST_VERSION 4u
#define MANIFEST_MAX_ALARMS 64u
#define MANIFEST_MAX_LIGHTS 16u

// alarm_record_t.flags bits.
#define ALARM_FLAG_ENABLED (1u << 0) // Alarm is active.
#define ALARM_FLAG_MONTHLY (1u << 1) // 0 = weekly, 1 = monthly (see day_sel).

// Weekly-mode day_sel bit positions (weekday mask). Monthly mode instead uses
// day_sel as a day-of-month value, 1..31.
#define ALARM_DAY_MON (1u << 0)
#define ALARM_DAY_TUE (1u << 1)
#define ALARM_DAY_WED (1u << 2)
#define ALARM_DAY_THU (1u << 3)
#define ALARM_DAY_FRI (1u << 4)
#define ALARM_DAY_SAT (1u << 5)
#define ALARM_DAY_SUN (1u << 6)

// light_seq_t.effect: how the look animates across the strip and over time.
#define LIGHT_FX_SOLID 0u   // Fade the whole strip to one colour, then hold.
#define LIGHT_FX_RAINBOW 1u // HSV hue cycle; spread = hue step per LED. Loops.
#define LIGHT_FX_SWEEP 2u // A lit band of (r,g,b) moving over darkness. Loops.
#define LIGHT_FX_BREATHE 3u // (r,g,b) brightness breathes in and out. Loops.

// light_seq_t.curve (SOLID only): shape of the fade to the target.
#define LIGHT_CURVE_LINEAR 0u  // Constant-rate ramp.
#define LIGHT_CURVE_EASE 1u    // Smooth ease-in-out ramp.
#define LIGHT_CURVE_FLICKER 2u // Linear rise with a random flicker overlay.

/** Public types. *************************************************************/

/**
 * @brief A strip-aware parametric light "look" (12 bytes, packed).
 *
 * effect selects a procedural animation rendered across every LED:
 *  - SOLID:   fade the whole strip from its current colour to (r,g,b) scaled by
 *             brightness over period_ms using curve, then hold it (so an "off"
 *             look can settle on a dim ambient rather than black).
 *  - RAINBOW: HSV hue cycling with period_ms per turn; spread = hue step per LED
 *             (0 = whole strip one hue, >0 = a rainbow spread along the chain).
 *  - SWEEP:   a lit band of (r,g,b) width spread moving along the strip, one
 *             pass per period_ms, over darkness.
 *  - BREATHE: (r,g,b) whose brightness oscillates in and out every period_ms.
 * Animated effects loop until another look is played; SOLID settles and holds.
 */
typedef struct __attribute__((packed)) {
  uint8_t effect;      // LIGHT_FX_*.
  uint8_t r;           // Base colour red.
  uint8_t g;           // Base colour green.
  uint8_t b;           // Base colour blue.
  uint8_t brightness;  // Master level 0..255, scales the effect. 0 = off.
  uint16_t period_ms;  // SOLID: fade duration. Animated fx: cycle period.
  uint8_t curve;       // SOLID ramp shape: LIGHT_CURVE_*.
  uint8_t spread;      // FLICKER amplitude / RAINBOW hue-step / SWEEP width.
  uint8_t reserved[3]; // Zero; reserved for future use.
} light_seq_t;

/**
 * @brief One alarm entry (12 bytes, packed to a fixed on-flash layout).
 *
 * Fires at hour:minute:second on the days selected by flags/day_sel (weekly
 * mask or monthly day-of-month). On firing it plays light look light_id, plays
 * sound_id (fading its volume in over sound_fade_s), then auto-quiets after
 * timeout_s (0 = until cancelled).
 */
typedef struct __attribute__((packed)) {
  uint8_t flags;        // ALARM_FLAG_* bits.
  uint8_t day_sel;      // Weekly: weekday mask. Monthly: day-of-month 1..31.
  uint8_t hour;         // 0..23.
  uint8_t minute;       // 0..59.
  uint8_t second;       // 0..59.
  uint16_t timeout_s;   // Auto-quiet this long after firing; 0 = manual only.
  uint8_t sound_id;     // Index into the sound region.
  uint8_t light_id;     // Index into lights[].
  uint8_t sound_fade_s; // Sound fade-in duration, seconds. 0 = no fade.
  uint8_t reserved[2];  // Zero; reserved for future use.
} alarm_record_t;

/**
 * @brief On-flash manifest header (20 bytes, packed).
 */
typedef struct __attribute__((packed)) {
  uint32_t magic;         // MANIFEST_MAGIC.
  uint16_t version;       // MANIFEST_VERSION.
  uint16_t alarm_count;   // Valid entries in alarms[], 0..MAX.
  uint16_t light_count;   // Valid entries in lights[], 0..MAX.
  uint8_t lamp_on_light;  // lights[] id played when the lamp toggles on.
  uint8_t lamp_off_light; // lights[] id played when the lamp toggles off.
  uint8_t led_count;      // Active LEDs in the chain (1..LED_COUNT_MAX).
  uint8_t reserved;       // Zero; reserved for future use.
  uint32_t seq_no;        // Monotonic; higher = newer slot.
  uint32_t crc32;         // Over the image excluding this field.
} manifest_header_t;

/**
 * @brief Full manifest image as stored in one flash sector.
 *
 * Unused alarm/light slots (index >= their count) are zero-filled so the image
 * is deterministic and the CRC is stable.
 */
typedef struct __attribute__((packed)) {
  manifest_header_t header;
  alarm_record_t alarms[MANIFEST_MAX_ALARMS];
  light_seq_t lights[MANIFEST_MAX_LIGHTS];
} manifest_t;

/**
 * @brief Outcome of manifest_load().
 */
typedef enum {
  MANIFEST_LOAD_SLOT_A,   // Loaded from slot A.
  MANIFEST_LOAD_SLOT_B,   // Loaded from slot B.
  MANIFEST_LOAD_DEFAULTS, // Neither slot valid; RAM set to empty defaults.
} manifest_load_result_t;

/** Public functions. *********************************************************/

/**
 * @brief Read both slots, pick the newest valid one into the RAM copy.
 *
 * Requires the W25Q driver initialized and in indirect mode (switches out of
 * memory-mapped mode if needed). Blocking (boot). Invalid pair -> defaults.
 *
 * @return Which slot was loaded, or MANIFEST_LOAD_DEFAULTS.
 */
manifest_load_result_t manifest_load(void);

/**
 * @brief Replace the RAM alarm table (does not touch flash).
 *
 * @param alarms Source records.
 * @param count Number of records (must be <= MANIFEST_MAX_ALARMS).
 *
 * @return true if applied, false if count exceeds MANIFEST_MAX_ALARMS.
 */
bool manifest_set_alarms(const alarm_record_t *alarms, uint16_t count);

/**
 * @brief Replace the RAM light table (does not touch flash).
 *
 * @param lights Source sequences.
 * @param count Number of sequences (must be <= MANIFEST_MAX_LIGHTS).
 *
 * @return true if applied, false if count exceeds MANIFEST_MAX_LIGHTS.
 */
bool manifest_set_lights(const light_seq_t *lights, uint16_t count);

/**
 * @brief Set the lamp on/off idle light ids in the RAM copy (not flash).
 *
 * @param on_light lights[] id for the lamp-on idle.
 * @param off_light lights[] id for the lamp-off idle.
 */
void manifest_set_lamp(uint8_t on_light, uint8_t off_light);

/**
 * @brief Set the active LED-chain count in the RAM copy (not flash).
 *
 * @param led_count Number of LEDs in the chain.
 */
void manifest_set_led_count(uint8_t led_count);

/**
 * @brief Persist the current RAM copy to the idle slot (power-safe).
 *
 * @return HAL_OK on success, HAL error status otherwise.
 */
HAL_StatusTypeDef manifest_save(void);

/**
 * @brief Erase both flash slots and reset the RAM copy to empty defaults.
 *
 * Factory-reset of the config (no alarms/lights, lamp fallbacks, led_count 1).
 * Blocking.
 *
 * @return HAL_OK on success, HAL error status otherwise.
 */
HAL_StatusTypeDef manifest_wipe(void);

/**
 * @brief Read-only view of the current RAM manifest.
 *
 * @return Pointer to the in-RAM manifest (valid after manifest_load()).
 */
const manifest_t *manifest_get(void);

#endif
