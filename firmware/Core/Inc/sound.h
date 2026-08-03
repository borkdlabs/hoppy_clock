/*******************************************************************************
 * @file sound.h
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

#ifndef HOPPY_CLOCK__SOUND_H
#define HOPPY_CLOCK__SOUND_H

/** Includes. *****************************************************************/

#include "stm32l4xx_hal.h"
#include <stdbool.h>
#include <stdint.h>

/** Definitions. **************************************************************/

#define SOUND_MAX_COUNT 2u // Distinct sounds; two large slots for full songs.

// Audio formats (sound_entry_t.format).
#define SOUND_FORMAT_PCM_U8 0u  // Unsigned 8-bit PCM, 1 byte/sample.
#define SOUND_FORMAT_PCM_S16 1u // Signed 16-bit LE PCM, 2 bytes/sample.

// sound_entry_t.flags bits.
#define SOUND_FLAG_VALID (1u << 0) // Entry holds a committed sound.

/** Public types. *************************************************************/

/**
 * @brief One sound index entry (16 bytes, packed to a fixed on-flash layout).
 */
typedef struct __attribute__((packed)) {
  uint32_t offset;      // Byte offset of the blob in flash.
  uint32_t length;      // Blob length in bytes.
  uint16_t sample_rate; // Playback rate in Hz.
  uint8_t format;       // SOUND_FORMAT_*.
  uint8_t flags;        // SOUND_FLAG_* bits.
  uint32_t crc32;       // Over the blob bytes (zlib-compatible).
} sound_entry_t;

/** Public functions. *********************************************************/

/**
 * @brief Load the sound index from flash into RAM.
 *
 * Call once at boot after w25q_init(). A blank or corrupt index yields no valid
 * sounds (alarms then ring LED-only).
 */
void sound_init(void);

/**
 * @brief Scheduler task: stop streaming once the current clip has drained.
 *
 * Call periodically while sounds may be playing (a 10 ms cadence is fine).
 */
void sound_task(void);

/**
 * @brief Look up a sound's index entry.
 *
 * @param id Sound id (0..SOUND_MAX_COUNT-1).
 * @param out Filled when true is returned.
 *
 * @return true if the id holds a valid sound, false otherwise.
 */
bool sound_get_info(uint8_t id, sound_entry_t *out);

/**
 * @brief Start streaming a sound to the amp (one-shot), enabling the amp.
 *
 * Switches the flash to memory-mapped mode and feeds the DAC via a DMA refill
 * callback. sound_task() ends it when drained, or call sound_stop().
 *
 * @param id Sound id to play.
 * @param fade_ms Volume fade-in duration in ms (0 = start at full volume).
 *
 * @return true if playback started, false if the id is invalid or busy.
 */
bool sound_start(uint8_t id, uint32_t fade_ms);

/**
 * @brief Stop playback and return the flash to indirect mode. No-op if idle.
 */
void sound_stop(void);

/**
 * @brief Whether a sound is currently streaming.
 */
bool sound_is_playing(void);

/**
 * @brief Whether a USB sound write is open (a blob is being programmed).
 *
 * True from the end of sound_write_begin() until sound_write_end() (or an
 * abort). Other subsystems should hold off DMA/flash activity (LED updates,
 * playback) during this window to avoid contending with the program DMA.
 */
bool sound_is_writing(void);

/**
 * @brief Begin writing a sound: validate, erase its slot, arm the byte stream.
 *
 * @param id Sound id.
 * @param format SOUND_FORMAT_*.
 * @param sample_rate Playback rate in Hz.
 * @param total_len Total blob length in bytes (1..SOUND_SLOT_SIZE).
 *
 * @return HAL_OK if the slot is erased and ready, HAL_ERROR on bad args.
 */
HAL_StatusTypeDef sound_write_begin(uint8_t id, uint8_t format,
                                    uint16_t sample_rate, uint32_t total_len);

/**
 * @brief Append the next chunk of blob bytes (in order) to the open write.
 *
 * @param data Bytes to program.
 * @param len Number of bytes.
 *
 * @return HAL_OK on success, HAL_ERROR if no write is open or it overruns.
 */
HAL_StatusTypeDef sound_write_data(const uint8_t *data, uint8_t len);

/**
 * @brief Finish the open write: verify the CRC and commit the index entry.
 *
 * @param host_crc CRC-32 the host computed over the blob.
 *
 * @return HAL_OK if the length and CRC match and the index was written,
 * HAL_ERROR otherwise (the entry is left uncommitted).
 */
HAL_StatusTypeDef sound_write_end(uint32_t host_crc);

/**
 * @brief Erase stored sounds. Always clears the index (all sounds invalid),
 * with full, also scrubs the audio data region (slow but thorough).
 *
 * @param full true to also erase the whole data region byte-for-byte.
 *
 * @return HAL_OK on success, HAL error status otherwise.
 */
HAL_StatusTypeDef sound_wipe(bool full);

#endif
