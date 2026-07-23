/*******************************************************************************
 * @file w25q128jv_hal_qspi.h
 * @brief W25Q128JV functions: abstracting STM32 HAL: QUADSPI.
 *******************************************************************************
 * @note:
 * Non-blocking: w25q_read()/w25q_write()/w25q_erase_sector() launch an
 * operation and return immediately, with completion observed through
 * w25q_get_state(). The data phase of a read/write is moved by DMA and the
 * erase/program BUSY wait runs on the QUADSPI interrupt via auto-polling. The
 * short command setup (and the per-page hand-off during a multi-page write)
 * runs in the QUADSPI IRQ context. (w25q_init() and w25q_read_id() are the
 * exception: blocking init-time probes.)
 *
 * Data transport uses the quad lines (IO0-IO3): reads use Quad I/O Fast Read
 * (0xEB) and writes use Quad Input Page Program (0x32). The instruction phase,
 * address-only commands (erase, write-enable) and status polling stay single-
 * line, which is standard for this part. Quad mode is enabled at init by
 * setting the QE bit in Status Register-2; the IO2/IO3 pull-ups keep /WP and
 * /HOLD deasserted until then. Memory-mapped playback is left for a later
 * layer.
 *
 * Requires QUADSPI DMA on DMA1 Channel 5 and the QUADSPI global interrupt to be
 * enabled (see HAL_QSPI_MspInit / MX_DMA_Init).
 *******************************************************************************
 */

#ifndef HOPPY_CLOCK__W25Q128JV_HAL_QSPI_H
#define HOPPY_CLOCK__W25Q128JV_HAL_QSPI_H

/** Includes. *****************************************************************/

#include "stm32l4xx_hal.h"

/** STM32 port and pin configs. ***********************************************/

extern QSPI_HandleTypeDef hqspi;

// QUADSPI.
#define W25Q_HQSPI hqspi

/** Definitions. **************************************************************/

// Expected JEDEC ID (0x9F) bytes for the W25Q128JV.
#define W25Q_MANUFACTURER_ID 0xEF // Winbond.
#define W25Q_MEMORY_TYPE 0x40
#define W25Q_CAPACITY 0x18 // 128 Mbit.

// Memory geometry.
#define W25Q_PAGE_SIZE 256U    // Bytes per programmable page.
#define W25Q_SECTOR_SIZE 4096U // Bytes per erasable sector.

// Maximum bytes moved by a single w25q_read(): the DMA transfer counter (CNDTR)
// is 16-bit, so one transfer is capped at 65535 bytes.
#define W25Q_READ_MAX 65535U

// Instruction set. Reads/writes use the quad-line variants; the remainder are
// single-line.
#define W25Q_CMD_WRITE_ENABLE 0x06
#define W25Q_CMD_READ_STATUS_1 0x05
#define W25Q_CMD_READ_STATUS_2 0x35
#define W25Q_CMD_WRITE_STATUS_2 0x31
#define W25Q_CMD_READ_JEDEC_ID 0x9F
#define W25Q_CMD_FAST_READ_QUAD_IO 0xEB
#define W25Q_CMD_QUAD_PAGE_PROGRAM 0x32
#define W25Q_CMD_SECTOR_ERASE 0x20

// Quad I/O Fast Read (0xEB): 8 mode bits (M7-M0) then 4 dummy clocks follow the
// address. Mode bits of 0x00 keep continuous-read mode disabled so every read
// re-sends the instruction.
#define W25Q_QUAD_READ_MODE_BITS 0x00
#define W25Q_QUAD_READ_DUMMY_CYCLES 4

// Status register bits.
#define W25Q_SR1_BUSY 0x01 // SR-1: write or erase in progress.
#define W25Q_SR2_QE 0x02   // SR-2: Quad Enable.

// Timeout (ms) for the short, blocking command-setup phase of each operation,
// and for the blocking Status Register-2 write during quad-mode enable.
#define W25Q_CMD_TIMEOUT_MS 100U
#define W25Q_STATUS_WRITE_TIMEOUT_MS 100U

/** Public types. *************************************************************/

/**
 * @brief Driver operation state, reported by w25q_get_state().
 */
typedef enum {
  W25Q_STATE_IDLE = 0, // Ready to accept a new operation.
  W25Q_STATE_BUSY,     // An operation is in progress.
  W25Q_STATE_ERROR,    // The last operation failed.
} w25q_state_t;

/** Public functions. *********************************************************/

/**
 * @brief Initialize, probe and enable quad mode on the W25Q128JV.
 *
 * Reads the JEDEC ID (blocking), checks it against the expected W25Q128JV
 * signature, then sets the Status Register-2 QE bit if not already set so quad
 * read/program work. Must be called once, after the QUADSPI peripheral is
 * initialized (MX_QUADSPI_Init) and while no asynchronous operation is in
 * flight.
 *
 * @return HAL_OK on success, HAL_ERROR on a signature mismatch, HAL error
 * status otherwise.
 */
HAL_StatusTypeDef w25q_init(void);

/**
 * @brief Read the 3-byte JEDEC ID (0x9F) (blocking, single-line).
 *
 * @param id Destination for the 3 ID bytes: [0] manufacturer, [1] memory type,
 * [2] capacity.
 *
 * @return HAL_OK on success, HAL error status otherwise.
 */
HAL_StatusTypeDef w25q_read_id(uint8_t id[3]);

/**
 * @brief Launch a non-blocking Quad I/O Fast Read (0xEB) into a buffer via DMA.
 *
 * Returns as soon as the transfer is started; the read is complete once
 * w25q_get_state() returns W25Q_STATE_IDLE. The destination buffer must stay
 * valid until then. Requires quad mode (enabled by w25q_init()).
 *
 * @param address Byte address to start reading from (0 .. 16 MB - 1).
 * @param buffer Destination buffer (must persist until completion).
 * @param length Number of bytes to read (1 .. W25Q_READ_MAX).
 *
 * @return HAL_OK if the read was launched, HAL_BUSY if another operation is in
 * flight, HAL error status otherwise.
 */
HAL_StatusTypeDef w25q_read(uint32_t address, uint8_t *buffer, uint32_t length);

/**
 * @brief Launch a non-blocking write of arbitrary length via DMA.
 *
 * Splits the write across 256-byte pages automatically, issuing Write Enable
 * and Quad Input Page Program (0x32) per page and chaining pages from the
 * QUADSPI interrupt. Returns as soon as the first page is started, the write is
 * complete once w25q_get_state() returns W25Q_STATE_IDLE. The source buffer
 * must stay valid until then, the target range must have been erased (0xFF)
 * beforehand, and quad mode must be enabled (by w25q_init()).
 *
 * @param address Byte address to start programming at.
 * @param buffer Source bytes (must persist until completion).
 * @param length Number of bytes to program.
 *
 * @return HAL_OK if the write was launched, HAL_BUSY if another operation is in
 * flight, HAL error status otherwise.
 */
HAL_StatusTypeDef w25q_write(uint32_t address, const uint8_t *buffer,
                             uint32_t length);

/**
 * @brief Launch a non-blocking erase of the 4 KB sector at an address.
 *
 * Issues Write Enable then Sector Erase (0x20, single-line) and waits for
 * completion on the QUADSPI interrupt. The whole 4 KB sector is set to 0xFF.
 * The erase is complete once w25q_get_state() returns W25Q_STATE_IDLE.
 *
 * @param address Any byte address within the sector to erase.
 *
 * @return HAL_OK if the erase was launched, HAL_BUSY if another operation is in
 * flight, HAL error status otherwise.
 */
HAL_StatusTypeDef w25q_erase_sector(uint32_t address);

/**
 * @brief Get the current driver state.
 *
 * @return W25Q_STATE_IDLE when ready, W25Q_STATE_BUSY while an operation is in
 * flight, W25Q_STATE_ERROR if the last operation failed.
 */
w25q_state_t w25q_get_state(void);

#endif
