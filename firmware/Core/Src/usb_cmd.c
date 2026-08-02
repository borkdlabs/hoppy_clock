/*******************************************************************************
 * @file usb_cmd.c
 * @brief USB CDC command layer: framed host<->device protocol.
 *******************************************************************************
 * @note:
 * Frame format (both directions):
 *
 *   [USB_CMD_SOF][cmd][len][payload 0..len-1][crc8]
 *
 * crc8 (poly 0x07, init 0x00) covers cmd + len + payload. Received bytes are
 * pushed from the USB ISR (usb_cmd_on_rx) into a ring buffer. usb_cmd_task()
 * called from the scheduler, drains it through an incremental parser and
 * dispatches, so all command handling (RTC writes, responses) runs in the
 * cooperative loop, not in interrupt context.
 *
 * A response echoes the request cmd, its payload[0] is a status byte
 * (USB_CMD_STATUS_*), followed by any returned data.
 *******************************************************************************
 */

/** Includes. *****************************************************************/

#include "usb_cmd.h"
#include "alarm_rt.h"
#include "light.h"
#include "manifest.h"
#include "rtc.h"
#include "sound.h"
#include "usbd_cdc_if.h"
#include "ws2812b_hal_pwm.h"
#include <stdbool.h>
#include <string.h>

/** Private variables. ********************************************************/

// RX ring: producer = usb_cmd_on_rx (ISR), consumer = usb_cmd_task (loop).
#define RX_RING_SIZE 128u
static volatile uint8_t s_ring[RX_RING_SIZE];
static volatile uint16_t s_head; // Write index (ISR).
static volatile uint16_t s_tail; // Read index (task).

// Incremental frame parser state.
typedef enum {
  PARSE_SOF = 0,
  PARSE_CMD,
  PARSE_LEN,
  PARSE_PAYLOAD,
  PARSE_CRC,
} parse_state_t;

static parse_state_t s_parse;
static uint8_t s_cmd;
static uint8_t s_len;
static uint8_t s_idx;
static uint8_t s_payload[USB_CMD_MAX_PAYLOAD];

// Static TX frame (must persist: the USB stack sends it asynchronously).
static uint8_t s_tx[3u + USB_CMD_MAX_PAYLOAD + 1u];

// Staging for a whole-config replace: CFG_BEGIN clears it, CFG_SET_* fill it,
// CFG_COMMIT hands it to the manifest and persists.
static alarm_record_t s_stage[MANIFEST_MAX_ALARMS];
static light_seq_t s_stage_lights[MANIFEST_MAX_LIGHTS];
static uint8_t s_stage_lamp_on;
static uint8_t s_stage_lamp_off;
static uint8_t s_stage_led_count;

// The config frames must fit the payload budget: index (1) + record (12), a
// GET_ALARM reply of status (1) + record (12), and id (1) + light_seq (8).
_Static_assert(1u + sizeof(alarm_record_t) <= USB_CMD_MAX_PAYLOAD,
               "alarm config frame exceeds USB_CMD_MAX_PAYLOAD");
_Static_assert(1u + sizeof(light_seq_t) <= USB_CMD_MAX_PAYLOAD,
               "light config frame exceeds USB_CMD_MAX_PAYLOAD");

extern USBD_HandleTypeDef hUsbDeviceFS;

/** Private functions. ********************************************************/

static uint8_t crc8_update(uint8_t crc, uint8_t byte) {
  crc ^= byte;
  for (uint8_t i = 0; i < 8u; i++) {
    crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0x07u) : (uint8_t)(crc << 1);
  }
  return crc;
}

static uint8_t frame_crc(uint8_t cmd, uint8_t len, const uint8_t *payload) {
  uint8_t crc = crc8_update(0u, cmd);
  crc = crc8_update(crc, len);
  for (uint8_t i = 0; i < len; i++) {
    crc = crc8_update(crc, payload[i]);
  }
  return crc;
}

// True once the CDC IN endpoint is free to accept a new transmit.
static bool usb_tx_ready(void) {
  const USBD_CDC_HandleTypeDef *hcdc =
      (USBD_CDC_HandleTypeDef *)hUsbDeviceFS.pClassData;
  return (hcdc != NULL) && (hcdc->TxState == 0u);
}

// Build and send a response frame. Waits (bounded) for the previous transmit
// to finish before reusing s_tx.
static void send_frame(uint8_t cmd, const uint8_t *payload, uint8_t len) {
  if (len > USB_CMD_MAX_PAYLOAD) {
    return;
  }

  // Wait for the endpoint to be free so s_tx is not overwritten mid-transfer.
  for (uint32_t spin = 0; spin < 100000u && !usb_tx_ready(); spin++) {}

  s_tx[0] = USB_CMD_SOF;
  s_tx[1] = cmd;
  s_tx[2] = len;
  for (uint8_t i = 0; i < len; i++) {
    s_tx[3u + i] = payload[i];
  }
  s_tx[3u + len] = frame_crc(cmd, len, payload);

  (void)CDC_Transmit_FS(s_tx, (uint16_t)(4u + len));
}

static void handle_set_time(const uint8_t *p, uint8_t len) {
  uint8_t status = USB_CMD_STATUS_OK;

  // Payload: year, month, date, weekday, hours, minutes, seconds.
  if (len != 7u || p[0] > 99u || p[1] < 1u || p[1] > 12u || p[2] < 1u ||
      p[2] > 31u || p[3] < 1u || p[3] > 7u || p[4] > 23u || p[5] > 59u ||
      p[6] > 59u) {
    status = USB_CMD_STATUS_ERR;
  } else {
    set_date(p[0], p[1], p[2], p[3]);
    set_time(p[4], p[5], p[6]);
    alarm_rt_rearm(); // "Now" moved; the next-alarm time may have changed.
  }

  send_frame(USB_CMD_SET_TIME, &status, 1u);
}

static void handle_get_time(void) {
  uint8_t resp[8];
  resp[0] = USB_CMD_STATUS_OK;
  get_date_time_fields(&resp[1], &resp[2], &resp[3], &resp[4], &resp[5],
                       &resp[6], &resp[7]);
  send_frame(USB_CMD_GET_TIME, resp, sizeof(resp));
}

static void handle_set_led(const uint8_t *p, uint8_t len) {
  uint8_t status = USB_CMD_STATUS_OK;

  // Payload: index, red, green, blue.
  if (len != 4u || p[0] >= ws2812b_get_count()) {
    status = USB_CMD_STATUS_ERR;
  } else {
    ws2812b_set_colour(p[0], p[1], p[2], p[3]);
    if (ws2812b_update() != HAL_OK) {
      status = USB_CMD_STATUS_ERR; // Prior LED DMA still in flight.
    }
  }

  send_frame(USB_CMD_SET_LED, &status, 1u);
}

static void handle_cfg_begin(void) {
  memset(s_stage, 0, sizeof(s_stage));
  memset(s_stage_lights, 0, sizeof(s_stage_lights));
  s_stage_lamp_on = 0u;
  s_stage_lamp_off = 0u;
  s_stage_led_count = 1u; // Default; the host resends the real count.
  const uint8_t status = USB_CMD_STATUS_OK;
  send_frame(USB_CMD_CFG_BEGIN, &status, 1u);
}

static void handle_cfg_set_alarm(const uint8_t *p, uint8_t len) {
  uint8_t status = USB_CMD_STATUS_OK;

  // Payload: index (1 B) followed by a 12-byte alarm record.
  if (len != 1u + sizeof(alarm_record_t) || p[0] >= MANIFEST_MAX_ALARMS) {
    status = USB_CMD_STATUS_ERR;
  } else {
    memcpy(&s_stage[p[0]], &p[1], sizeof(alarm_record_t));
  }

  send_frame(USB_CMD_CFG_SET_ALARM, &status, 1u);
}

static void handle_cfg_set_light(const uint8_t *p, uint8_t len) {
  uint8_t status = USB_CMD_STATUS_OK;

  // Payload: id (1 B) followed by an 8-byte light_seq.
  if (len != 1u + sizeof(light_seq_t) || p[0] >= MANIFEST_MAX_LIGHTS) {
    status = USB_CMD_STATUS_ERR;
  } else {
    memcpy(&s_stage_lights[p[0]], &p[1], sizeof(light_seq_t));
  }

  send_frame(USB_CMD_CFG_SET_LIGHT, &status, 1u);
}

static void handle_cfg_set_lamp(const uint8_t *p, uint8_t len) {
  uint8_t status = USB_CMD_STATUS_OK;

  // Payload: lamp on id, lamp off id.
  if (len != 2u) {
    status = USB_CMD_STATUS_ERR;
  } else {
    s_stage_lamp_on = p[0];
    s_stage_lamp_off = p[1];
  }

  send_frame(USB_CMD_CFG_SET_LAMP, &status, 1u);
}

static void handle_cfg_set_leds(const uint8_t *p, uint8_t len) {
  uint8_t status = USB_CMD_STATUS_OK;

  // Payload: active LED count.
  if (len != 1u) {
    status = USB_CMD_STATUS_ERR;
  } else {
    s_stage_led_count = p[0];
  }

  send_frame(USB_CMD_CFG_SET_LEDS, &status, 1u);
}

static void handle_cfg_commit(const uint8_t *p, uint8_t len) {
  uint8_t status = USB_CMD_STATUS_OK;

  // Payload: alarm_count, light_count. Applies the staged config and persists
  // it (blocking erase + program, the cooperative loop stalls briefly).
  if (len != 2u || !manifest_set_alarms(s_stage, p[0]) ||
      !manifest_set_lights(s_stage_lights, p[1])) {
    status = USB_CMD_STATUS_ERR;
  } else {
    manifest_set_lamp(s_stage_lamp_on, s_stage_lamp_off);
    manifest_set_led_count(s_stage_led_count);
    if (manifest_save() != HAL_OK) {
      status = USB_CMD_STATUS_ERR;
    } else {
      ws2812b_set_count(s_stage_led_count); // Apply the new chain length.
      light_lamp_reapply(); // Refresh the strip at the new size.
      alarm_rt_rearm();     // Re-arm to the soonest entry.
    }
  }

  send_frame(USB_CMD_CFG_COMMIT, &status, 1u);
}

static void handle_cfg_get_count(void) {
  const manifest_t *m = manifest_get();
  const uint8_t resp[6] = {USB_CMD_STATUS_OK,
                           (uint8_t)m->header.alarm_count,
                           (uint8_t)m->header.light_count,
                           m->header.lamp_on_light,
                           m->header.lamp_off_light,
                           m->header.led_count};
  send_frame(USB_CMD_CFG_GET_COUNT, resp, sizeof(resp));
}

static void handle_cfg_get_alarm(const uint8_t *p, uint8_t len) {
  const manifest_t *m = manifest_get();

  if (len != 1u || p[0] >= m->header.alarm_count) {
    const uint8_t status = USB_CMD_STATUS_ERR;
    send_frame(USB_CMD_CFG_GET_ALARM, &status, 1u);
    return;
  }

  uint8_t resp[1u + sizeof(alarm_record_t)];
  resp[0] = USB_CMD_STATUS_OK;
  memcpy(&resp[1], &m->alarms[p[0]], sizeof(alarm_record_t));
  send_frame(USB_CMD_CFG_GET_ALARM, resp, sizeof(resp));
}

static void handle_cfg_get_light(const uint8_t *p, uint8_t len) {
  const manifest_t *m = manifest_get();

  if (len != 1u || p[0] >= m->header.light_count) {
    const uint8_t status = USB_CMD_STATUS_ERR;
    send_frame(USB_CMD_CFG_GET_LIGHT, &status, 1u);
    return;
  }

  uint8_t resp[1u + sizeof(light_seq_t)];
  resp[0] = USB_CMD_STATUS_OK;
  memcpy(&resp[1], &m->lights[p[0]], sizeof(light_seq_t));
  send_frame(USB_CMD_CFG_GET_LIGHT, resp, sizeof(resp));
}

static uint16_t le16(const uint8_t *p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static void handle_snd_begin(const uint8_t *p, uint8_t len) {
  uint8_t status = USB_CMD_STATUS_OK;

  // Payload: id (1), format (1), sample_rate (2), total_len (4).
  if (len != 8u ||
      sound_write_begin(p[0], p[1], le16(&p[2]), le32(&p[4])) != HAL_OK) {
    status = USB_CMD_STATUS_ERR;
  }

  send_frame(USB_CMD_SND_BEGIN, &status, 1u);
}

static void handle_snd_data(const uint8_t *p, uint8_t len) {
  const uint8_t status = (sound_write_data(p, len) == HAL_OK)
                             ? USB_CMD_STATUS_OK
                             : USB_CMD_STATUS_ERR;
  send_frame(USB_CMD_SND_DATA, &status, 1u);
}

static void handle_snd_end(const uint8_t *p, uint8_t len) {
  uint8_t status = USB_CMD_STATUS_OK;

  if (len != 4u || sound_write_end(le32(p)) != HAL_OK) {
    status = USB_CMD_STATUS_ERR;
  }

  send_frame(USB_CMD_SND_END, &status, 1u);
}

static void handle_snd_info(const uint8_t *p, uint8_t len) {
  sound_entry_t e;

  if (len != 1u || !sound_get_info(p[0], &e)) {
    const uint8_t status = USB_CMD_STATUS_ERR;
    send_frame(USB_CMD_SND_INFO, &status, 1u);
    return;
  }

  // Reply: status, format, sample_rate (2), length (4), crc32 (4).
  uint8_t resp[12];
  resp[0] = USB_CMD_STATUS_OK;
  resp[1] = e.format;
  resp[2] = (uint8_t)e.sample_rate;
  resp[3] = (uint8_t)(e.sample_rate >> 8);
  resp[4] = (uint8_t)e.length;
  resp[5] = (uint8_t)(e.length >> 8);
  resp[6] = (uint8_t)(e.length >> 16);
  resp[7] = (uint8_t)(e.length >> 24);
  resp[8] = (uint8_t)e.crc32;
  resp[9] = (uint8_t)(e.crc32 >> 8);
  resp[10] = (uint8_t)(e.crc32 >> 16);
  resp[11] = (uint8_t)(e.crc32 >> 24);
  send_frame(USB_CMD_SND_INFO, resp, sizeof(resp));
}

static void dispatch(uint8_t cmd, const uint8_t *payload, uint8_t len) {
  switch (cmd) {
  case USB_CMD_PING: {
    const uint8_t status = USB_CMD_STATUS_OK;
    send_frame(USB_CMD_PING, &status, 1u);
    break;
  }
  case USB_CMD_SET_TIME:
    handle_set_time(payload, len);
    break;
  case USB_CMD_GET_TIME:
    handle_get_time();
    break;
  case USB_CMD_SET_LED:
    handle_set_led(payload, len);
    break;
  case USB_CMD_CFG_BEGIN:
    handle_cfg_begin();
    break;
  case USB_CMD_CFG_SET_ALARM:
    handle_cfg_set_alarm(payload, len);
    break;
  case USB_CMD_CFG_COMMIT:
    handle_cfg_commit(payload, len);
    break;
  case USB_CMD_CFG_GET_COUNT:
    handle_cfg_get_count();
    break;
  case USB_CMD_CFG_GET_ALARM:
    handle_cfg_get_alarm(payload, len);
    break;
  case USB_CMD_CFG_SET_LIGHT:
    handle_cfg_set_light(payload, len);
    break;
  case USB_CMD_CFG_SET_LAMP:
    handle_cfg_set_lamp(payload, len);
    break;
  case USB_CMD_CFG_GET_LIGHT:
    handle_cfg_get_light(payload, len);
    break;
  case USB_CMD_CFG_SET_LEDS:
    handle_cfg_set_leds(payload, len);
    break;
  case USB_CMD_SND_BEGIN:
    handle_snd_begin(payload, len);
    break;
  case USB_CMD_SND_DATA:
    handle_snd_data(payload, len);
    break;
  case USB_CMD_SND_END:
    handle_snd_end(payload, len);
    break;
  case USB_CMD_SND_INFO:
    handle_snd_info(payload, len);
    break;
  default: {
    const uint8_t status = USB_CMD_STATUS_ERR; // Unknown command.
    send_frame(cmd, &status, 1u);
    break;
  }
  }
}

// Advance the frame parser by one received byte.
static void parse_byte(uint8_t byte) {
  switch (s_parse) {
  case PARSE_SOF:
    if (byte == USB_CMD_SOF) {
      s_parse = PARSE_CMD;
    }
    break;
  case PARSE_CMD:
    s_cmd = byte;
    s_parse = PARSE_LEN;
    break;
  case PARSE_LEN:
    s_len = byte;
    if (s_len > USB_CMD_MAX_PAYLOAD) {
      s_parse = PARSE_SOF; // Invalid length; resync.
    } else {
      s_idx = 0;
      s_parse = (s_len == 0u) ? PARSE_CRC : PARSE_PAYLOAD;
    }
    break;
  case PARSE_PAYLOAD:
    s_payload[s_idx++] = byte;
    if (s_idx >= s_len) {
      s_parse = PARSE_CRC;
    }
    break;
  case PARSE_CRC:
    if (byte == frame_crc(s_cmd, s_len, s_payload)) {
      dispatch(s_cmd, s_payload, s_len);
    }
    s_parse = PARSE_SOF;
    break;
  default:
    s_parse = PARSE_SOF;
    break;
  }
}

/** Public functions. *********************************************************/

void usb_cmd_init(void) {
  s_head = 0;
  s_tail = 0;
  s_parse = PARSE_SOF;
}

void usb_cmd_on_rx(const uint8_t *data, uint32_t len) {
  for (uint32_t i = 0; i < len; i++) {
    const uint16_t next = (uint16_t)((s_head + 1u) % RX_RING_SIZE);
    if (next == s_tail) {
      break; // Ring full; drop the rest.
    }
    s_ring[s_head] = data[i];
    s_head = next;
  }
}

void usb_cmd_task(void) {
  while (s_tail != s_head) {
    const uint8_t byte = s_ring[s_tail];
    s_tail = (uint16_t)((s_tail + 1u) % RX_RING_SIZE);
    parse_byte(byte);
  }
}
