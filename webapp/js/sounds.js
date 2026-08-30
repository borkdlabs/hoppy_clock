/**
 * Stored sound slots, mirroring sound_entry_t in firmware/Core/Inc/sound.h.
 *
 * The blobs themselves are far too large for this page to care about: it only
 * reads back what each slot holds and asks the firmware to play it. Uploading
 * is still the Python tool's job, since that means decoding and resampling the
 * source audio first.
 */

/** SOUND_MAX_COUNT: two large slots, sized for whole songs. */
export const MAX_SOUNDS = 2;

/** SOUND_FORMAT_* values, by index. */
export const FORMATS = ['u8', 's16'];

/** Bytes per sample for each format, for the duration estimate. */
const BYTES_PER_SAMPLE = {u8: 1, s16: 2};

/**
 * @typedef {object} Sound
 * @property {string} format One of FORMATS.
 * @property {number} rateHz Sample rate.
 * @property {number} lengthBytes Blob length.
 * @property {number} crc32 Checksum the firmware verified on commit.
 */

/**
 * Decode a SND_INFO response body (the status byte already stripped).
 *
 * @param {Uint8Array} data The 11 bytes after the status.
 * @returns {Sound} The slot's entry.
 * @throws {RangeError} If the response is too short.
 */
export function decodeSound(data) {
  if (data.length < 11) {
    throw new RangeError(`sound info is ${data.length} B, need 11 B`);
  }
  const view = new DataView(data.buffer, data.byteOffset, 11);
  return {
    format: FORMATS[data[0]] ?? data[0],
    rateHz: view.getUint16(1, true),
    lengthBytes: view.getUint32(3, true),
    crc32: view.getUint32(7, true),
  };
}

/**
 * How long a slot plays for, from its length and rate.
 *
 * @param {Sound} sound The slot entry.
 * @returns {number} Seconds, or 0 when the rate is unknown.
 */
export function soundSeconds(sound) {
  const bytes = BYTES_PER_SAMPLE[sound.format] ?? 1;
  return sound.rateHz ? sound.lengthBytes / (sound.rateHz * bytes) : 0;
}

/**
 * Describe a slot the way the Python tool's sound-info does.
 *
 * @param {Sound} sound The slot entry.
 * @returns {string} e.g. "s16, 16000 Hz, 231.4 kB (7.2 s)".
 */
export function describeSound(sound) {
  const kb = (sound.lengthBytes / 1000).toFixed(1);
  const secs = soundSeconds(sound);
  const clock = secs >= 60 ? `${Math.floor(secs / 60)}m ${Math.round(secs % 60)}s` : `${secs.toFixed(1)} s`;
  return `${sound.format}, ${sound.rateHz} Hz, ${kb} kB (${clock})`;
}
