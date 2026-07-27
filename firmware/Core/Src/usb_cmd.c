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
#include "rtc.h"
#include "usbd_cdc_if.h"
#include <stdbool.h>

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
