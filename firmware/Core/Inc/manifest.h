/*******************************************************************************
 * @file manifest.h
 * @brief Persisted clock settings (alarm table) in external W25Q NOR flash.
 *******************************************************************************
 * @note:
 * Firmware is fixed, the user-editable settings (alarms, and later sound
 * assets) live in the NOR flash and are written over USB at firmware runtime.
 *
 * The alarm table is stored as a fixed-size image in one 4 KB sector, kept in
 * two slots (A/B) for power-safe updates. A save writes the idle slot, bumps a
 * monotonic sequence number, and writes the CRC last. On boot the valid slot
 * with the higher sequence number wins. A blank or corrupt pair yields empty
 * defaults, so a half-finished write can never brick the clock.
 *
 *   Header (16 B): magic | version | alarm_count | seq_no | crc32
 *   Alarms[MANIFEST_MAX_ALARMS] x 12 B
 *
 * crc32 is the standard reflected CRC-32 (poly 0xEDB88320, init/final
 * 0xFFFFFFFF), matching zlib/Python's zlib.crc32 so the host webapp can compute
 * it identically. It covers the whole image except the crc32 field itself.
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
#define MANIFEST_VERSION 1u
#define MANIFEST_MAX_ALARMS 64u

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

/** Public types. *************************************************************/

/**
 * @brief One alarm entry (12 bytes, packed to a fixed on-flash layout).
 *
 * An alarm fires at hour:minute:second on the days selected by flags/day_sel:
 * weekly mode matches a weekday mask (ALARM_DAY_*), monthly mode matches a
 * day-of-month. On firing it ramps LED 0 to brightness_max over ramp_time_s
 * (0 = instant), plays sound_id, then auto-quiets after timeout_s.
 */
typedef struct __attribute__((packed)) {
  uint8_t flags;          // ALARM_FLAG_* bits.
  uint8_t day_sel;        // Weekly: weekday mask. Monthly: day-of-month 1..31.
  uint8_t hour;           // 0..23.
  uint8_t minute;         // 0..59.
  uint8_t second;         // 0..59.
  uint16_t ramp_time_s;   // LED fade-to-max duration, 0 = instant.
  uint16_t timeout_s;     // Auto-quiet this long after firing.
  uint8_t sound_id;       // Index into the sound region (added later).
  uint8_t brightness_max; // LED target brightness 0..255.
  uint8_t reserved;       // Zero, reserved for future use.
} alarm_record_t;

/**
 * @brief On-flash manifest header (16 bytes, packed).
 */
typedef struct __attribute__((packed)) {
  uint32_t magic;       // MANIFEST_MAGIC.
  uint16_t version;     // MANIFEST_VERSION.
  uint16_t alarm_count; // Number of valid entries in alarms[], 0..MAX.
  uint32_t seq_no;      // Monotonic, higher = newer slot.
  uint32_t crc32;       // Over the image excluding this field.
} manifest_header_t;

/**
 * @brief Full manifest image as stored in one flash sector.
 *
 * Unused alarm slots (index >= alarm_count) are zero-filled so the image is
 * deterministic and the CRC is stable.
 */
typedef struct __attribute__((packed)) {
  manifest_header_t header;
  alarm_record_t alarms[MANIFEST_MAX_ALARMS];
} manifest_t;

/**
 * @brief Outcome of manifest_load().
 */
typedef enum {
  MANIFEST_LOAD_SLOT_A,   // Loaded from slot A.
  MANIFEST_LOAD_SLOT_B,   // Loaded from slot B.
  MANIFEST_LOAD_DEFAULTS, // Neither slot valid, RAM set to empty defaults.
} manifest_load_result_t;

/** Public functions. *********************************************************/

/**
 * @brief Read both slots, pick the newest valid one into the RAM copy.
 *
 * Requires the W25Q driver to be initialized (w25q_init) and in indirect mode,
 * this call switches it out of memory-mapped mode if needed. Blocking (used at
 * boot). If neither slot is valid, the RAM copy is set to empty defaults.
 *
 * @return Which slot was loaded, or MANIFEST_LOAD_DEFAULTS.
 */
manifest_load_result_t manifest_load(void);

/**
 * @brief Replace the RAM alarm table (does not touch flash).
 *
 * Copies up to MANIFEST_MAX_ALARMS records and zero-fills the rest. Call
 * manifest_save() afterwards to persist. Intended for the USB SET_CONFIG path.
 *
 * @param alarms Source records.
 * @param count Number of records (must be <= MANIFEST_MAX_ALARMS).
 *
 * @return true if applied, false if count exceeds MANIFEST_MAX_ALARMS.
 */
bool manifest_set_alarms(const alarm_record_t *alarms, uint16_t count);

/**
 * @brief Persist the current RAM copy to the idle slot (power-safe).
 *
 * Writes the slot not currently active with seq_no + 1 and a fresh CRC (erase,
 * program, then read-back verify), and only on success promotes it to active.
 * A failure or power loss leaves the previously active slot intact. Blocking.
 *
 * @return HAL_OK on success, HAL error status otherwise.
 */
HAL_StatusTypeDef manifest_save(void);

/**
 * @brief Read-only view of the current RAM manifest.
 *
 * @return Pointer to the in-RAM manifest (valid after manifest_load()).
 */
const manifest_t *manifest_get(void);

#endif
