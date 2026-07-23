/*******************************************************************************
 * @file w25q128jv_hal_qspi.c
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

/** Includes. *****************************************************************/

#include "w25q128jv_hal_qspi.h"

/** Private variables. ********************************************************/

// Operation currently owning the peripheral, so the shared completion
// callbacks know how to advance.
typedef enum {
  W25Q_OP_NONE = 0,
  W25Q_OP_READ,
  W25Q_OP_WRITE,
  W25Q_OP_ERASE,
} w25q_op_t;

static volatile w25q_state_t s_state = W25Q_STATE_IDLE;
static volatile w25q_op_t s_op = W25Q_OP_NONE;

// Multi-page write cursor, advanced from the QUADSPI interrupt as each page
// completes. s_wr_ptr points into the caller's buffer (no internal copy).
static const uint8_t *s_wr_ptr;
static uint32_t s_wr_addr;
static uint32_t s_wr_remaining;
static uint32_t s_wr_chunk;

/** Private functions. ********************************************************/

// Populate a command struct with the fields common to every instruction.
// Callers override Instruction, the Address/AlternateByte/Data line modes and
// sizes, NbData and DummyCycles as needed.
static void w25q_cmd_defaults(QSPI_CommandTypeDef *cmd) {
  cmd->InstructionMode = QSPI_INSTRUCTION_1_LINE;
  cmd->Instruction = 0;
  cmd->AddressMode = QSPI_ADDRESS_NONE;
  cmd->AddressSize = QSPI_ADDRESS_24_BITS;
  cmd->Address = 0;
  cmd->AlternateByteMode = QSPI_ALTERNATE_BYTES_NONE;
  cmd->AlternateBytes = 0;
  cmd->AlternateBytesSize = QSPI_ALTERNATE_BYTES_8_BITS;
  cmd->DataMode = QSPI_DATA_NONE;
  cmd->NbData = 0;
  cmd->DummyCycles = 0;
  cmd->DdrMode = QSPI_DDR_MODE_DISABLE;
  cmd->DdrHoldHalfCycle = QSPI_DDR_HHC_ANALOG_DELAY;
  cmd->SIOOMode = QSPI_SIOO_INST_EVERY_CMD;
}

// Set the Write Enable Latch (0x06) ahead of a program, erase or status write.
static HAL_StatusTypeDef w25q_write_enable(void) {
  QSPI_CommandTypeDef cmd;
  w25q_cmd_defaults(&cmd);
  cmd.Instruction = W25Q_CMD_WRITE_ENABLE;

  return HAL_QSPI_Command(&W25Q_HQSPI, &cmd, W25Q_CMD_TIMEOUT_MS);
}

// Fill the command/config used to poll Status Register-1 BUSY.
static void w25q_busy_poll_config(QSPI_CommandTypeDef *cmd,
                                  QSPI_AutoPollingTypeDef *cfg) {
  w25q_cmd_defaults(cmd);
  cmd->Instruction = W25Q_CMD_READ_STATUS_1;
  cmd->DataMode = QSPI_DATA_1_LINE;

  cfg->Match = 0x00;         // Wait for BUSY == 0.
  cfg->Mask = W25Q_SR1_BUSY; // Watch only the BUSY bit.
  cfg->Interval = 0x10;      // Poll interval in clock cycles.
  cfg->StatusBytesSize = 1;  // Status Register-1 is one byte.
  cfg->MatchMode = QSPI_MATCH_MODE_AND;
  cfg->AutomaticStop = QSPI_AUTOMATIC_STOP_ENABLE;
}

// Start a non-blocking wait on BUSY. Completion fires StatusMatchCallback.
static HAL_StatusTypeDef w25q_wait_busy_it(void) {
  QSPI_CommandTypeDef cmd;
  QSPI_AutoPollingTypeDef cfg;
  w25q_busy_poll_config(&cmd, &cfg);

  return HAL_QSPI_AutoPolling_IT(&W25Q_HQSPI, &cmd, &cfg);
}

// Block until BUSY clears. Used only on the init-time path.
static HAL_StatusTypeDef w25q_wait_busy(uint32_t timeout_ms) {
  QSPI_CommandTypeDef cmd;
  QSPI_AutoPollingTypeDef cfg;
  w25q_busy_poll_config(&cmd, &cfg);

  return HAL_QSPI_AutoPolling(&W25Q_HQSPI, &cmd, &cfg, timeout_ms);
}

// Enable quad mode by setting the Status Register-2 QE bit (blocking). Reads the
// register first and skips the write when QE is already set, to avoid spending
// the non-volatile status register's limited write endurance.
static HAL_StatusTypeDef w25q_quad_enable(void) {
  QSPI_CommandTypeDef cmd;
  w25q_cmd_defaults(&cmd);
  cmd.Instruction = W25Q_CMD_READ_STATUS_2;
  cmd.DataMode = QSPI_DATA_1_LINE;
  cmd.NbData = 1;

  HAL_StatusTypeDef hal_status =
      HAL_QSPI_Command(&W25Q_HQSPI, &cmd, W25Q_CMD_TIMEOUT_MS);
  if (hal_status != HAL_OK) {
    return hal_status;
  }

  uint8_t sr2 = 0;
  hal_status = HAL_QSPI_Receive(&W25Q_HQSPI, &sr2, W25Q_CMD_TIMEOUT_MS);
  if (hal_status != HAL_OK) {
    return hal_status;
  }

  // Already enabled: nothing to do.
  if (sr2 & W25Q_SR2_QE) {
    return HAL_OK;
  }

  hal_status = w25q_write_enable();
  if (hal_status != HAL_OK) {
    return hal_status;
  }

  // Write Status Register-2 (0x31) with QE set.
  sr2 |= W25Q_SR2_QE;
  w25q_cmd_defaults(&cmd);
  cmd.Instruction = W25Q_CMD_WRITE_STATUS_2;
  cmd.DataMode = QSPI_DATA_1_LINE;
  cmd.NbData = 1;

  hal_status = HAL_QSPI_Command(&W25Q_HQSPI, &cmd, W25Q_CMD_TIMEOUT_MS);
  if (hal_status != HAL_OK) {
    return hal_status;
  }
  hal_status = HAL_QSPI_Transmit(&W25Q_HQSPI, &sr2, W25Q_CMD_TIMEOUT_MS);
  if (hal_status != HAL_OK) {
    return hal_status;
  }

  // Block until the status-register write completes.
  return w25q_wait_busy(W25Q_STATUS_WRITE_TIMEOUT_MS);
}

// Program the chunk of the pending write that lies in the current page, then
// hand the data phase to DMA. Uses the s_wr_* cursor. Completion fires
// HAL_QSPI_TxCpltCallback.
static HAL_StatusTypeDef w25q_program_current_page(void) {
  // Clamp the chunk to the end of the current 256-byte page.
  const uint32_t page_remaining = W25Q_PAGE_SIZE - (s_wr_addr % W25Q_PAGE_SIZE);
  s_wr_chunk =
      (s_wr_remaining < page_remaining) ? s_wr_remaining : page_remaining;

  HAL_StatusTypeDef hal_status = w25q_write_enable();
  if (hal_status != HAL_OK) {
    return hal_status;
  }

  QSPI_CommandTypeDef cmd;
  w25q_cmd_defaults(&cmd);
  cmd.Instruction = W25Q_CMD_QUAD_PAGE_PROGRAM;
  cmd.AddressMode = QSPI_ADDRESS_1_LINE; // 0x32 sends the address single-line.
  cmd.Address = s_wr_addr;
  cmd.DataMode = QSPI_DATA_4_LINES; // Data is clocked in on IO0-IO3.
  cmd.NbData = s_wr_chunk;

  hal_status = HAL_QSPI_Command(&W25Q_HQSPI, &cmd, W25Q_CMD_TIMEOUT_MS);
  if (hal_status != HAL_OK) {
    return hal_status;
  }

  return HAL_QSPI_Transmit_DMA(&W25Q_HQSPI, (uint8_t *)s_wr_ptr);
}

// Mark the active operation fully finished. reason == HAL_OK leaves the driver
// idle, anything else latches the error state.
static void w25q_finish(HAL_StatusTypeDef reason) {
  s_op = W25Q_OP_NONE;
  s_state = (reason == HAL_OK) ? W25Q_STATE_IDLE : W25Q_STATE_ERROR;
}

// Advance to the next step of a multi-step operation: keep the driver BUSY on a
// successful launch, or latch the error if the follow-on launch failed.
static void w25q_continue(HAL_StatusTypeDef reason) {
  if (reason != HAL_OK) {
    w25q_finish(reason);
  }
}

/** User implementations into STM32 HAL (overwrite weak HAL functions). *******/

void HAL_QSPI_RxCpltCallback(QSPI_HandleTypeDef *hqspi) {
  if (hqspi->Instance != QUADSPI) {
    return;
  }
  // Read data phase done.
  w25q_finish(HAL_OK);
}

void HAL_QSPI_TxCpltCallback(QSPI_HandleTypeDef *hqspi) {
  if (hqspi->Instance != QUADSPI) {
    return;
  }
  // Page data sent; wait (non-blocking) for the program to complete. Stays BUSY
  // until HAL_QSPI_StatusMatchCallback fires.
  w25q_continue(w25q_wait_busy_it());
}

void HAL_QSPI_StatusMatchCallback(QSPI_HandleTypeDef *hqspi) {
  if (hqspi->Instance != QUADSPI) {
    return;
  }

  if (s_op == W25Q_OP_WRITE) {
    // Current page programmed; advance the cursor.
    s_wr_addr += s_wr_chunk;
    s_wr_ptr += s_wr_chunk;
    s_wr_remaining -= s_wr_chunk;

    // Chain the next page if bytes remain, stay BUSY until it completes.
    if (s_wr_remaining > 0) {
      w25q_continue(w25q_program_current_page());
      return;
    }
  }

  // Erase finished, or the final write page completed.
  w25q_finish(HAL_OK);
}

void HAL_QSPI_ErrorCallback(QSPI_HandleTypeDef *hqspi) {
  if (hqspi->Instance != QUADSPI) {
    return;
  }
  w25q_finish(HAL_ERROR);
}

/** Public functions. *********************************************************/

HAL_StatusTypeDef w25q_init(void) {
  uint8_t id[3];
  const HAL_StatusTypeDef hal_status = w25q_read_id(id);
  if (hal_status != HAL_OK) {
    return hal_status;
  }

  // Confirm the JEDEC ID matches the W25Q128JV.
  if (id[0] != W25Q_MANUFACTURER_ID || id[1] != W25Q_MEMORY_TYPE ||
      id[2] != W25Q_CAPACITY) {
    return HAL_ERROR;
  }

  // Enable quad mode so w25q_read()/w25q_write() can use the quad lines.
  return w25q_quad_enable();
}

HAL_StatusTypeDef w25q_read_id(uint8_t id[3]) {
  QSPI_CommandTypeDef cmd;
  w25q_cmd_defaults(&cmd);
  cmd.Instruction = W25Q_CMD_READ_JEDEC_ID;
  cmd.DataMode = QSPI_DATA_1_LINE;
  cmd.NbData = 3;

  const HAL_StatusTypeDef hal_status =
      HAL_QSPI_Command(&W25Q_HQSPI, &cmd, W25Q_CMD_TIMEOUT_MS);
  if (hal_status != HAL_OK) {
    return hal_status;
  }

  return HAL_QSPI_Receive(&W25Q_HQSPI, id, W25Q_CMD_TIMEOUT_MS);
}

HAL_StatusTypeDef w25q_read(uint32_t address, uint8_t *buffer,
                            uint32_t length) {
  if (s_state == W25Q_STATE_BUSY) {
    return HAL_BUSY;
  }

  QSPI_CommandTypeDef cmd;
  w25q_cmd_defaults(&cmd);
  cmd.Instruction = W25Q_CMD_FAST_READ_QUAD_IO;
  cmd.AddressMode = QSPI_ADDRESS_4_LINES;
  cmd.Address = address;
  cmd.AlternateByteMode = QSPI_ALTERNATE_BYTES_4_LINES;
  cmd.AlternateBytes = W25Q_QUAD_READ_MODE_BITS;
  cmd.AlternateBytesSize = QSPI_ALTERNATE_BYTES_8_BITS;
  cmd.DummyCycles = W25Q_QUAD_READ_DUMMY_CYCLES;
  cmd.DataMode = QSPI_DATA_4_LINES;
  cmd.NbData = length;

  s_op = W25Q_OP_READ;
  s_state = W25Q_STATE_BUSY;

  HAL_StatusTypeDef hal_status =
      HAL_QSPI_Command(&W25Q_HQSPI, &cmd, W25Q_CMD_TIMEOUT_MS);
  if (hal_status != HAL_OK) {
    w25q_finish(hal_status);
    return hal_status;
  }

  hal_status = HAL_QSPI_Receive_DMA(&W25Q_HQSPI, buffer);
  if (hal_status != HAL_OK) {
    w25q_finish(hal_status);
  }
  return hal_status;
}

HAL_StatusTypeDef w25q_write(uint32_t address, const uint8_t *buffer,
                             uint32_t length) {
  if (s_state == W25Q_STATE_BUSY) {
    return HAL_BUSY;
  }

  s_wr_addr = address;
  s_wr_ptr = buffer;
  s_wr_remaining = length;
  s_op = W25Q_OP_WRITE;
  s_state = W25Q_STATE_BUSY;

  const HAL_StatusTypeDef hal_status = w25q_program_current_page();
  if (hal_status != HAL_OK) {
    w25q_finish(hal_status);
  }
  return hal_status;
}

HAL_StatusTypeDef w25q_erase_sector(uint32_t address) {
  if (s_state == W25Q_STATE_BUSY) {
    return HAL_BUSY;
  }

  s_op = W25Q_OP_ERASE;
  s_state = W25Q_STATE_BUSY;

  HAL_StatusTypeDef hal_status = w25q_write_enable();
  if (hal_status != HAL_OK) {
    w25q_finish(hal_status);
    return hal_status;
  }

  QSPI_CommandTypeDef cmd;
  w25q_cmd_defaults(&cmd);
  cmd.Instruction = W25Q_CMD_SECTOR_ERASE;
  cmd.AddressMode = QSPI_ADDRESS_1_LINE;
  cmd.Address = address;

  hal_status = HAL_QSPI_Command(&W25Q_HQSPI, &cmd, W25Q_CMD_TIMEOUT_MS);
  if (hal_status != HAL_OK) {
    w25q_finish(hal_status);
    return hal_status;
  }

  // Wait for the erase to complete on the interrupt (tSE up to 400 ms).
  hal_status = w25q_wait_busy_it();
  if (hal_status != HAL_OK) {
    w25q_finish(hal_status);
  }
  return hal_status;
}

w25q_state_t w25q_get_state(void) { return s_state; }
