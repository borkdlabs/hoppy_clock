/**
 * Framed USB CDC protocol, mirroring firmware/Core/Inc/usb_cmd.h.
 *
 * Frame (both directions):
 *
 *   [SOF][cmd][len][payload 0..len-1][crc8]
 *
 * crc8 (poly 0x07, init 0x00) covers cmd + len + payload. A response echoes
 * the request cmd and its payload[0] is a status byte (STATUS_OK = 0).
 */

export const SOF = 0xa5;
export const MAX_PAYLOAD = 64;

export const STATUS_OK = 0x00;
export const STATUS_ERR = 0x01;

/** Command IDs. Must match usb_cmd.h. */
export const CMD = {
  PING: 0x01,
  SET_TIME: 0x10,
  GET_TIME: 0x11,
  SET_LED: 0x20,
  CFG_BEGIN: 0x30,
  CFG_SET_ALARM: 0x31,
  CFG_COMMIT: 0x32,
  CFG_GET_COUNT: 0x33,
  CFG_GET_ALARM: 0x34,
  CFG_SET_LIGHT: 0x35,
  CFG_SET_LAMP: 0x36,
  CFG_GET_LIGHT: 0x37,
  CFG_SET_LEDS: 0x38,
  CFG_SET_BTN: 0x39,
  SND_BEGIN: 0x40,
  SND_DATA: 0x41,
  SND_END: 0x42,
  SND_INFO: 0x43,
  SND_PLAY: 0x44,
  SND_STOP: 0x45,
  WIPE: 0x50,
};

/** Reverse lookup for log lines, e.g. 0x10 -> "SET_TIME". */
const CMD_NAMES = Object.fromEntries(Object.entries(CMD).map(([name, id]) => [id, name]),);

/**
 * Name a command ID for display.
 *
 * @param {number} cmd Command ID.
 * @returns {string} Its name, or a hex literal if unknown.
 */
export function cmdName(cmd) {
  return CMD_NAMES[cmd] ?? `0x${cmd.toString(16).padStart(2, '0')}`;
}

/**
 * CRC-8, poly 0x07, init 0x00.
 *
 * @param {Uint8Array|number[]} data Bytes to checksum.
 * @returns {number} The checksum, 0-255.
 */
export function crc8(data) {
  let crc = 0;
  for (const b of data) {
    crc ^= b;
    for (let i = 0; i < 8; i++) {
      crc = crc & 0x80 ? ((crc << 1) ^ 0x07) & 0xff : (crc << 1) & 0xff;
    }
  }
  return crc;
}

/**
 * Build a request frame.
 *
 * @param {number} cmd Command ID.
 * @param {Uint8Array|number[]} [payload] Payload bytes, at most MAX_PAYLOAD.
 * @returns {Uint8Array} The framed bytes, ready to write.
 */
export function buildFrame(cmd, payload = []) {
  if (payload.length > MAX_PAYLOAD) {
    throw new RangeError(`payload of ${payload.length} B exceeds the ${MAX_PAYLOAD} B limit`,);
  }
  const body = Uint8Array.from([cmd, payload.length, ...payload]);
  return Uint8Array.from([SOF, ...body, crc8(body)]);
}

/**
 * Incremental frame parser.
 *
 * Web Serial delivers arbitrary-sized chunks that may split or coalesce
 * frames, so bytes are fed through a state machine rather than read in fixed
 * blocks. A frame failing CRC is dropped and the parser resyncs on the next
 * SOF.
 */
export class FrameParser {
  #state = 'sof';
  #cmd = 0;
  #len = 0;
  #payload = [];

  /** Discard any partially received frame. */
  reset() {
    this.#state = 'sof';
    this.#payload = [];
  }

  /**
   * Feed received bytes in.
   *
   * @param {Uint8Array} chunk Bytes off the wire.
   * @returns {Array<{cmd: number, payload: Uint8Array}>} Completed frames.
   * @throws Never; malformed frames are dropped silently.
   */
  push(chunk) {
    const frames = [];

    for (const b of chunk) {
      switch (this.#state) {
        case 'sof':
          if (b === SOF) {
            this.#state = 'cmd';
          }
          break;

        case 'cmd':
          this.#cmd = b;
          this.#state = 'len';
          break;

        case 'len':
          this.#len = b;
          this.#payload = [];
          this.#state = b > MAX_PAYLOAD ? 'sof' : b === 0 ? 'crc' : 'payload';
          break;

        case 'payload':
          this.#payload.push(b);
          if (this.#payload.length === this.#len) {
            this.#state = 'crc';
          }
          break;

        case 'crc': {
          const body = [this.#cmd, this.#len, ...this.#payload];
          if (b === crc8(body)) {
            frames.push({
              cmd: this.#cmd, payload: Uint8Array.from(this.#payload),
            });
          }
          this.#state = 'sof';
          break;
        }
      }
    }

    return frames;
  }
}
