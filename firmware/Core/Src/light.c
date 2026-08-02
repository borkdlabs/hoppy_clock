/*******************************************************************************
 * @file light.c
 * @brief LED strip owner: renders parametric light_seq_t effects + lamp idle.
 *******************************************************************************
 * @note:
 * Single owner of the WS2812B LED. Everything that lights the LED (the lamp
 * button, alarm rings) funnels through here so nothing fights over it.
 *
 * light_play() starts a timed transition from the LED's current colour to a
 * sequence's target, then holds that target as the idle. light_task() renders
 * it (interpolation + optional flicker) and pushes changes to the LED.
 *
 * The lamp is a two-state idle toggled by the button: light_lamp_toggle() plays
 * the manifest's lamp-on / lamp-off look (falling back to built-ins if none is
 * programmed). After an alarm ring the runtime calls light_lamp_reapply() to
 * return the LED to the current lamp idle.
 *******************************************************************************
 */

/** Includes. *****************************************************************/

#include "light.h"
#include "ws2812b_hal_pwm.h"
#include <stdint.h>

/** Definitions. **************************************************************/

// Flicker cadence (SOLID + FLICKER): a new random dip this often (ms), so the
// wobble reads as a slow flicker rather than fast noise.
#define LIGHT_FLICKER_PERIOD_MS 120u

// Built-in fallbacks used when the manifest has no lamp look programmed.
static const light_seq_t LAMP_ON_DEFAULT = {.effect = LIGHT_FX_SOLID,
                                            .r = 255,
                                            .g = 200,
                                            .b = 120,
                                            .brightness = 160,
                                            .period_ms = 1500,
                                            .curve = LIGHT_CURVE_FLICKER,
                                            .spread = 40};
static const light_seq_t LAMP_OFF_DEFAULT = {.effect = LIGHT_FX_SOLID,
                                             .r = 0,
                                             .g = 0,
                                             .b = 0,
                                             .brightness = 0,
                                             .period_ms = 1500,
                                             .curve = LIGHT_CURVE_LINEAR,
                                             .spread = 0};

/** Private variables. ********************************************************/

// Colours currently rendered on the strip (start point for the next SOLID).
// Sized to the maximum; only the active ws2812b_get_count() are rendered.
static uint8_t s_cur_r[LED_COUNT_MAX], s_cur_g[LED_COUNT_MAX];
static uint8_t s_cur_b[LED_COUNT_MAX];

// SOLID transition: per-LED start colours, and the uniform target.
static uint8_t s_from_r[LED_COUNT_MAX], s_from_g[LED_COUNT_MAX];
static uint8_t s_from_b[LED_COUNT_MAX];
static uint8_t s_to_r, s_to_g, s_to_b;

// Active look.
static light_seq_t s_seq;
static uint32_t s_start_ms;
static bool s_active; // Rendering (a SOLID transition or a looping effect).

// Flicker state (SOLID + FLICKER).
static uint32_t s_flick_ms;
static uint8_t s_flick;
static uint32_t s_rng = 0x2545F491u;

// Lamp on/off state.
static bool s_lamp_on = false;

/** Private functions. ********************************************************/

static uint8_t lerp8(uint8_t a, uint8_t b, float p) {
  return (uint8_t)((float)a + ((float)b - (float)a) * p + 0.5f);
}

static uint8_t scale8(uint8_t v, float s) {
  return (uint8_t)((float)v * s + 0.5f);
}

/**
 * @brief Fractional part of a non-negative float (no libm dependency).
 */
static float fracf(float x) { return x - (float)(uint32_t)x; }

/**
 * @brief HSV (all in [0,1], hue wraps) to 8-bit RGB.
 */
static void hsv_to_rgb(float h, float s, float v, uint8_t *r, uint8_t *g,
                       uint8_t *b) {
  h = fracf(h) * 6.0f;
  const int i = (int)h;
  const float f = h - (float)i;
  const float p = v * (1.0f - s);
  const float q = v * (1.0f - s * f);
  const float t = v * (1.0f - s * (1.0f - f));
  float rf, gf, bf;
  switch (i) {
  case 0:
    rf = v, gf = t, bf = p;
    break;
  case 1:
    rf = q, gf = v, bf = p;
    break;
  case 2:
    rf = p, gf = v, bf = t;
    break;
  case 3:
    rf = p, gf = q, bf = v;
    break;
  case 4:
    rf = t, gf = p, bf = v;
    break;
  default:
    rf = v, gf = p, bf = q;
    break;
  }
  *r = (uint8_t)(rf * 255.0f + 0.5f);
  *g = (uint8_t)(gf * 255.0f + 0.5f);
  *b = (uint8_t)(bf * 255.0f + 0.5f);
}

/**
 * @brief Push the current colour buffer to the strip.
 */
static void strip_write(void) {
  for (uint16_t i = 0u; i < ws2812b_get_count(); i++) {
    ws2812b_set_colour((uint8_t)i, s_cur_r[i], s_cur_g[i], s_cur_b[i]);
  }
  ws2812b_update();
}

/**
 * @brief Fractional animation phase in [0,1) for a looping effect.
 */
static float phase_of(uint32_t el) {
  const float period =
      (s_seq.period_ms == 0u) ? 1000.0f : (float)s_seq.period_ms;
  return fracf((float)el / period);
}

static void render_solid(uint32_t el) {
  float t;
  if (s_seq.period_ms == 0u || el >= s_seq.period_ms) {
    t = 1.0f;
  } else {
    t = (float)el / (float)s_seq.period_ms;
  }
  const float p =
      (s_seq.curve == LIGHT_CURVE_EASE) ? (t * t * (3.0f - 2.0f * t)) : t;

  // Slow random flicker dip while transitioning.
  uint16_t keep = 255u;
  if (s_seq.curve == LIGHT_CURVE_FLICKER && s_seq.spread != 0u && t < 1.0f) {
    const uint32_t now = HAL_GetTick();
    if (now - s_flick_ms >= LIGHT_FLICKER_PERIOD_MS) {
      s_flick_ms = now;
      s_rng = s_rng * 1664525u + 1013904223u;
      s_flick = (uint8_t)(((s_rng >> 24) & 0xFFu) * s_seq.spread / 255u);
    }
    keep = (uint16_t)(255u - s_flick);
  }

  for (uint16_t i = 0u; i < ws2812b_get_count(); i++) {
    const uint8_t r = lerp8(s_from_r[i], s_to_r, p);
    const uint8_t g = lerp8(s_from_g[i], s_to_g, p);
    const uint8_t b = lerp8(s_from_b[i], s_to_b, p);
    s_cur_r[i] = (uint8_t)((uint16_t)r * keep / 255u);
    s_cur_g[i] = (uint8_t)((uint16_t)g * keep / 255u);
    s_cur_b[i] = (uint8_t)((uint16_t)b * keep / 255u);
  }

  if (t >= 1.0f) {
    for (uint16_t i = 0u; i < ws2812b_get_count(); i++) {
      s_cur_r[i] = s_to_r;
      s_cur_g[i] = s_to_g;
      s_cur_b[i] = s_to_b;
    }
    s_active = false; // Settle on the target and hold.
  }
}

static void render_rainbow(uint32_t el) {
  const float phase = phase_of(el);
  const float v = (float)s_seq.brightness / 255.0f;
  const float step = (float)s_seq.spread / 255.0f; // Hue turn per LED.
  for (uint16_t i = 0u; i < ws2812b_get_count(); i++) {
    hsv_to_rgb(phase + step * (float)i, 1.0f, v, &s_cur_r[i], &s_cur_g[i],
               &s_cur_b[i]);
  }
}

static void render_breathe(uint32_t el) {
  const float phase = phase_of(el);
  // Smooth 0->1->0 breathe: a triangle wave shaped by smoothstep (no libm).
  const float tri = (phase < 0.5f) ? (phase * 2.0f) : ((1.0f - phase) * 2.0f);
  const float lvl = tri * tri * (3.0f - 2.0f * tri);
  const float s = lvl * (float)s_seq.brightness / 255.0f;
  const uint8_t r = scale8(s_seq.r, s);
  const uint8_t g = scale8(s_seq.g, s);
  const uint8_t b = scale8(s_seq.b, s);
  for (uint16_t i = 0u; i < ws2812b_get_count(); i++) {
    s_cur_r[i] = r;
    s_cur_g[i] = g;
    s_cur_b[i] = b;
  }
}

static void render_sweep(uint32_t el) {
  const float head =
      phase_of(el) * (float)ws2812b_get_count(); // Band leading edge.
  const float width = (s_seq.spread == 0u) ? 1.0f : (float)s_seq.spread;
  const float br = (float)s_seq.brightness / 255.0f;
  for (uint16_t i = 0u; i < ws2812b_get_count(); i++) {
    float d = (float)i - head; // Distance behind the head (wrapped).
    while (d < 0.0f) {
      d += (float)ws2812b_get_count();
    }
    if (d < width) {
      const float f = br * (1.0f - d / width); // Fade along the tail.
      s_cur_r[i] = scale8(s_seq.r, f);
      s_cur_g[i] = scale8(s_seq.g, f);
      s_cur_b[i] = scale8(s_seq.b, f);
    } else {
      s_cur_r[i] = 0u;
      s_cur_g[i] = 0u;
      s_cur_b[i] = 0u;
    }
  }
}

/**
 * @brief Play the manifest lamp look for the current state (or a fallback).
 */
static void lamp_apply(void) {
  const manifest_t *m = manifest_get();
  uint8_t id = s_lamp_on ? m->header.lamp_on_light : m->header.lamp_off_light;
  if (id < m->header.light_count) {
    light_play(&m->lights[id]);
  } else {
    light_play(s_lamp_on ? &LAMP_ON_DEFAULT : &LAMP_OFF_DEFAULT);
  }
}

/** Public functions. *********************************************************/

void light_init(void) {
  for (uint16_t i = 0u; i < ws2812b_get_count(); i++) {
    s_cur_r[i] = s_cur_g[i] = s_cur_b[i] = 0u;
  }
  s_active = false;
  s_flick = 0u;
  s_lamp_on = false;
  strip_write();
}

void light_play(const light_seq_t *seq) {
  s_seq = *seq;
  s_start_ms = HAL_GetTick();
  s_flick_ms = s_start_ms;
  s_flick = 0u;

  if (s_seq.effect == LIGHT_FX_SOLID) {
    // Capture the current strip as the start and the uniform scaled target.
    for (uint16_t i = 0u; i < ws2812b_get_count(); i++) {
      s_from_r[i] = s_cur_r[i];
      s_from_g[i] = s_cur_g[i];
      s_from_b[i] = s_cur_b[i];
    }
    s_to_r = (uint8_t)((uint16_t)s_seq.r * s_seq.brightness / 255u);
    s_to_g = (uint8_t)((uint16_t)s_seq.g * s_seq.brightness / 255u);
    s_to_b = (uint8_t)((uint16_t)s_seq.b * s_seq.brightness / 255u);
  }
  s_active = true;
}

void light_task(void) {
  if (!s_active) {
    return; // Idle: holding a settled SOLID target.
  }

  const uint32_t el = HAL_GetTick() - s_start_ms;
  switch (s_seq.effect) {
  case LIGHT_FX_RAINBOW:
    render_rainbow(el);
    break;
  case LIGHT_FX_SWEEP:
    render_sweep(el);
    break;
  case LIGHT_FX_BREATHE:
    render_breathe(el);
    break;
  default:
    render_solid(el);
    break;
  }
  strip_write();
}

void light_lamp_toggle(void) {
  s_lamp_on = !s_lamp_on;
  lamp_apply();
}

void light_lamp_reapply(void) { lamp_apply(); }

bool light_lamp_is_on(void) { return s_lamp_on; }
