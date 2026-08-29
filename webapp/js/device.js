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

/** STM32 Virtual COM Port, from firmware/USB_DEVICE/App/usbd_desc.c. */
export const USB_FILTER = {usbVendorId: 0x0483, usbProductId: 0x5740};

/** CDC ignores the line rate, but match the Python tool's 115200. */
const BAUD_RATE = 115200;

/** How long to wait for a response before giving up on a command. */
const TXN_TIMEOUT_MS = 1000;

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

    const target =
      port ?? (await navigator.serial.requestPort({filters: [USB_FILTER]}));
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
   * @returns {Promise<Uint8Array>} Response payload, status byte included.
   */
  txn(cmd, payload = []) {
    const run = () => this.#txnNow(cmd, payload);
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
   * @returns {Promise<Uint8Array>} Response data after the status byte.
   */
  async command(cmd, payload = []) {
    const response = await this.txn(cmd, payload);
    if (response.length < 1) {
      throw new DeviceError(`${cmdName(cmd)}: empty response`);
    }
    if (response[0] !== STATUS_OK) {
      throw new DeviceError(
        `${cmdName(cmd)}: device reported an error`,
        response[0],
      );
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
    await this.command(CMD.SET_TIME, [
      when.getFullYear() % 100,
      when.getMonth() + 1,
      when.getDate(),
      weekday,
      when.getHours(),
      when.getMinutes(),
      when.getSeconds(),
    ]);
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

  /** Send one command, now that the queue has granted us the wire. */
  #txnNow(cmd, payload) {
    if (!this.#port || !this.#writer) {
      return Promise.reject(new DeviceError('not connected'));
    }

    const frame = buildFrame(cmd, payload);

    return new Promise((resolve, reject) => {
      this.#pending = {
        cmd,
        resolve,
        reject,
        timer: setTimeout(() => {
          this.#pending = null;
          reject(new DeviceError(`${cmdName(cmd)}: timed out`));
        }, TXN_TIMEOUT_MS),
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
      this.#settlePending((p) =>
        p.reject(
          new DeviceError(
            `expected ${cmdName(p.cmd)}, got ${cmdName(frame.cmd)}`,
          ),
        ),
      );
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

    navigator.serial.removeEventListener(
      'disconnect',
      this.#onSerialDisconnect,
    );
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
