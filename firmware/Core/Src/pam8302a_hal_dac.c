/*******************************************************************************
 * @file pam8302a_hal_dac.c
 * @brief PAM8302A functions: abstracting STM32 HAL: DAC.
 *******************************************************************************
 * @note:
 * DAC1_OUT1 (PA4) is triggered by TIM6 TRGO at the sample rate and fed by a
 * circular DMA (DMA1_Channel3). While idle the DAC is held at mid-scale via a
 * plain (non-DMA) channel start, so the amp input stays quiescent between
 * clips. Playback swaps the DMA mode at runtime: circular for looping/streaming
 * clips, normal for one-shot clips (so the transfer-complete interrupt can drop
 * cleanly back to mid-scale idle without a wrap-around click).
 *
 * The pop-free enable/disable ordering and the mid-scale glide live here, all
 * amplitude/volume shaping stays in higher layers. See the header for the
 * hardware and format contract.
 *******************************************************************************
 */

/** Includes. *****************************************************************/

#include "pam8302a_hal_dac.h"

/** Private types. ************************************************************/

typedef enum {
  AMP_STATE_UNINIT = 0, // Peripherals not orchestrated yet.
  AMP_STATE_IDLE,       // DAC at mid-scale, no DMA.
  AMP_STATE_PLAYING,    // DMA feeding the DAC (clip or stream).
} amp_state_t;

/** Private variables. ********************************************************/

// DAC DMA handle, defined by CubeMX in main.c. Needed to switch the DMA mode
// (circular vs normal) between looping/streaming and one-shot playback.
extern DMA_HandleTypeDef hdma_dac_ch1;

static volatile amp_state_t s_state = AMP_STATE_UNINIT;
static volatile bool s_enabled = false; // SD driven high.
static bool s_loop = false;             // Current clip loops (no refill).

// Streaming state: refill callback and the ring buffer it services. s_refill is
// NULL for direct (non-streaming) playback.
static amp_refill_cb_t s_refill = NULL;
static uint16_t *s_stream_buf = NULL;
static uint16_t s_stream_half = 0;

/** Private functions. ********************************************************/

// TIM6 kernel clock in Hz. APB1 timer clock is 2x PCLK1 whenever the APB1
// prescaler is not 1, per the STM32 clock tree. Read live so a system
// clock-profile change is picked up by amp_set_sample_rate().
static uint32_t amp_timer_clock_hz(void) {
  uint32_t pclk1 = HAL_RCC_GetPCLK1Freq();
  const uint32_t ppre1 = (RCC->CFGR & RCC_CFGR_PPRE1_Msk) >> RCC_CFGR_PPRE1_Pos;
  if (ppre1 >= 4u) { // Field values >= 0b100 select a divider >= 2.
    pclk1 *= 2u;
  }
  return pclk1;
}

// Set the DAC DMA to circular or normal, re-initializing only on a change. The
// __HAL_LINKDMA association survives re-init (the handle address is unchanged).
static HAL_StatusTypeDef amp_config_dma_mode(uint32_t mode) {
  if (hdma_dac_ch1.Init.Mode == mode) {
    return HAL_OK;
  }
  hdma_dac_ch1.Init.Mode = mode;
  return HAL_DMA_Init(&hdma_dac_ch1);
}

// Stop DMA (if any) without a glide and clear streaming state. Leaves the DAC
// channel ready; callers follow up with idle bias or a new transfer.
static void amp_abort_playback(void) {
  if (s_state == AMP_STATE_PLAYING) {
    HAL_DAC_Stop_DMA(&AMP_HDAC, AMP_DAC_CHANNEL);
    s_state = AMP_STATE_IDLE;
  }
  s_refill = NULL;
  s_stream_buf = NULL;
  s_stream_half = 0;
}

// Drive the DAC to a static mid-scale output (non-DMA). TIM6 keeps triggering
// DHR -> DOR so the level actually appears at the pin.
static void amp_hold_midscale(void) {
  HAL_DAC_SetValue(&AMP_HDAC, AMP_DAC_CHANNEL, AMP_DAC_ALIGN, AMP_DAC_MIDSCALE);
  HAL_DAC_Start(&AMP_HDAC, AMP_DAC_CHANNEL);
}

// Blocking linear glide from @p start to mid-scale, for click suppression on
// stop/disable. App context only (uses HAL_Delay); never call from an ISR.
static void amp_glide_to_midscale(uint16_t start) {
  const int32_t from = (int32_t)start;
  const int32_t to = (int32_t)AMP_DAC_MIDSCALE;
  for (uint32_t step = 1u; step <= AMP_MUTE_RAMP_STEPS; step++) {
    const int32_t val =
        from + (to - from) * (int32_t)step / AMP_MUTE_RAMP_STEPS;
    HAL_DAC_SetValue(&AMP_HDAC, AMP_DAC_CHANNEL, AMP_DAC_ALIGN, (uint32_t)val);
    HAL_Delay(AMP_MUTE_RAMP_STEP_MS);
  }
  HAL_DAC_SetValue(&AMP_HDAC, AMP_DAC_CHANNEL, AMP_DAC_ALIGN, AMP_DAC_MIDSCALE);
}

// ISR-safe return to mid-scale idle after a one-shot clip / on DMA error. No
// glide (one-shot clips are authored to end at silence); no HAL_Delay.
static void amp_return_to_idle_from_isr(void) {
  HAL_DAC_Stop_DMA(&AMP_HDAC, AMP_DAC_CHANNEL);
  s_refill = NULL;
  amp_hold_midscale();
  s_state = AMP_STATE_IDLE;
}

/** User implementations into STM32 HAL (overwrite weak HAL functions). *******/

void HAL_DAC_ConvHalfCpltCallbackCh1_pam8302a(DAC_HandleTypeDef *hdac) {
  if (hdac->Instance != AMP_DAC_INSTANCE) {
    return;
  }

  // First half drained: refill it with the next chunk.
  if (s_refill != NULL) {
    s_refill(s_stream_buf, s_stream_half);
  }
}

void HAL_DAC_ConvCpltCallbackCh1_pam8302a(DAC_HandleTypeDef *hdac) {
  if (hdac->Instance != AMP_DAC_INSTANCE) {
    return;
  }

  if (s_refill != NULL) {
    // Second half drained: refill it. DMA wraps (circular) and streaming
    // continues.
    s_refill(s_stream_buf + s_stream_half, s_stream_half);
  } else if (!s_loop) {
    // One-shot clip finished (normal-mode DMA already stopped): idle at
    // mid-scale.
    amp_return_to_idle_from_isr();
  }
  // Looping direct clip (circular, no refill): nothing to do, DMA wraps.
}

void HAL_DAC_ErrorCallbackCh1_pam8302a(DAC_HandleTypeDef *hdac) {
  if (hdac->Instance != AMP_DAC_INSTANCE) {
    return;
  }

  // Underrun or DMA error: fail safe to mid-scale idle so the amp input does
  // not sit at a stuck level.
  amp_return_to_idle_from_isr();
}

/** Public functions. *********************************************************/

HAL_StatusTypeDef amp_init(void) {
  // Peripherals (PA1, DAC1, TIM6, DMA1_Ch3) are already configured by MX_*.
  // Start muted regardless of the reset state.
  HAL_GPIO_WritePin(AMP_SD_GPIO_PORT, AMP_SD_GPIO_PIN, GPIO_PIN_RESET);
  s_enabled = false;
  s_loop = false;
  s_refill = NULL;
  s_stream_buf = NULL;
  s_stream_half = 0;

  if (amp_set_sample_rate(AMP_DEFAULT_SAMPLE_RATE_HZ) != HAL_OK) {
    return HAL_ERROR;
  }

  // Start the sample-rate trigger, then park the DAC at mid-scale idle.
  if (HAL_TIM_Base_Start(&AMP_HTIM) != HAL_OK) {
    return HAL_ERROR;
  }
  amp_hold_midscale();

  s_state = AMP_STATE_IDLE;
  return HAL_OK;
}

HAL_StatusTypeDef amp_enable(void) {
  if (s_state == AMP_STATE_UNINIT) {
    return HAL_ERROR;
  }
  // Caller guarantees the 5V rail is up. Re-assert mid-scale bias and let the
  // AC-coupling network settle before un-muting.
  amp_hold_midscale();
  HAL_Delay(AMP_ENABLE_SETTLE_MS);
  HAL_GPIO_WritePin(AMP_SD_GPIO_PORT, AMP_SD_GPIO_PIN, GPIO_PIN_SET);
  s_enabled = true;
  return HAL_OK;
}

HAL_StatusTypeDef amp_disable(void) {
  if (s_state == AMP_STATE_UNINIT) {
    return HAL_ERROR;
  }
  // Glide to mid-scale (also stops any playback), then mute the amp.
  amp_stop();
  HAL_GPIO_WritePin(AMP_SD_GPIO_PORT, AMP_SD_GPIO_PIN, GPIO_PIN_RESET);
  s_enabled = false;
  return HAL_OK;
}

HAL_StatusTypeDef amp_play(const uint16_t *samples, uint16_t length,
                           bool loop) {
  if (s_state == AMP_STATE_UNINIT) {
    return HAL_ERROR;
  }
  if (samples == NULL || length == 0u) {
    return HAL_ERROR;
  }

  amp_abort_playback();
  s_loop = loop;
  s_refill = NULL;

  // Circular to loop, normal so a one-shot ends cleanly at mid-scale idle.
  HAL_StatusTypeDef status =
      amp_config_dma_mode(loop ? DMA_CIRCULAR : DMA_NORMAL);
  if (status != HAL_OK) {
    return status;
  }

  // pData is typed uint32_t* but the DMA is half-word: only the low 16 bits of
  // each source element are used (same pattern as the WS2812B PWM DMA).
  status =
      HAL_DAC_Start_DMA(&AMP_HDAC, AMP_DAC_CHANNEL,
                        (uint32_t *)(uintptr_t)samples, length, AMP_DAC_ALIGN);
  if (status == HAL_OK) {
    s_state = AMP_STATE_PLAYING;
  }
  return status;
}

HAL_StatusTypeDef amp_play_clip(const amp_clip_t *clip) {
  if (clip == NULL || clip->samples == NULL || clip->length == 0u) {
    return HAL_ERROR;
  }
  if (clip->length > UINT16_MAX) {
    // Too large for a single DMA transfer; use amp_play_stream() instead.
    return HAL_ERROR;
  }
  if (clip->sample_rate_hz != 0u) {
    const HAL_StatusTypeDef status = amp_set_sample_rate(clip->sample_rate_hz);
    if (status != HAL_OK) {
      return status;
    }
  }
  return amp_play(clip->samples, (uint16_t)clip->length, clip->loop);
}

HAL_StatusTypeDef amp_play_stream(uint16_t *ring, uint16_t total_len,
                                  amp_refill_cb_t refill) {
  if (s_state == AMP_STATE_UNINIT) {
    return HAL_ERROR;
  }
  if (ring == NULL || refill == NULL || total_len < 2u || (total_len & 1u)) {
    return HAL_ERROR;
  }

  amp_abort_playback();
  s_stream_buf = ring;
  s_stream_half = total_len / 2u;
  s_refill = refill;
  s_loop = true; // Streaming is inherently circular.

  // Prime the whole ring before the DMA starts reading it.
  refill(ring, total_len);

  HAL_StatusTypeDef status = amp_config_dma_mode(DMA_CIRCULAR);
  if (status != HAL_OK) {
    s_refill = NULL;
    return status;
  }
  status = HAL_DAC_Start_DMA(&AMP_HDAC, AMP_DAC_CHANNEL, (uint32_t *)ring,
                             total_len, AMP_DAC_ALIGN);
  if (status == HAL_OK) {
    s_state = AMP_STATE_PLAYING;
  } else {
    s_refill = NULL;
  }
  return status;
}

HAL_StatusTypeDef amp_stop(void) {
  if (s_state == AMP_STATE_UNINIT) {
    return HAL_ERROR;
  }

  uint16_t start = AMP_DAC_MIDSCALE;
  if (s_state == AMP_STATE_PLAYING) {
    // Capture the current output level, stop the DMA, and resume a static
    // output at that level so the glide is continuous.
    start = (uint16_t)HAL_DAC_GetValue(&AMP_HDAC, AMP_DAC_CHANNEL);
    HAL_DAC_Stop_DMA(&AMP_HDAC, AMP_DAC_CHANNEL);
    s_refill = NULL;
    HAL_DAC_SetValue(&AMP_HDAC, AMP_DAC_CHANNEL, AMP_DAC_ALIGN, start);
    HAL_DAC_Start(&AMP_HDAC, AMP_DAC_CHANNEL);
  }

  amp_glide_to_midscale(start);
  s_state = AMP_STATE_IDLE;
  return HAL_OK;
}

HAL_StatusTypeDef amp_set_sample_rate(uint32_t sample_rate_hz) {
  if (sample_rate_hz < AMP_MIN_SAMPLE_RATE_HZ ||
      sample_rate_hz > AMP_MAX_SAMPLE_RATE_HZ) {
    return HAL_ERROR;
  }

  const uint32_t timer_clk = amp_timer_clock_hz();
  // Rounded divide: period = timer_clk / sample_rate.
  const uint32_t period = (timer_clk + sample_rate_hz / 2u) / sample_rate_hz;
  if (period == 0u || period > 0x10000u) { // ARR is 16-bit (period - 1).
    return HAL_ERROR;
  }

  __HAL_TIM_SET_PRESCALER(&AMP_HTIM, 0u);
  __HAL_TIM_SET_AUTORELOAD(&AMP_HTIM, period - 1u);
  AMP_HTIM.Init.Prescaler = 0u;
  AMP_HTIM.Init.Period = period - 1u;
  return HAL_OK;
}

void amp_suspend(void) {
  // Fast, non-blocking mute for Stop 2 / coin-cell outage entry.
  HAL_GPIO_WritePin(AMP_SD_GPIO_PORT, AMP_SD_GPIO_PIN, GPIO_PIN_RESET);
  if (s_state == AMP_STATE_PLAYING) {
    HAL_DAC_Stop_DMA(&AMP_HDAC, AMP_DAC_CHANNEL);
  }
  HAL_DAC_Stop(&AMP_HDAC, AMP_DAC_CHANNEL);
  HAL_TIM_Base_Stop(&AMP_HTIM);

  s_state = AMP_STATE_UNINIT;
  s_enabled = false;
  s_refill = NULL;
  s_stream_buf = NULL;
  s_stream_half = 0;
}

bool amp_is_playing(void) { return s_state == AMP_STATE_PLAYING; }
