/*******************************************************************************
 * @file sound.c
 * @brief Sound asset region in external W25Q NOR flash: store + stream.
 *******************************************************************************
 * @note:
 * Alarm sounds live in NOR flash, written over USB and streamed to the amp at
 * ring time. Layout: a single index sector (id -> location) plus a data region
 * of fixed per-id slots.
 *
 *   Index sector: magic | crc32 | sound_entry_t[SOUND_MAX_COUNT]
 *   Data region : slot i at SOUND_REGION_ADDR + i * SOUND_SLOT_SIZE
 *
 * Audio is linear PCM, per-entry format: PCM_U8 (1 byte/sample, 128 = silence)
 * for short effects, or PCM_S16 (signed 16-bit LE, 2 bytes/sample) for full-
 * quality songs. Both decode to the DAC's 12-bit range at play time. A format
 * field leaves room for a compressed codec (e.g. ADPCM) later.
 *
 * Writing (indirect mode): sound_write_begin() erases the slot,
 * sound_write_data() streams bytes, sound_write_end() verifies the CRC and
 * commits the index entry. Playback (memory-mapped mode): sound_start() streams
 * the blob to the amp via a DMA refill callback, sound_task() stops it when the
 * clip ends. Writing and playback never overlap (config vs. ringing), so the
 * two QUADSPI modes do not collide.
 *******************************************************************************
 */

/** Includes. *****************************************************************/

#include "sound.h"
#include "pam8302a_hal_dac.h"
#include "w25q128jv_hal_qspi.h"
#include <stddef.h>
#include <string.h>

/** Definitions. **************************************************************/

// Flash map (sectors 0/1 hold the manifest A/B slots).
#define SOUND_INDEX_ADDR (2u * W25Q_SECTOR_SIZE) // Index sector at 0x002000.
#define SOUND_REGION_ADDR 0x010000u              // Data region base (64 KB in).
// Fixed bytes reserved per id: 7.5 MB ~= 4 min at 16-bit / 16 kHz (32 KB/s).
// Two slots (SOUND_MAX_COUNT) fit in the 16 MB chip after the region base.
#define SOUND_SLOT_SIZE (7680u * 1024u)
#define SOUND_INDEX_MAGIC 0x444E5348u // "HSND".

// DMA ring for streaming: two halves refilled from flash. 512 samples at
// 16 kHz is ~16 ms per half -- ample time for the refill callback.
#define SOUND_RING_SAMPLES 512u

// Blocking-op guard for erase/program/read wrappers. A 64 KB block erase can
// take up to ~2 s (tBE2), so allow margin above that.
#define SOUND_FLASH_TIMEOUT_MS 3000u

// A page program can occasionally fail on a transient in the async QUADSPI
// completion path under sustained streaming. The driver recovers on the next
// op and re-programming identical bytes is safe, so retry the chunk.
#define SOUND_WRITE_RETRIES 4u

/**
 * @brief On-flash sound index image (magic + CRC + entry table).
 */
typedef struct __attribute__((packed)) {
  uint32_t magic;
  uint32_t crc32; // Over entries[].
  sound_entry_t entries[SOUND_MAX_COUNT];
} sound_index_t;

_Static_assert(sizeof(sound_entry_t) == 16u, "sound_entry_t must be 16 bytes");
_Static_assert(sizeof(sound_index_t) <= W25Q_SECTOR_SIZE,
               "sound index must fit in one sector");
_Static_assert(SOUND_REGION_ADDR + SOUND_MAX_COUNT * SOUND_SLOT_SIZE <=
                   16u * 1024u * 1024u,
               "sound slots overflow the W25Q128 (16 MB)");
_Static_assert(SOUND_REGION_ADDR % W25Q_BLOCK_SIZE == 0u &&
                   SOUND_SLOT_SIZE % W25Q_BLOCK_SIZE == 0u,
               "region/slot must be 64 KB aligned so block erase can't spill");

/** Private variables. ********************************************************/

// RAM mirror of the index.
static sound_index_t s_index;

// Open write state (indirect-mode programming, driven by USB).
static bool s_wr_active = false;
static uint8_t s_wr_id = 0u;
static uint8_t s_wr_format = 0u;
static uint16_t s_wr_rate = 0u;
static uint32_t s_wr_base = 0u;   // Flash address of the slot.
static uint32_t s_wr_len = 0u;    // Expected total bytes.
static uint32_t s_wr_cursor = 0u; // Bytes written so far.
static uint32_t s_wr_crc = 0u;    // Running CRC-32 state (pre-final-xor).

// Playback state (memory-mapped streaming, driven by the amp DMA ISR).
static uint16_t s_ring[SOUND_RING_SAMPLES];
static bool s_playing = false;
static volatile bool s_play_done = false;
static const uint8_t *s_play_src = NULL;     // Into memory-mapped flash.
static volatile uint32_t s_play_cursor = 0u; // Sample index.
static uint32_t s_play_len = 0u;             // Total samples in the clip.
static uint32_t s_play_fade = 0u; // Fade-in length in samples (0 = none).
static uint8_t s_play_format = SOUND_FORMAT_PCM_U8;

/** Private functions. ********************************************************/

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t k = 0; k < 8u; k++) {
      crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
    }
  }
  return crc;
}

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
    if (HAL_GetTick() - start > SOUND_FLASH_TIMEOUT_MS) {
      return HAL_TIMEOUT;
    }
  }
}

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

static HAL_StatusTypeDef flash_erase_block(uint32_t addr) {
  HAL_StatusTypeDef st = w25q_erase_block_64k(addr);
  return (st == HAL_OK) ? flash_wait_idle() : st;
}

/**
 * @brief Rewrite the whole index sector from the RAM mirror.
 */
static HAL_StatusTypeDef index_persist(void) {
  s_index.magic = SOUND_INDEX_MAGIC;
  s_index.crc32 = crc32_update(0xFFFFFFFFu, (const uint8_t *)s_index.entries,
                               sizeof(s_index.entries)) ^
                  0xFFFFFFFFu;

  HAL_StatusTypeDef st = flash_erase_sector(SOUND_INDEX_ADDR);
  if (st != HAL_OK) {
    return st;
  }
  return flash_write(SOUND_INDEX_ADDR, &s_index, sizeof(s_index));
}

/**
 * @brief Decode one sample (by index) to a 12-bit DAC code for the format.
 */
static uint16_t decode_sample(uint32_t i) {
  if (s_play_format == SOUND_FORMAT_PCM_S16) {
    const uint32_t o = i * 2u; // 2 bytes/sample, little-endian.
    const int16_t s = (int16_t)((uint16_t)s_play_src[o] |
                                ((uint16_t)s_play_src[o + 1u] << 8));
    return amp_pcm16_to_dac12(s);
  }
  return (uint16_t)s_play_src[i] << 4; // PCM_U8: 128 -> mid-scale.
}

/**
 * @brief Scale a DAC code toward mid-scale for the fade-in ramp (ISR context).
 *
 * During the first s_play_fade samples the AC swing about mid-scale is scaled
 * by cursor/fade (0 -> silence, fade -> full volume). 64-bit intermediate to
 * avoid overflow of (swing * cursor).
 */
static uint16_t apply_fade(uint16_t code, uint32_t cursor) {
  if (s_play_fade == 0u || cursor >= s_play_fade) {
    return code;
  }
  int64_t swing = (int64_t)code - (int64_t)AMP_DAC_MIDSCALE;
  swing = swing * (int64_t)cursor / (int64_t)s_play_fade;
  return (uint16_t)((int64_t)AMP_DAC_MIDSCALE + swing);
}

/**
 * @brief DMA refill: decode the next samples to 12-bit DAC codes (ISR context).
 */
static void stream_refill(uint16_t *dst, uint16_t count) {
  for (uint16_t i = 0u; i < count; i++) {
    if (s_play_cursor < s_play_len) {
      dst[i] = apply_fade(decode_sample(s_play_cursor), s_play_cursor);
      s_play_cursor++;
    } else {
      dst[i] = AMP_DAC_MIDSCALE;
      s_play_done = true;
    }
  }
}

/** Public functions. *********************************************************/

void sound_init(void) {
  w25q_mmap_disable();

  if (flash_read(SOUND_INDEX_ADDR, &s_index, sizeof(s_index)) != HAL_OK) {
    memset(&s_index, 0, sizeof(s_index));
    return;
  }

  const uint32_t crc =
      crc32_update(0xFFFFFFFFu, (const uint8_t *)s_index.entries,
                   sizeof(s_index.entries)) ^
      0xFFFFFFFFu;
  if (s_index.magic != SOUND_INDEX_MAGIC || crc != s_index.crc32) {
    memset(&s_index, 0, sizeof(s_index)); // No valid sounds.
  }
}

bool sound_get_info(uint8_t id, sound_entry_t *out) {
  if (id >= SOUND_MAX_COUNT) {
    return false;
  }
  const sound_entry_t *e = &s_index.entries[id];
  if ((e->flags & SOUND_FLAG_VALID) == 0u) {
    return false;
  }
  *out = *e;
  return true;
}

bool sound_start(uint8_t id, uint32_t fade_ms) {
  sound_entry_t e;
  // Never switch to memory-mapped mode while a USB write owns the flash.
  if (s_playing || s_wr_active || !sound_get_info(id, &e)) {
    return false;
  }
  if (w25q_mmap_enable() != HAL_OK) {
    return false;
  }

  s_play_src = (const uint8_t *)W25Q_MMAP_PTR(e.offset);
  s_play_format = e.format;
  // length is bytes; PCM_S16 packs 2 bytes per sample.
  s_play_len = (e.format == SOUND_FORMAT_PCM_S16) ? (e.length / 2u) : e.length;
  s_play_cursor = 0u;
  s_play_done = false;

  // Fade length in samples, clamped so it never exceeds the clip.
  s_play_fade = (uint32_t)((uint64_t)fade_ms * e.sample_rate / 1000u);
  if (s_play_fade > s_play_len) {
    s_play_fade = s_play_len;
  }

  amp_enable(); // Owned here so both the alarm and USB-play paths match.
  amp_set_sample_rate(e.sample_rate);
  if (amp_play_stream(s_ring, (uint16_t)SOUND_RING_SAMPLES, stream_refill) !=
      HAL_OK) {
    amp_disable();
    w25q_mmap_disable();
    return false;
  }
  s_playing = true;
  return true;
}

void sound_stop(void) {
  if (!s_playing) {
    return;
  }
  amp_stop();
  amp_disable();
  w25q_mmap_disable();
  s_playing = false;
  s_play_done = false;
}

bool sound_is_playing(void) { return s_playing; }

bool sound_is_writing(void) { return s_wr_active; }

void sound_task(void) {
  if (s_playing && s_play_done) {
    sound_stop();
  }
}

HAL_StatusTypeDef sound_write_begin(uint8_t id, uint8_t format,
                                    uint16_t sample_rate, uint32_t total_len) {
  if (id >= SOUND_MAX_COUNT ||
      (format != SOUND_FORMAT_PCM_U8 && format != SOUND_FORMAT_PCM_S16) ||
      total_len == 0u || total_len > SOUND_SLOT_SIZE) {
    return HAL_ERROR;
  }
  if (s_playing) {
    return HAL_ERROR; // Never erase under an active stream.
  }

  w25q_mmap_disable();

  // Erase only what the blob needs, in 64 KB blocks. Slot bases are 64 KB
  // aligned, so whole-block erases never spill into a neighbouring slot, and a
  // multi-MB song erases in ~16x fewer commands than sector-erasing.
  s_wr_base = SOUND_REGION_ADDR + (uint32_t)id * SOUND_SLOT_SIZE;
  const uint32_t blocks = (total_len + W25Q_BLOCK_SIZE - 1u) / W25Q_BLOCK_SIZE;
  for (uint32_t k = 0u; k < blocks; k++) {
    HAL_StatusTypeDef st = flash_erase_block(s_wr_base + k * W25Q_BLOCK_SIZE);
    if (st != HAL_OK) {
      return st;
    }
  }

  s_wr_id = id;
  s_wr_format = format;
  s_wr_rate = sample_rate;
  s_wr_len = total_len;
  s_wr_cursor = 0u;
  s_wr_crc = 0xFFFFFFFFu;
  s_wr_active = true;
  return HAL_OK;
}

HAL_StatusTypeDef sound_write_data(const uint8_t *data, uint8_t len) {
  if (!s_wr_active || len == 0u || (uint32_t)s_wr_cursor + len > s_wr_len) {
    return HAL_ERROR;
  }

  // Retry transient program failures; re-writing the same bytes is idempotent.
  HAL_StatusTypeDef st = HAL_ERROR;
  for (uint8_t attempt = 0u; attempt < SOUND_WRITE_RETRIES; attempt++) {
    st = flash_write(s_wr_base + s_wr_cursor, data, len);
    if (st == HAL_OK) {
      break;
    }
    HAL_Delay(1u); // Let the QUADSPI/DMA IRQs settle before retrying.
  }
  if (st != HAL_OK) {
    s_wr_active = false;
    return st;
  }
  s_wr_crc = crc32_update(s_wr_crc, data, len);
  s_wr_cursor += len;
  return HAL_OK;
}

HAL_StatusTypeDef sound_write_end(uint32_t host_crc) {
  if (!s_wr_active || s_wr_cursor != s_wr_len) {
    s_wr_active = false;
    return HAL_ERROR;
  }

  const uint32_t crc = s_wr_crc ^ 0xFFFFFFFFu;
  if (crc != host_crc) {
    s_wr_active = false;
    return HAL_ERROR;
  }

  sound_entry_t *e = &s_index.entries[s_wr_id];
  e->offset = s_wr_base;
  e->length = s_wr_len;
  e->sample_rate = s_wr_rate;
  e->format = s_wr_format;
  e->flags = SOUND_FLAG_VALID;
  e->crc32 = host_crc;

  HAL_StatusTypeDef st = index_persist();
  s_wr_active = false;
  return st;
}

HAL_StatusTypeDef sound_wipe(bool full) {
  sound_stop();        // Stop any playback (returns flash to indirect mode).
  w25q_mmap_disable(); // Ensure indirect mode for erase.
  s_wr_active = false; // Cancel any open write.

  // Erase the index first so sounds are invalid immediately (a power loss
  // during the optional data wipe still leaves the device with no sounds).
  HAL_StatusTypeDef st = flash_erase_sector(SOUND_INDEX_ADDR);
  if (st != HAL_OK) {
    return st;
  }
  memset(&s_index, 0, sizeof(s_index));

  // Optionally scrub the audio data region too (slow: the whole region in
  // 64 KB blocks). Otherwise the stale bytes stay but are unreferenced.
  if (full) {
    const uint32_t bytes = (uint32_t)SOUND_MAX_COUNT * SOUND_SLOT_SIZE;
    for (uint32_t off = 0u; off < bytes; off += W25Q_BLOCK_SIZE) {
      st = flash_erase_block(SOUND_REGION_ADDR + off);
      if (st != HAL_OK) {
        return st;
      }
    }
  }
  return HAL_OK;
}
