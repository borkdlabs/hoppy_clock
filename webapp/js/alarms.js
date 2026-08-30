/**
 * Alarm records, mirroring alarm_record_t in firmware/Core/Inc/manifest.h.
 *
 * One alarm is 12 packed bytes, the same layout on the wire and on flash:
 *
 *   flags | day_sel | hour | minute | second | timeout_s (u16 LE) |
 *   sound_id | light_id | sound_fade_s | reserved[2]
 *
 * day_sel carries two different things depending on the MONTHLY flag: a
 * weekday bit mask (bit 0 = Monday) in weekly mode, or a day-of-month 1..31
 * in monthly mode.
 */

/** Size of one packed record. */
export const ALARM_BYTES = 12;

/** MANIFEST_MAX_ALARMS: how many the firmware will store. */
export const MAX_ALARMS = 64;

/** alarm_record_t.flags bits. */
export const FLAG_ENABLED = 1 << 0;
export const FLAG_MONTHLY = 1 << 1;

/** Weekday labels by day_sel bit position, Monday first. */
export const DAY_NAMES = ['Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat', 'Sun'];

/** Masks worth naming rather than spelling out day by day. */
const DAY_GROUPS = [[0b1111111, 'Every day'], [0b0011111, 'Weekdays'], [0b1100000, 'Weekends'],];

/**
 * @typedef {object} Alarm
 * @property {boolean} enabled Whether the firmware may fire it.
 * @property {boolean} monthly True if daySel is a day-of-month.
 * @property {number} daySel Weekday mask, or day-of-month 1..31.
 * @property {number} hour 0..23.
 * @property {number} minute 0..59.
 * @property {number} second 0..59.
 * @property {number} timeoutS Auto-quiet after this long; 0 = manual only.
 * @property {number} soundId Sound slot to play.
 * @property {number} lightId Light look to play.
 * @property {number} fadeS Sound fade-in seconds; 0 = none.
 */

/**
 * Decode a packed record.
 *
 * @param {Uint8Array} bytes The 12 record bytes.
 * @returns {Alarm} The decoded alarm.
 * @throws {RangeError} If fewer than ALARM_BYTES bytes are given.
 */
export function decodeAlarm(bytes) {
  if (bytes.length < ALARM_BYTES) {
    throw new RangeError(`alarm record is ${bytes.length} B, need 12 B`);
  }
  const view = new DataView(bytes.buffer, bytes.byteOffset, ALARM_BYTES);
  return {
    enabled: (bytes[0] & FLAG_ENABLED) !== 0,
    monthly: (bytes[0] & FLAG_MONTHLY) !== 0,
    daySel: bytes[1],
    hour: bytes[2],
    minute: bytes[3],
    second: bytes[4],
    timeoutS: view.getUint16(5, true),
    soundId: bytes[7],
    lightId: bytes[8],
    fadeS: bytes[9],
  };
}

/**
 * Encode an alarm into its packed record.
 *
 * Reserved bytes are left zero so the flash image stays deterministic and its
 * CRC stable, the way the firmware writes it.
 *
 * @param {Alarm} alarm The alarm to encode.
 * @returns {Uint8Array} The 12 record bytes.
 * @throws {RangeError} If any field is outside what the record can hold.
 */
export function encodeAlarm(alarm) {
  const fit = (name, value, max) => {
    if (!Number.isInteger(value) || value < 0 || value > max) {
      throw new RangeError(`${name} must be an integer 0-${max}, got ${value}`);
    }
    return value;
  };

  const bytes = new Uint8Array(ALARM_BYTES);
  const view = new DataView(bytes.buffer);

  bytes[0] = (alarm.enabled ? FLAG_ENABLED : 0) | (alarm.monthly ? FLAG_MONTHLY : 0);
  bytes[1] = fit('day', alarm.daySel, 255);
  bytes[2] = fit('hour', alarm.hour, 23);
  bytes[3] = fit('minute', alarm.minute, 59);
  bytes[4] = fit('second', alarm.second, 59);
  view.setUint16(5, fit('timeout', alarm.timeoutS, 65535), true);
  bytes[7] = fit('sound id', alarm.soundId, 255);
  bytes[8] = fit('light id', alarm.lightId, 255);
  bytes[9] = fit('fade', alarm.fadeS, 255);

  if (alarm.monthly && (alarm.daySel < 1 || alarm.daySel > 31)) {
    throw new RangeError(`day of month must be 1-31, got ${alarm.daySel}`);
  }
  if (!alarm.monthly && alarm.daySel === 0) {
    throw new RangeError('a weekly alarm needs at least one day');
  }
  return bytes;
}

/**
 * Format the fire time, hiding seconds when they are zero.
 *
 * @param {Alarm} alarm The alarm to describe.
 * @returns {string} e.g. "08:00" or "08:00:30".
 */
export function formatAlarmTime(alarm) {
  const pad = (n) => String(n).padStart(2, '0');
  const hhmm = `${pad(alarm.hour)}:${pad(alarm.minute)}`;
  return alarm.second ? `${hhmm}:${pad(alarm.second)}` : hhmm;
}

/**
 * Describe when an alarm repeats.
 *
 * @param {Alarm} alarm The alarm to describe.
 * @returns {string} e.g. "Weekdays", "Mon, Thu" or "Day 1 of each month".
 */
export function describeDays(alarm) {
  if (alarm.monthly) {
    return `Day ${alarm.daySel} of each month`;
  }
  const named = DAY_GROUPS.find(([mask]) => mask === alarm.daySel);
  if (named) {
    return named[1];
  }
  const days = DAY_NAMES.filter((_, bit) => alarm.daySel & (1 << bit));
  return days.length ? days.join(', ') : 'No days set';
}
