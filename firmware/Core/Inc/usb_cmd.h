/*******************************************************************************
 * @file usb_cmd.h
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

#ifndef HOPPY_CLOCK__USB_CMD_H
#define HOPPY_CLOCK__USB_CMD_H

/** Includes. *****************************************************************/

#include "stm32l4xx_hal.h"
#include <stdint.h>

/** Definitions. **************************************************************/

#define USB_CMD_SOF 0xA5u
#define USB_CMD_MAX_PAYLOAD 32u

// Command IDs.
#define USB_CMD_PING 0x01u     // No payload, replies [status].
#define USB_CMD_SET_TIME 0x10u // Payload: year,month,date,weekday,h,m,s (7 B).
#define USB_CMD_GET_TIME 0x11u // No payload, replies [status + same 7 fields].
#define USB_CMD_SET_LED 0x20u  // Payload: index,r,g,b (4 B), replies [status].

// Response status codes (payload[0] of a response frame).
#define USB_CMD_STATUS_OK 0x00u
#define USB_CMD_STATUS_ERR 0x01u

/** Public functions. *********************************************************/

/**
 * @brief Reset the command layer (ring buffer + frame parser).
 */
void usb_cmd_init(void);

/**
 * @brief Feed bytes received over CDC into the RX ring (ISR context).
 *
 * Called from CDC_Receive_FS. Copies bytes only; parsing happens in the task.
 *
 * @param data Received bytes.
 * @param len Number of bytes.
 */
void usb_cmd_on_rx(const uint8_t *data, uint32_t len);

/**
 * @brief Drain the RX ring, parse frames, and dispatch commands.
 *
 * Call periodically from the scheduler. Non-blocking apart from a brief wait
 * for the CDC IN endpoint to be free when sending a response.
 */
void usb_cmd_task(void);

#endif
