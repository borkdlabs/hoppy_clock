/**
 * Stored sound slots, mirroring sound_entry_t in firmware/Core/Inc/sound.h.
 *
 * Also holds the upload pipeline: the browser decodes anything it can play
 * (WAV, MP3, FLAC, OGG, M4A) through the Web Audio API, so a file is turned
 * into mono PCM at the target rate here rather than by an external tool.
 */

/** SOUND_MAX_COUNT: two large slots, sized for whole songs. */
export const MAX_SOUNDS = 2;

/** SOUND_FORMAT_* values, by index. */
export const FORMATS = ['u8', 's16'];

/** Bytes per sample for each format, for the duration estimate. */
const BYTES_PER_SAMPLE = {u8: 1, s16: 2};

/** SOUND_SLOT_SIZE: the flash a single sound gets. */
export const SLOT_BYTES = 7680 * 1024;

/** SND_RATE_HZ: 16-bit PCM at 16 kHz is what the firmware targets. */
export const DEFAULT_RATE_HZ = 16000;

/** Bytes per SND_DATA frame; must stay within USB_CMD_MAX_PAYLOAD. */
export const CHUNK_BYTES = 64;

/**
 * How many seconds fit in one slot at a given rate and format.
 *
 * @param {number} rateHz Sample rate.
 * @param {string} format One of FORMATS.
 * @returns {number} Seconds.
 */
export function slotSeconds(rateHz, format) {
  return SLOT_BYTES / (rateHz * (BYTES_PER_SAMPLE[format] ?? 1));
}

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

/** Lazily built CRC-32 table, reflected polynomial 0xEDB88320. */
let crcTable = null;

/**
 * CRC-32 over a blob, matching zlib.crc32 and the firmware's check.
 *
 * @param {Uint8Array} bytes The data to sum.
 * @returns {number} The checksum as an unsigned 32-bit value.
 */
export function crc32(bytes) {
  if (!crcTable) {
    crcTable = new Uint32Array(256);
    for (let i = 0; i < 256; i++) {
      let c = i;
      for (let bit = 0; bit < 8; bit++) {
        c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
      }
      crcTable[i] = c;
    }
  }

  let crc = 0xffffffff;
  for (const byte of bytes) {
    crc = crcTable[(crc ^ byte) & 0xff] ^ (crc >>> 8);
  }
  return (crc ^ 0xffffffff) >>> 0;
}

/**
 * Encode mono samples in [-1, 1] as the firmware's PCM.
 *
 * u8 is centred on 128 and s16 is little-endian signed, both scaled by gain
 * and clipped, exactly as the Python tool encodes them.
 *
 * @param {Float32Array|number[]} samples Mono samples.
 * @param {string} format One of FORMATS.
 * @param {number} [gain] Volume, 1 = as-is.
 * @returns {Uint8Array} The PCM blob.
 */
export function encodePcm(samples, format, gain = 1) {
  const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);

  if (format === 's16') {
    const out = new Uint8Array(samples.length * 2);
    const view = new DataView(out.buffer);
    for (let i = 0; i < samples.length; i++) {
      const v = clamp(Math.round(samples[i] * 32767 * gain), -32768, 32767);
      view.setInt16(i * 2, v, true);
    }
    return out;
  }

  const out = new Uint8Array(samples.length);
  for (let i = 0; i < samples.length; i++) {
    out[i] = clamp(Math.round(samples[i] * 127 * gain) + 128, 0, 255);
  }
  return out;
}

/**
 * Synthesize a test tone, the same full-scale sine as `--tone`.
 *
 * @param {number} hz Frequency.
 * @param {number} seconds Length.
 * @param {number} rateHz Sample rate.
 * @returns {Float32Array} Mono samples.
 */
export function synthesizeTone(hz, seconds, rateHz) {
  const samples = new Float32Array(Math.max(1, Math.round(seconds * rateHz)));
  for (let i = 0; i < samples.length; i++) {
    samples[i] = Math.sin((2 * Math.PI * hz * i) / rateHz);
  }
  return samples;
}

/**
 * Decode an audio file to mono samples at the target rate.
 *
 * Whatever the browser can play, it can decode: WAV, MP3, FLAC, OGG and M4A
 * all work, with no external tool. Rendering through a one-channel offline
 * context does the downmix and the resample in the same pass.
 *
 * @param {ArrayBuffer} bytes The file's contents.
 * @param {number} rateHz Target sample rate.
 * @returns {Promise<Float32Array>} Mono samples in [-1, 1].
 * @throws {Error} If the browser cannot decode the file.
 */
export async function decodeAudioFile(bytes, rateHz) {
  // decodeAudioData resamples to its own context's rate, so decode at the
  // target rate rather than at the hardware's and resampling twice.
  const decoder = new OfflineAudioContext(1, 1, rateHz);
  const buffer = await decoder.decodeAudioData(bytes);

  const frames = Math.max(1, Math.round(buffer.duration * rateHz));
  const mixer = new OfflineAudioContext(1, frames, rateHz);
  const source = mixer.createBufferSource();
  source.buffer = buffer;
  source.connect(mixer.destination);
  source.start();
  const rendered = await mixer.startRendering();
  return rendered.getChannelData(0);
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
