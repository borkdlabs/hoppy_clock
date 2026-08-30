/**
 * Hoppy Clock device connection over Web Serial.
 *
 * The board enumerates as a plain USB CDC virtual serial port, so the browser
 * talks to it through navigator.serial rather than WebUSB (on Windows the OS
 * CDC driver owns the interface and WebUSB cannot claim it). Framing lives in
 * protocol.js; this layer owns the port lifecycle and turns frames into
 * request/response transactions.
 */

import {CMD, FrameParser, STATUS_OK, buildFrame, cmdName} from './protocol.js';
import {ALARM_BYTES, MAX_ALARMS} from './alarms.js';
import {MAX_LEDS, MAX_LIGHTS} from './lights.js';
import {
  CHUNK_BYTES, FORMATS, MAX_SOUNDS, SLOT_BYTES, crc32, decodeSound,
} from './sounds.js';

/** STM32 Virtual COM Port, from firmware/USB_DEVICE/App/usbd_desc.c. */
export const USB_FILTER = {usbVendorId: 0x0483, usbProductId: 0x5740};

/** CDC ignores the line rate, but match the Python tool's 115200. */
const BAUD_RATE = 115200;

/** How long to wait for a response before giving up on a command. */
const TXN_TIMEOUT_MS = 1000;

/**
 * Longer budget for CFG_COMMIT, which erases and rewrites a flash sector
 * before it answers. Matches the 5 s the Python tool allows.
 */
const COMMIT_TIMEOUT_MS = 5000;

/** A wipe erases the config and the sound index before answering. */
const WIPE_TIMEOUT_MS = 10000;

/** A full wipe scrubs the whole ~15 MB audio region: minutes, not seconds. */
const WIPE_FULL_TIMEOUT_MS = 300000;

/** A command that failed: no response, a bad echo, or a non-OK status. */
export class DeviceError extends Error {
  /**
   * @param {string} message Human-readable reason.
   * @param {number} [status] Status byte, when the device did reply.
   */
  constructor(message, status) {
    super(message);
    this.name = 'DeviceError';
    this.status = status;
  }
}

/** True if this browser exposes Web Serial at all. */
export function isSupported() {
  return typeof navigator !== 'undefined' && 'serial' in navigator;
}

/**
 * Sleep until the host clock crosses its next whole second.
 *
 * SET_TIME carries whole seconds only, so sending on the boundary keeps the
 * value from being stale the moment it lands.
 *
 * @returns {Promise<void>}
 */
function awaitSecondBoundary() {
  const delay = 1000 - (Date.now() % 1000);
  return new Promise((resolve) => setTimeout(resolve, delay));
}

/**
 * @typedef {object} Config
 * @property {Uint8Array[]} alarms Packed alarm records, in table order.
 * @property {Uint8Array[]} lights Packed light looks, in id order.
 * @property {number} lampOn Light id the lamp plays when switched on.
 * @property {number} lampOff Light id the lamp settles to when switched off.
 * @property {number} ledCount Active LEDs in the chain.
 * @property {number} buttonSound Sound id the long-press plays.
 */

/**
 * An open (or openable) connection to one clock.
 *
 * Events: 'open', 'close' (detail: Error), 'frame' (detail: {cmd, payload}).
 * Listen with addEventListener and read `detail` off the CustomEvent.
 */
export class HoppyClock extends EventTarget {
  #port = null;
  #reader = null;
  #writer = null;
  #parser = new FrameParser();
  #pending = null; // {cmd, resolve, reject, timer}.
  #queue = Promise.resolve(); // Serialises transactions.
  #onSerialDisconnect = null;

  /** True once a port is open and commands can be sent. */
  get connected() {
    return this.#port !== null;
  }

  /**
   * Prompt for a port and open it.
   *
   * Only ports matching the STM32 CDC VID/PID are offered. The clock presents
   * as detached while deep-asleep, so it must be woken with a button press
   * before it appears in the picker at all.
   *
   * @param {SerialPort} [port] An already-granted port to reuse, skipping the
   *   picker.
   * @returns {Promise<void>}
   */
  async connect(port) {
    if (this.#port) {
      return;
    }
    if (!isSupported()) {
      throw new DeviceError('this browser does not support Web Serial');
    }

    const target = port ?? (await navigator.serial.requestPort({filters: [USB_FILTER]}));
    await target.open({baudRate: BAUD_RATE});

    this.#port = target;
    this.#parser.reset();
    this.#writer = target.writable.getWriter();

    // Fires when the board physically drops off the bus: unplugged, or gone
    // back to sleep and detached.
    this.#onSerialDisconnect = (event) => {
      if (event.target === this.#port) {
        this.#teardown(new DeviceError('device disconnected'));
      }
    };
    navigator.serial.addEventListener('disconnect', this.#onSerialDisconnect);

    this.#readLoop();
    this.dispatchEvent(new CustomEvent('open'));
  }

  /**
   * Close the port and drop any in-flight command.
   *
   * @returns {Promise<void>}
   */
  async disconnect() {
    const port = this.#port;
    if (!port) {
      return;
    }

    const reader = this.#reader;
    this.#teardown(new DeviceError('disconnected'));

    try {
      await reader?.cancel();
    } catch {
      // Already gone; closing below is what matters.
    }
    try {
      await port.close();
    } catch {
      // The port may already be closed if the board was unplugged.
    }
  }

  /**
   * Send a command and await its response.
   *
   * Transactions are serialised, so overlapping callers queue rather than
   * interleave frames on the wire.
   *
   * @param {number} cmd Command ID.
   * @param {Uint8Array|number[]} [payload] Request payload.
   * @param {number} [timeoutMs] How long to wait for the response.
   * @returns {Promise<Uint8Array>} Response payload, status byte included.
   */
  txn(cmd, payload = [], timeoutMs = TXN_TIMEOUT_MS) {
    const run = () => this.#txnNow(cmd, payload, timeoutMs);
    // Chain onto the queue either way, but keep this caller's own outcome.
    const result = this.#queue.then(run, run);
    this.#queue = result.catch(() => {
    });
    return result;
  }

  /**
   * Send a command and require an OK status.
   *
   * @param {number} cmd Command ID.
   * @param {Uint8Array|number[]} [payload] Request payload.
   * @param {number} [timeoutMs] How long to wait for the response.
   * @returns {Promise<Uint8Array>} Response data after the status byte.
   */
  async command(cmd, payload = [], timeoutMs = TXN_TIMEOUT_MS) {
    const response = await this.txn(cmd, payload, timeoutMs);
    if (response.length < 1) {
      throw new DeviceError(`${cmdName(cmd)}: empty response`);
    }
    if (response[0] !== STATUS_OK) {
      throw new DeviceError(`${cmdName(cmd)}: device reported an error`, response[0],);
    }
    return response.subarray(1);
  }

  /**
   * Check the device is alive and speaking the protocol.
   *
   * @returns {Promise<void>}
   */
  async ping() {
    await this.command(CMD.PING);
  }

  /**
   * Read the RTC.
   *
   * @returns {Promise<Date>} The device's wall-clock time, as a local Date.
   */
  async getTime() {
    const data = await this.command(CMD.GET_TIME);
    if (data.length < 7) {
      throw new DeviceError('GET_TIME: short response');
    }
    const [yy, month, day, , hours, minutes, seconds] = data;
    return new Date(2000 + yy, month - 1, day, hours, minutes, seconds);
  }

  /**
   * Write the RTC.
   *
   * @param {Date} when The time to set, read in host local time.
   * @returns {Promise<void>}
   */
  async setTime(when) {
    // The firmware wants an ISO weekday (Mon=1 .. Sun=7); JS counts Sun=0.
    const weekday = when.getDay() === 0 ? 7 : when.getDay();
    await this.command(CMD.SET_TIME, [when.getFullYear() % 100, when.getMonth() + 1, when.getDate(), weekday, when.getHours(), when.getMinutes(), when.getSeconds(),]);
  }

  /**
   * Sync the RTC to this computer, then read it back.
   *
   * @returns {Promise<Date>} The device time as read back after the write.
   */
  async syncTime() {
    await awaitSecondBoundary();
    await this.setTime(new Date());
    return this.getTime();
  }

  /**
   * Read the whole stored config: alarms, light looks and the odds and ends.
   *
   * Light looks come back as raw records. The page does not edit them yet, but
   * writeConfig has to send them back untouched, so they are carried rather
   * than decoded.
   *
   * @returns {Promise<Config>} The config as the clock currently holds it.
   */
  async readConfig() {
    const counts = await this.command(CMD.CFG_GET_COUNT);
    if (counts.length < 6) {
      throw new DeviceError('CFG_GET_COUNT: short response');
    }
    const [alarmCount, lightCount, lampOn, lampOff, ledCount, buttonSound] = counts;

    // Alarm and light records are both 12 packed bytes.
    const read = async (cmd, count) => {
      const records = [];
      for (let i = 0; i < count; i++) {
        const data = await this.command(cmd, [i]);
        if (data.length < ALARM_BYTES) {
          throw new DeviceError(`${cmdName(cmd)} ${i}: short response`);
        }
        // Copy: the response payload is a view onto the parser's frame.
        records.push(data.slice(0, ALARM_BYTES));
      }
      return records;
    };

    return {
      alarms: await read(CMD.CFG_GET_ALARM, alarmCount),
      lights: await read(CMD.CFG_GET_LIGHT, lightCount),
      lampOn,
      lampOff,
      ledCount,
      buttonSound,
    };
  }

  /**
   * Push a whole config back and commit it.
   *
   * The manifest is one atomic image: CFG_BEGIN clears the staging area and
   * CFG_COMMIT writes whatever is in it, so anything not resent here is lost.
   * Always build the argument by editing a readConfig() result rather than
   * assembling one from scratch.
   *
   * @param {Config} config The complete config to store.
   * @returns {Promise<void>}
   */
  async writeConfig(config) {
    await this.command(CMD.CFG_BEGIN);
    for (const [i, record] of config.alarms.entries()) {
      await this.command(CMD.CFG_SET_ALARM, [i, ...record]);
    }
    for (const [i, look] of config.lights.entries()) {
      await this.command(CMD.CFG_SET_LIGHT, [i, ...look]);
    }
    await this.command(CMD.CFG_SET_LAMP, [config.lampOn, config.lampOff]);
    await this.command(CMD.CFG_SET_LEDS, [config.ledCount]);
    await this.command(CMD.CFG_SET_BTN, [config.buttonSound]);
    await this.command(CMD.CFG_COMMIT, [config.alarms.length, config.lights.length], COMMIT_TIMEOUT_MS,);
  }

  /**
   * Append one alarm, leaving every other setting as it was.
   *
   * @param {Uint8Array} record A packed record from encodeAlarm().
   * @returns {Promise<Uint8Array[]>} The stored alarm records afterwards.
   */
  async addAlarm(record) {
    const config = await this.readConfig();
    if (config.alarms.length >= MAX_ALARMS) {
      throw new DeviceError(`the clock holds at most ${MAX_ALARMS} alarms`);
    }
    config.alarms.push(record);
    await this.writeConfig(config);
    return config.alarms;
  }

  /**
   * Delete every alarm, leaving the rest of the manifest alone.
   *
   * @returns {Promise<Uint8Array[]>} The empty alarm table.
   */
  async clearAlarms() {
    const config = await this.readConfig();
    config.alarms = [];
    await this.writeConfig(config);
    return config.alarms;
  }

  /**
   * Delete the alarm at one index, closing the gap behind it.
   *
   * @param {number} index Position in the stored table.
   * @returns {Promise<Uint8Array[]>} The stored alarm records afterwards.
   */
  async removeAlarm(index) {
    const config = await this.readConfig();
    if (!Number.isInteger(index) || index < 0 || index >= config.alarms.length) {
      throw new DeviceError(`no alarm at index ${index}`);
    }
    config.alarms.splice(index, 1);
    await this.writeConfig(config);
    return config.alarms;
  }

  /**
   * Store one light look at an id, leaving every other setting as it was.
   *
   * Ids are positions in the table, so setting one past the end grows it with
   * blank looks, the same way the Python tool does.
   *
   * @param {number} id Light id, 0..MAX_LIGHTS - 1.
   * @param {Uint8Array} record A packed look from encodeLight().
   * @returns {Promise<Uint8Array[]>} The stored light records afterwards.
   */
  async saveLight(id, record) {
    if (!Number.isInteger(id) || id < 0 || id >= MAX_LIGHTS) {
      throw new DeviceError(`light id must be 0-${MAX_LIGHTS - 1}, got ${id}`);
    }
    const config = await this.readConfig();
    while (config.lights.length <= id) {
      config.lights.push(new Uint8Array(record.length));
    }
    config.lights[id] = record;
    await this.writeConfig(config);
    return config.lights;
  }

  /**
   * Point the lamp's two idle states at light ids.
   *
   * These are what the button plays: the short press toggles between them, so
   * "off" can settle on a dim ambient rather than going fully dark.
   *
   * @param {number} onId Light id played when the lamp switches on.
   * @param {number} offId Light id the lamp settles to when switched off.
   * @returns {Promise<void>}
   */
  async setLamp(onId, offId) {
    for (const [name, id] of [['on', onId], ['off', offId]]) {
      if (!Number.isInteger(id) || id < 0 || id >= MAX_LIGHTS) {
        throw new DeviceError(`lamp ${name} id must be 0-${MAX_LIGHTS - 1}, got ${id}`,);
      }
    }
    const config = await this.readConfig();
    config.lampOn = onId;
    config.lampOff = offId;
    await this.writeConfig(config);
  }

  /**
   * Light one LED a colour right now.
   *
   * Transient and unstored: it writes straight to the strip, so the next light
   * look, alarm or lamp press paints over it. The firmware refuses an index at
   * or past the active chain length.
   *
   * @param {number} index Position in the chain, from 0.
   * @param {number} r Red, 0..255.
   * @param {number} g Green, 0..255.
   * @param {number} b Blue, 0..255.
   * @returns {Promise<void>}
   */
  async setLed(index, r, g, b) {
    const fields = [['index', index], ['red', r], ['green', g], ['blue', b],];
    for (const [name, value] of fields) {
      if (!Number.isInteger(value) || value < 0 || value > 255) {
        throw new DeviceError(`${name} must be 0-255, got ${value}`);
      }
    }
    await this.command(CMD.SET_LED, [index, r, g, b]);
  }

  /**
   * Set how many LEDs in the chain the firmware drives.
   *
   * Looks are rendered across exactly this many, so it is the strip length,
   * not a brightness or power setting.
   *
   * @param {number} count Active LEDs, 1..MAX_LEDS.
   * @returns {Promise<void>}
   */
  async setLedCount(count) {
    if (!Number.isInteger(count) || count < 1 || count > MAX_LEDS) {
      throw new DeviceError(`LED count must be 1-${MAX_LEDS}, got ${count}`,);
    }
    const config = await this.readConfig();
    config.ledCount = count;
    await this.writeConfig(config);
  }

  /**
   * Read what one sound slot holds.
   *
   * An empty slot is not a failure, so this reads the status byte itself
   * rather than letting command() throw on it.
   *
   * @param {number} id Slot id, 0..MAX_SOUNDS - 1.
   * @returns {Promise<Sound|null>} The entry, or null if the slot is empty.
   */
  async soundInfo(id) {
    if (!Number.isInteger(id) || id < 0 || id >= MAX_SOUNDS) {
      throw new DeviceError(`sound id must be 0-${MAX_SOUNDS - 1}, got ${id}`);
    }
    const response = await this.txn(CMD.SND_INFO, [id]);
    if (response.length < 12 || response[0] !== STATUS_OK) {
      return null;
    }
    return decodeSound(response.subarray(1));
  }

  /**
   * Play a stored sound now.
   *
   * @param {number} id Slot id.
   * @param {number} [fadeS] Fade the volume in over this many seconds.
   * @returns {Promise<void>}
   */
  async playSound(id, fadeS = 0) {
    if (!Number.isInteger(id) || id < 0 || id >= MAX_SOUNDS) {
      throw new DeviceError(`sound id must be 0-${MAX_SOUNDS - 1}, got ${id}`);
    }
    // The fade byte is optional in the protocol; send it only when asked for.
    await this.command(CMD.SND_PLAY, fadeS ? [id, fadeS] : [id]);
  }

  /**
   * Stop whatever is playing.
   *
   * @returns {Promise<void>}
   */
  async stopSound() {
    await this.command(CMD.SND_STOP);
  }

  /**
   * Choose the sound a long press plays.
   *
   * Any id with nothing stored in it means the long press plays nothing, so
   * this deliberately allows ids past the slot count.
   *
   * @param {number} id Sound id, 0..255.
   * @returns {Promise<void>}
   */
  async setButtonSound(id) {
    if (!Number.isInteger(id) || id < 0 || id > 255) {
      throw new DeviceError(`button song id must be 0-255, got ${id}`);
    }
    const config = await this.readConfig();
    config.buttonSound = id;
    await this.writeConfig(config);
  }

  /**
   * Stream a PCM blob into a sound slot.
   *
   * SND_BEGIN erases the slot before it answers, in 64 KB blocks at roughly
   * 2 s each, so its budget is scaled to the size rather than the usual one
   * second. The blob then goes out in CHUNK_BYTES frames, each acknowledged,
   * and SND_END commits it once the firmware agrees on the CRC.
   *
   * Cancelling stops sending, which leaves the slot erased and uncommitted:
   * the firmware only publishes an entry when SND_END verifies it.
   *
   * @param {number} id Slot id.
   * @param {{format: string, rateHz: number, data: Uint8Array}} sound The blob
   *   and how to describe it to the firmware.
   * @param {{onProgress?: (sent: number, total: number) => void,
   *          signal?: AbortSignal}} [options] Progress and cancellation.
   * @returns {Promise<void>}
   */
  async uploadSound(id, sound, options = {}) {
    const {onProgress, signal} = options;
    const {data, rateHz} = sound;
    const format = FORMATS.indexOf(sound.format);

    if (!Number.isInteger(id) || id < 0 || id >= MAX_SOUNDS) {
      throw new DeviceError(`sound id must be 0-${MAX_SOUNDS - 1}, got ${id}`);
    }
    if (format < 0) {
      throw new DeviceError(`unknown sound format '${sound.format}'`);
    }
    if (!Number.isInteger(rateHz) || rateHz < 1 || rateHz > 65535) {
      throw new DeviceError(`sample rate must be 1-65535, got ${rateHz}`);
    }
    if (data.length < 1 || data.length > SLOT_BYTES) {
      throw new DeviceError(`sound is ${data.length} B; a slot holds ${SLOT_BYTES} B`,);
    }

    const total = data.length;
    const blocks = Math.ceil(total / 65536);
    await this.command(CMD.SND_BEGIN, [id, format, rateHz & 0xff, (rateHz >> 8) & 0xff, total & 0xff, (total >> 8) & 0xff, (total >> 16) & 0xff, (total >>> 24) & 0xff,], Math.max(10000, blocks * 2500),);

    // Aim for about a hundred updates whatever the size: reporting every
    // chunk would be thousands of repaints, and a fixed byte interval never
    // fires at all for a short sound.
    const chunks = Math.ceil(total / CHUNK_BYTES);
    const reportEvery = Math.max(1, Math.floor(chunks / 100));
    let done = 0;

    for (let sent = 0; sent < total; sent += CHUNK_BYTES) {
      if (signal?.aborted) {
        throw new DeviceError('upload cancelled');
      }
      const end = Math.min(sent + CHUNK_BYTES, total);
      await this.command(CMD.SND_DATA, data.subarray(sent, end));
      if (onProgress && ++done % reportEvery === 0) {
        onProgress(end, total);
      }
    }
    onProgress?.(total, total);

    const crc = crc32(data);
    await this.command(CMD.SND_END, [crc & 0xff, (crc >> 8) & 0xff, (crc >> 16) & 0xff, (crc >>> 24) & 0xff], 5000,);
  }

  /**
   * Factory-reset the flash.
   *
   * Clears the manifest and the sound index, and the firmware re-applies the
   * blank defaults live. Firmware and the running clock are untouched.
   *
   * @param {boolean} [full] Also scrub the audio data region, which erases
   *   roughly 15 MB and takes minutes rather than seconds.
   * @returns {Promise<void>}
   */
  async wipe(full = false) {
    await this.command(CMD.WIPE, full ? [1] : [], full ? WIPE_FULL_TIMEOUT_MS : WIPE_TIMEOUT_MS,);
  }

  /** Send one command, now that the queue has granted us the wire. */
  #txnNow(cmd, payload, timeoutMs = TXN_TIMEOUT_MS) {
    if (!this.#port || !this.#writer) {
      return Promise.reject(new DeviceError('not connected'));
    }

    const frame = buildFrame(cmd, payload);

    return new Promise((resolve, reject) => {
      this.#pending = {
        cmd, resolve, reject, timer: setTimeout(() => {
          this.#pending = null;
          reject(new DeviceError(`${cmdName(cmd)}: timed out`));
        }, timeoutMs),
      };

      this.#writer.write(frame).catch((err) => {
        this.#settlePending((p) => p.reject(err));
      });
    });
  }

  /** Pump the port into the parser until it closes or errors. */
  async #readLoop() {
    while (this.#port?.readable) {
      const reader = this.#port.readable.getReader();
      this.#reader = reader;
      try {
        for (; ;) {
          const {value, done} = await reader.read();
          if (done) {
            return;
          }
          for (const frame of this.#parser.push(value)) {
            this.#onFrame(frame);
          }
        }
      } catch (err) {
        this.#teardown(err);
        return;
      } finally {
        reader.releaseLock();
        this.#reader = null;
      }
    }
  }

  /** Route one decoded frame to the waiting caller. */
  #onFrame(frame) {
    this.dispatchEvent(new CustomEvent('frame', {detail: frame}));

    const pending = this.#pending;
    if (!pending) {
      return; // Late reply to an already-timed-out command; ignore it.
    }
    if (frame.cmd !== pending.cmd) {
      this.#settlePending((p) => p.reject(new DeviceError(`expected ${cmdName(p.cmd)}, got ${cmdName(frame.cmd)}`,),),);
      return;
    }
    this.#settlePending((p) => p.resolve(frame.payload));
  }

  /** Clear the pending slot and hand it to `settle` exactly once. */
  #settlePending(settle) {
    const pending = this.#pending;
    if (!pending) {
      return;
    }
    this.#pending = null;
    clearTimeout(pending.timer);
    settle(pending);
  }

  /** Drop all connection state and announce the close. */
  #teardown(reason) {
    if (!this.#port) {
      return;
    }

    navigator.serial.removeEventListener('disconnect', this.#onSerialDisconnect,);
    this.#onSerialDisconnect = null;

    this.#settlePending((p) => p.reject(reason));

    try {
      this.#writer?.releaseLock();
    } catch {
      // Nothing to release if the stream already errored.
    }
    this.#writer = null;
    this.#port = null;
    this.#parser.reset();

    this.dispatchEvent(new CustomEvent('close', {detail: reason}));
  }
}
