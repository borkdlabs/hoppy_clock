/*******************************************************************************
 * @file manifest.c
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

/** Includes. *****************************************************************/

#include "manifest.h"
#include "w25q128jv_hal_qspi.h"
#include <stddef.h>
#include <string.h>

/** Definitions. **************************************************************/

// Slot base addresses: one 4 KB sector each, at the start of the flash.
#define MANIFEST_SLOT_A_ADDR (0u * W25Q_SECTOR_SIZE)
#define MANIFEST_SLOT_B_ADDR (1u * W25Q_SECTOR_SIZE)

// Blocking-op guard: manifest_save() erases + programs < 1 KB, well under this.
#define MANIFEST_FLASH_TIMEOUT_MS 1000u

// s_active_slot sentinel: no valid slot loaded yet (first save targets A).
#define MANIFEST_SLOT_NONE 0xFFu

// Compile-time layout guarantees (the on-flash image is contract, not luck).
_Static_assert(sizeof(alarm_record_t) == 12u,
               "alarm_record_t must be 12 bytes");
_Static_assert(sizeof(light_seq_t) == 12u, "light_seq_t must be 12 bytes");
_Static_assert(sizeof(manifest_header_t) == 22u, "header must be 22 bytes");
_Static_assert(sizeof(manifest_t) <= W25Q_SECTOR_SIZE,
               "manifest must fit in one sector");
_Static_assert(offsetof(manifest_header_t, crc32) == 18u,
               "crc32 must be the last header field");

/** Private variables. ********************************************************/

// The working copy: what the rest of the firmware reads, and what save writes.
static manifest_t s_manifest;

// Scratch for reading/verifying a slot without disturbing s_manifest.
static manifest_t s_scratch;

// Which slot s_manifest came from (0 = A, 1 = B, MANIFEST_SLOT_NONE = none),
// and its sequence number, so save targets the idle slot with seq + 1.
static uint8_t s_active_slot = MANIFEST_SLOT_NONE;
static uint32_t s_active_seq = 0u;

/** Private functions. ********************************************************/

/**
 * @brief Reflected CRC-32 (poly 0xEDB88320), streaming update.
 *
 * Caller supplies the running value, init with 0xFFFFFFFF and XOR the result
 * with 0xFFFFFFFF at the end (see manifest_crc). Matches zlib.crc32.
 */
static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t k = 0; k < 8u; k++) {
      crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
    }
  }
  return crc;
}

/**
 * @brief CRC-32 over a manifest image, excluding the header.crc32 field.
 */
static uint32_t manifest_crc(const manifest_t *m) {
  const uint8_t *base = (const uint8_t *)m;
  uint32_t crc = 0xFFFFFFFFu;
  // Header up to (but not including) the crc32 field.
  crc = crc32_update(crc, base, offsetof(manifest_t, header.crc32));
  // Then the alarm and light tables (skipping the crc32 field itself).
  crc = crc32_update(crc, (const uint8_t *)m->alarms, sizeof(m->alarms));
  crc = crc32_update(crc, (const uint8_t *)m->lights, sizeof(m->lights));
  return crc ^ 0xFFFFFFFFu;
}

/**
 * @brief Whether a freshly read image is a valid, current-version manifest.
 */
static bool slot_valid(const manifest_t *m) {
  if (m->header.magic != MANIFEST_MAGIC) {
    return false;
  }
  if (m->header.version != MANIFEST_VERSION) {
    return false;
  }
  if (m->header.alarm_count > MANIFEST_MAX_ALARMS) {
    return false;
  }
  if (m->header.light_count > MANIFEST_MAX_LIGHTS) {
    return false;
  }
  return manifest_crc(m) == m->header.crc32;
}

/**
 * @brief Block until the W25Q driver returns to idle, or time out.
 */
static HAL_StatusTypeDef flash_wait_idle(void) {
  uint32_t start = HAL_GetTick();
  for (;;) {
    switch (w25q_get_state()) {
    case W25Q_STATE_IDLE:
      return HAL_OK;
    case W25Q_STATE_ERROR:
      return HAL_ERROR;
    default:
      break;
    }
    if (HAL_GetTick() - start > MANIFEST_FLASH_TIMEOUT_MS) {
      return HAL_TIMEOUT;
    }
  }
}

/**
 * @brief Blocking wrappers over the non-blocking W25Q driver.
 */
static HAL_StatusTypeDef flash_read(uint32_t addr, void *buf, uint32_t len) {
  HAL_StatusTypeDef st = w25q_read(addr, (uint8_t *)buf, len);
  return (st == HAL_OK) ? flash_wait_idle() : st;
}

static HAL_StatusTypeDef flash_write(uint32_t addr, const void *buf,
                                     uint32_t len) {
  HAL_StatusTypeDef st = w25q_write(addr, (const uint8_t *)buf, len);
  return (st == HAL_OK) ? flash_wait_idle() : st;
}

static HAL_StatusTypeDef flash_erase_sector(uint32_t addr) {
  HAL_StatusTypeDef st = w25q_erase_sector(addr);
  return (st == HAL_OK) ? flash_wait_idle() : st;
}

/**
 * @brief Set the RAM copy to an empty, valid manifest (no alarms).
 */
static void manifest_set_defaults(void) {
  memset(&s_manifest, 0, sizeof(s_manifest));
  s_manifest.header.magic = MANIFEST_MAGIC;
  s_manifest.header.version = MANIFEST_VERSION;
  s_manifest.header.alarm_count = 0u;
  s_manifest.header.light_count = 0u;
  s_manifest.header.lamp_on_light = 0u;
  s_manifest.header.lamp_off_light = 0u;
  s_manifest.header.led_count = 1u; // Onboard LED until configured.
  s_manifest.header.seq_no = 0u;
  s_manifest.header.crc32 = manifest_crc(&s_manifest);
}

/** Public functions. *********************************************************/

manifest_load_result_t manifest_load(void) {
  w25q_mmap_disable(); // Indirect mode required for reads.

  const uint32_t addr[2] = {MANIFEST_SLOT_A_ADDR, MANIFEST_SLOT_B_ADDR};
  bool found = false;

  for (uint8_t i = 0; i < 2u; i++) {
    if (flash_read(addr[i], &s_scratch, sizeof(s_scratch)) != HAL_OK) {
      continue;
    }
    if (!slot_valid(&s_scratch)) {
      continue;
    }
    // Prefer the higher sequence number, the first valid slot seeds it.
    if (!found || s_scratch.header.seq_no > s_active_seq) {
      memcpy(&s_manifest, &s_scratch, sizeof(s_manifest));
      s_active_slot = i;
      s_active_seq = s_scratch.header.seq_no;
      found = true;
    }
  }

  if (!found) {
    manifest_set_defaults();
    s_active_slot = MANIFEST_SLOT_NONE;
    s_active_seq = 0u;
    return MANIFEST_LOAD_DEFAULTS;
  }
  return (s_active_slot == 0u) ? MANIFEST_LOAD_SLOT_A : MANIFEST_LOAD_SLOT_B;
}

bool manifest_set_alarms(const alarm_record_t *alarms, uint16_t count) {
  if (count > MANIFEST_MAX_ALARMS) {
    return false;
  }
  memset(s_manifest.alarms, 0, sizeof(s_manifest.alarms));
  memcpy(s_manifest.alarms, alarms, (size_t)count * sizeof(alarm_record_t));
  s_manifest.header.alarm_count = count;
  return true;
}

bool manifest_set_lights(const light_seq_t *lights, uint16_t count) {
  if (count > MANIFEST_MAX_LIGHTS) {
    return false;
  }
  memset(s_manifest.lights, 0, sizeof(s_manifest.lights));
  memcpy(s_manifest.lights, lights, (size_t)count * sizeof(light_seq_t));
  s_manifest.header.light_count = count;
  return true;
}

void manifest_set_lamp(uint8_t on_light, uint8_t off_light) {
  s_manifest.header.lamp_on_light = on_light;
  s_manifest.header.lamp_off_light = off_light;
}

void manifest_set_led_count(uint8_t led_count) {
  s_manifest.header.led_count = led_count;
}

HAL_StatusTypeDef manifest_save(void) {
  w25q_mmap_disable(); // Indirect mode required for erase/program.

  // Target the idle slot, a fresh (no valid slot) save lands in A.
  uint8_t target = (s_active_slot == 0u) ? 1u : 0u;
  uint32_t addr = (target == 0u) ? MANIFEST_SLOT_A_ADDR : MANIFEST_SLOT_B_ADDR;
  uint32_t next_seq = s_active_seq + 1u;

  // Finalize the header and CRC over the working copy.
  s_manifest.header.magic = MANIFEST_MAGIC;
  s_manifest.header.version = MANIFEST_VERSION;
  s_manifest.header.seq_no = next_seq;
  s_manifest.header.crc32 = manifest_crc(&s_manifest);

  HAL_StatusTypeDef st = flash_erase_sector(addr);
  if (st != HAL_OK) {
    return st;
  }
  st = flash_write(addr, &s_manifest, sizeof(s_manifest));
  if (st != HAL_OK) {
    return st;
  }

  // Read back and verify before promoting the slot to active.
  st = flash_read(addr, &s_scratch, sizeof(s_scratch));
  if (st != HAL_OK) {
    return st;
  }
  if (!slot_valid(&s_scratch) || s_scratch.header.seq_no != next_seq) {
    return HAL_ERROR;
  }

  s_active_slot = target;
  s_active_seq = next_seq;
  return HAL_OK;
}

const manifest_t *manifest_get(void) { return &s_manifest; }
