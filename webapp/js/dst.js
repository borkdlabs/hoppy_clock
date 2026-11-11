/**
 * Daylight saving rule for the clock, mirroring firmware/Core/Inc/dst.h.
 *
 * The clock keeps local time and moves itself an hour twice a year on a rule it
 * is handed along with the time: each changeover as "the Nth <weekday> of
 * <month> at <hour>". The browser does not expose its zone's rule, only the UTC
 * offset at any instant, so the rule is recovered by finding where that offset
 * changes over the next few years and looking for the pattern.
 */

/** dst_edge_t.week value meaning "the last <weekday> of the month". */
export const WEEK_LAST = 5;

/** Bytes in a packed rule: two edges of month, week, weekday, hour. */
export const RULE_BYTES = 8;

const DAY_MS = 86400000;

/** Search resolution. Zones change on a quarter hour of UTC at the finest. */
const STEP_MS = 15 * 60000;

/**
 * Years compared to tell "the 4th" from "the last" weekday of a month. The
 * last one drifts between a 4th and a 5th occurrence within any such run.
 */
const YEARS = 6;

const MONTHS = ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September', 'October', 'November', 'December',];
const WEEKDAYS = ['Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday', 'Sunday',];
const WEEKS = ['first', 'second', 'third', 'fourth', 'last'];

/**
 * @typedef {Object} DstEdge
 * @property {number} month 1-12.
 * @property {number} week 1-4 for the Nth occurrence, WEEK_LAST for the last.
 * @property {number} weekday 1-7, Monday first.
 * @property {number} hour Local hour the clock reads as it changes.
 */

/**
 * @typedef {Object} DstRule
 * @property {DstEdge} start Clock goes forward, read in standard time.
 * @property {DstEdge} end Clock goes back, read in daylight time.
 */

/**
 * @typedef {Object} DstDetection
 * @property {DstRule|null} rule The zone's rule, or null if there is none to
 *     send.
 * @property {boolean} active Whether daylight time is in effect right now.
 * @property {'ok'|'none'|'unsupported'} kind Why rule is or is not set: the
 *     zone has a rule, has no daylight saving, or changes in a way the clock
 *     cannot follow.
 */

/**
 * Minutes this computer's zone is behind UTC at an instant.
 *
 * @param {number} ms Instant, as epoch milliseconds.
 * @returns {number} The offset, positive west of Greenwich.
 */
function offsetAt(ms) {
  return new Date(ms).getTimezoneOffset();
}

/**
 * Find where the zone's UTC offset changes during one year.
 *
 * @param {number} year Calendar year, searched in UTC.
 * @param {(ms: number) => number} offset Offset lookup, see offsetAt.
 * @returns {Array<{at: number, before: number, after: number}>} Each change:
 *     the first instant on the new offset, and the offsets either side.
 */
function transitionsIn(year, offset) {
  const found = [];
  const end = Date.UTC(year + 1, 0, 1);
  let before = offset(Date.UTC(year, 0, 1));

  for (let day = Date.UTC(year, 0, 1); day < end; day += DAY_MS) {
    const after = offset(day + DAY_MS);
    if (after === before) {
      continue;
    }
    // Somewhere in this day: walk it for the first step on the new offset.
    let at = day + STEP_MS;
    while (offset(at) === before) {
      at += STEP_MS;
    }
    found.push({at, before, after});
    before = after;
  }
  return found;
}

/**
 * Read a change as the wall clock showed it, in the offset it is leaving.
 *
 * @param {{at: number, before: number}} change From transitionsIn.
 * @returns {{month: number, date: number, weekday: number, hour: number,
 *     minute: number, isLast: boolean}} Calendar fields of that moment.
 */
function wallFields(change) {
  // Shift into the old offset, then read with the UTC getters.
  const wall = new Date(change.at - change.before * 60000);
  const month = wall.getUTCMonth() + 1;
  const date = wall.getUTCDate();
  const daysInMonth = new Date(Date.UTC(wall.getUTCFullYear(), month, 0)).getUTCDate();
  return {
    month,
    date,
    weekday: wall.getUTCDay() === 0 ? 7 : wall.getUTCDay(),
    hour: wall.getUTCHours(),
    minute: wall.getUTCMinutes(),
    isLast: date + 7 > daysInMonth,
  };
}

/**
 * Reduce one changeover, seen across several years, to a single edge.
 *
 * @param {ReturnType<typeof wallFields>[]} seen The same edge in each year.
 * @returns {DstEdge|null} The edge, or null if the years do not agree on one.
 */
function edgeFrom(seen) {
  const [first] = seen;
  const same = (key) => seen.every((s) => s[key] === first[key]);
  if (!same('month') || !same('weekday') || !same('hour') || first.minute !== 0 || !same('minute')) {
    return null;
  }

  const nth = Math.ceil(first.date / 7);
  let week;
  if (seen.every((s) => Math.ceil(s.date / 7) === nth)) {
    week = nth;
  } else if (seen.every((s) => s.isLast)) {
    week = WEEK_LAST;
  } else {
    return null; // A fixed date, or something stranger.
  }
  return {month: first.month, week, weekday: first.weekday, hour: first.hour};
}

/**
 * Work out this computer's daylight saving rule.
 *
 * @param {Date} [now] The moment to report the current state for.
 * @param {(ms: number) => number} [offset] Offset lookup, for testing.
 * @returns {DstDetection}
 */
export function detectDst(now = new Date(), offset = offsetAt) {
  const starts = [];
  const ends = [];
  let daylightOffset = null;

  for (let i = 0; i < YEARS; i++) {
    const changes = transitionsIn(now.getFullYear() + i, offset);
    if (changes.length === 0 && i === 0) {
      return {rule: null, active: false, kind: 'none'};
    }
    // The clock moves by exactly one hour, once each way a year.
    const forward = changes.filter((c) => c.before - c.after === 60);
    const back = changes.filter((c) => c.before - c.after === -60);
    if (changes.length !== 2 || forward.length !== 1 || back.length !== 1) {
      return {rule: null, active: false, kind: 'unsupported'};
    }
    starts.push(wallFields(forward[0]));
    ends.push(wallFields(back[0]));
    daylightOffset ??= forward[0].after;
  }

  const start = edgeFrom(starts);
  const end = edgeFrom(ends);
  // The hour limits are the firmware's: both jumps must stay inside one day.
  if (!start || !end || start.hour > 22 || end.hour < 1 || start.month === end.month) {
    return {rule: null, active: false, kind: 'unsupported'};
  }
  return {
    rule: {start, end},
    active: offset(now.getTime()) === daylightOffset,
    kind: 'ok',
  };
}

/**
 * Pack a rule for SET_TIME.
 *
 * @param {DstRule|null} rule The rule, or null for no automatic adjustment.
 * @returns {number[]} RULE_BYTES bytes, all zero when off.
 */
export function encodeRule(rule) {
  if (!rule) {
    return new Array(RULE_BYTES).fill(0);
  }
  const edge = (e) => [e.month, e.week, e.weekday, e.hour];
  return [...edge(rule.start), ...edge(rule.end)];
}

/**
 * Unpack a rule from a GET_TIME reply.
 *
 * @param {Uint8Array|number[]} bytes RULE_BYTES bytes.
 * @returns {DstRule|null} The rule, or null if adjustment is off.
 */
export function decodeRule(bytes) {
  if (bytes[0] === 0) {
    return null;
  }
  const edge = (at) => ({
    month: bytes[at],
    week: bytes[at + 1],
    weekday: bytes[at + 2],
    hour: bytes[at + 3],
  });
  return {start: edge(0), end: edge(4)};
}

/**
 * Put one changeover into words.
 *
 * @param {DstEdge} edge The changeover.
 * @returns {string} e.g. "the last Sunday of March at 02:00".
 */
function describeEdge(edge) {
  const hour = String(edge.hour).padStart(2, '0');
  return `the ${WEEKS[edge.week - 1]} ${WEEKDAYS[edge.weekday - 1]} of ${MONTHS[edge.month - 1]} at ${hour}:00`;
}

/**
 * Put a rule into words.
 *
 * @param {DstRule} rule The rule.
 * @returns {string} One sentence naming both changeovers.
 */
export function describeRule(rule) {
  return `forward an hour on ${describeEdge(rule.start)}, back on ${describeEdge(rule.end)}`;
}
