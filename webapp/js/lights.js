/**
 * Light looks, mirroring light_seq_t in firmware/Core/Inc/manifest.h.
 *
 * One look is 12 packed bytes, the same layout on the wire and on flash:
 *
 *   effect | r | g | b | brightness | period_ms (u16 LE) | curve | spread |
 *   fade_ms (u16 LE) | flicker
 *
 * A look is parametric rather than a frame list: the firmware renders it
 * across every LED in the chain. Alarms and the two lamp idle states just
 * reference one by id.
 *
 * `effect` picks what is rendered and gives `periodMs` and `spread` their
 * meaning. `fadeMs`, `curve` and `flicker` are modifiers that apply the same
 * way whichever effect is chosen: the look crossfades in from whatever the
 * strip already shows over `fadeMs` shaped by `curve`, and `flicker` dips its
 * brightness at random while it runs. Because the fade belongs to the look
 * being played, one setting covers both directions - fading a look in is what
 * fades the previous one out.
 */

/** Size of one packed look. */
export const LIGHT_BYTES = 12;

/** MANIFEST_MAX_LIGHTS: how many the firmware will store. */
export const MAX_LIGHTS = 16;

/**
 * LED_COUNT_MAX from ws2812b_hal_pwm.h: the longest chain the firmware will
 * drive. The active count is a runtime value of 1..this.
 */
export const MAX_LEDS = 64;

/** light_seq_t.effect values, by index. */
export const EFFECTS = ['solid', 'rainbow', 'sweep', 'breathe'];

/** light_seq_t.curve values, by index. Shapes the fade for every effect. */
export const CURVES = ['linear', 'ease'];

/**
 * What `spread` means for each effect, since the field is overloaded. Null
 * where the effect ignores it entirely.
 */
export const SPREAD_LABELS = {
  solid: null,
  rainbow: 'Hue step per LED',
  sweep: 'Band width (LEDs)',
  breathe: null,
};

/**
 * Whether an effect animates on a cycle, so `periodMs` means something to it.
 * A solid look holds still and ignores the field.
 */
export const USES_PERIOD = {
  solid: false, rainbow: true, sweep: true, breathe: true,
};

/** The blank look the firmware stores in an unused slot. */
export const BLANK_LIGHT = Object.freeze({
  effect: 'solid',
  r: 0,
  g: 0,
  b: 0,
  brightness: 0,
  periodMs: 0,
  curve: 'linear',
  spread: 0,
  fadeMs: 0,
  flicker: 0,
});

/**
 * @typedef {object} Light
 * @property {string} effect One of EFFECTS.
 * @property {number} r Base colour red, 0..255.
 * @property {number} g Base colour green, 0..255.
 * @property {number} b Base colour blue, 0..255.
 * @property {number} brightness Master level 0..255, scales the effect.
 * @property {number} periodMs Animated effects: cycle time. Solid ignores it.
 * @property {number} curve One of CURVES; the shape of the fade.
 * @property {number} spread Hue step per LED, or band width.
 * @property {number} fadeMs Crossfade into this look, 0 = snap. Any effect.
 * @property {number} flicker Random brightness dip, 0 = steady. Any effect.
 */

/**
 * Decode a packed look.
 *
 * @param {Uint8Array} bytes The 12 record bytes.
 * @returns {Light} The decoded look.
 * @throws {RangeError} If fewer than LIGHT_BYTES bytes are given.
 */
export function decodeLight(bytes) {
  if (bytes.length < LIGHT_BYTES) {
    throw new RangeError(`light record is ${bytes.length} B, need 12 B`);
  }
  const view = new DataView(bytes.buffer, bytes.byteOffset, LIGHT_BYTES);
  return {
    // An id the firmware does not know yet still has to round-trip, so fall
    // back to the raw number rather than dropping it.
    effect: EFFECTS[bytes[0]] ?? bytes[0],
    r: bytes[1],
    g: bytes[2],
    b: bytes[3],
    brightness: bytes[4],
    periodMs: view.getUint16(5, true),
    curve: CURVES[bytes[7]] ?? bytes[7],
    spread: bytes[8],
    fadeMs: view.getUint16(9, true),
    flicker: bytes[11],
  };
}

/**
 * Encode a look into its packed record.
 *
 * @param {Light} light The look to encode.
 * @returns {Uint8Array} The 12 record bytes.
 * @throws {RangeError} If any field is outside what the record can hold.
 */
export function encodeLight(light) {
  const fit = (name, value, max) => {
    if (!Number.isInteger(value) || value < 0 || value > max) {
      throw new RangeError(`${name} must be an integer 0-${max}, got ${value}`);
    }
    return value;
  };
  const index = (name, table, value) => {
    const found = table.indexOf(value);
    if (found < 0) {
      throw new RangeError(`unknown ${name} '${value}'`);
    }
    return found;
  };

  const bytes = new Uint8Array(LIGHT_BYTES);
  const view = new DataView(bytes.buffer);

  bytes[0] = index('effect', EFFECTS, light.effect);
  bytes[1] = fit('red', light.r, 255);
  bytes[2] = fit('green', light.g, 255);
  bytes[3] = fit('blue', light.b, 255);
  bytes[4] = fit('brightness', light.brightness, 255);
  view.setUint16(5, fit('period', light.periodMs, 65535), true);
  bytes[7] = index('curve', CURVES, light.curve);
  bytes[8] = fit('spread', light.spread, 255);
  view.setUint16(9, fit('fade', light.fadeMs, 65535), true);
  bytes[11] = fit('flicker', light.flicker, 255);
  return bytes;
}

/**
 * Describe how a look behaves, in the terms its effect actually uses, then the
 * modifiers it shares with every other effect.
 *
 * @param {Light} light The look to describe.
 * @returns {string} e.g. "Breathes every 3000 ms, 1500 ms ease fade".
 */
export function describeLight(light) {
  const ms = `${light.periodMs} ms`;
  let base;
  switch (light.effect) {
    case 'solid':
      base = 'Holds one colour';
      break;
    case 'rainbow':
      base = light.spread ? `Hue cycle every ${ms}, ${light.spread} per LED` : `Hue cycle every ${ms}, whole strip as one`;
      break;
    case 'sweep':
      base = `Sweeps every ${ms}, ${light.spread} LEDs wide`;
      break;
    case 'breathe':
      base = `Breathes every ${ms}`;
      break;
    default:
      base = `Effect ${light.effect}, ${ms}`;
      break;
  }

  const modifiers = [];
  if (light.fadeMs) {
    modifiers.push(`${light.fadeMs} ms ${light.curve} fade`);
  }
  if (light.flicker) {
    modifiers.push(`flickering by ${light.flicker}`);
  }
  return [base, ...modifiers].join(', ');
}

/**
 * The colour a look shows at rest, for a swatch.
 *
 * Brightness scales the base colour in the firmware, so apply it here too or a
 * dim look would preview as a bright one.
 *
 * @param {Light} light The look to sample.
 * @returns {string} A CSS colour.
 */
export function lightColor(light) {
  if (light.effect === 'rainbow') {
    // No single base colour; show the spectrum it cycles through.
    return 'conic-gradient(red, yellow, lime, cyan, blue, magenta, red)';
  }
  const scale = light.brightness / 255;
  const channel = (v) => Math.round(v * scale);
  return `rgb(${channel(light.r)}, ${channel(light.g)}, ${channel(light.b)})`;
}

/**
 * Convert a colour input's value into record fields.
 *
 * @param {string} hex A "#rrggbb" string.
 * @returns {{r: number, g: number, b: number}} The channels.
 */
export function fromHex(hex) {
  const value = Number.parseInt(hex.replace('#', ''), 16);
  return {
    r: (value >> 16) & 0xff, g: (value >> 8) & 0xff, b: value & 0xff,
  };
}

/**
 * Convert record fields into a colour input's value.
 *
 * @param {Light} light The look to read.
 * @returns {string} A "#rrggbb" string.
 */
export function toHex(light) {
  const pair = (v) => v.toString(16).padStart(2, '0');
  return `#${pair(light.r)}${pair(light.g)}${pair(light.b)}`;
}
