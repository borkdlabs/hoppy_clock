/*******************************************************************************
 * @file w25q128jv_hal_qspi.c
 * @brief W25Q128JV functions: abstracting STM32 HAL: QUADSPI.
 *******************************************************************************
 * @note:
 * Read/write/erase are non-blocking (DMA + QUADSPI IRQ), poll w25q_get_state()
 * for completion. Init/read_id are blocking init-time probes. Transfers use the
 * quad lines (0xEB read, 0x32 program), quad mode is enabled at init.
 *
 * w25q_mmap_enable() exposes the flash read-only at W25Q_MMAP_BASE for direct
 * pointer access, mutually exclusive with the indirect ops (which return
 * HAL_ERROR while mapped). The caller decides when to switch.
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

// True while the peripheral is in memory-mapped mode; indirect read/write/erase
// are then rejected until w25q_mmap_disable().
static volatile bool s_mmapped = false;

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
  if (s_mmapped) {
    return HAL_ERROR; // Indirect read unavailable while memory-mapped.
  }
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
  if (s_mmapped) {
    return HAL_ERROR; // Must w25q_mmap_disable() before programming.
  }
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

// Issue an erase (sector or block) and wait for it on the interrupt. The only
// difference between erase sizes is the opcode; the address selects the region.
static HAL_StatusTypeDef w25q_erase(uint8_t instruction, uint32_t address) {
  if (s_mmapped) {
    return HAL_ERROR; // Must w25q_mmap_disable() before erasing.
  }
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
  cmd.Instruction = instruction;
  cmd.AddressMode = QSPI_ADDRESS_1_LINE;
  cmd.Address = address;

  hal_status = HAL_QSPI_Command(&W25Q_HQSPI, &cmd, W25Q_CMD_TIMEOUT_MS);
  if (hal_status != HAL_OK) {
    w25q_finish(hal_status);
    return hal_status;
  }

  // Wait for the erase to complete on the interrupt (tSE ~400 ms sector, tBE
  // ~2 s for a 64 KB block).
  hal_status = w25q_wait_busy_it();
  if (hal_status != HAL_OK) {
    w25q_finish(hal_status);
  }
  return hal_status;
}

HAL_StatusTypeDef w25q_erase_sector(uint32_t address) {
  return w25q_erase(W25Q_CMD_SECTOR_ERASE, address);
}

HAL_StatusTypeDef w25q_erase_block_64k(uint32_t address) {
  return w25q_erase(W25Q_CMD_BLOCK_ERASE_64K, address);
}

w25q_state_t w25q_get_state(void) { return s_state; }

HAL_StatusTypeDef w25q_mmap_enable(void) {
  if (s_state == W25Q_STATE_BUSY) {
    return HAL_BUSY;
  }
  if (s_mmapped) {
    return HAL_OK; // Already mapped.
  }

  // Same Quad I/O Fast Read (0xEB) recipe as w25q_read(), minus the per-call
  // Address/NbData -- in memory-mapped mode the AHB access supplies those.
  QSPI_CommandTypeDef cmd;
  w25q_cmd_defaults(&cmd);
  cmd.Instruction = W25Q_CMD_FAST_READ_QUAD_IO;
  cmd.AddressMode = QSPI_ADDRESS_4_LINES;
  cmd.AlternateByteMode = QSPI_ALTERNATE_BYTES_4_LINES;
  cmd.AlternateBytes = W25Q_QUAD_READ_MODE_BITS;
  cmd.AlternateBytesSize = QSPI_ALTERNATE_BYTES_8_BITS;
  cmd.DummyCycles = W25Q_QUAD_READ_DUMMY_CYCLES;
  cmd.DataMode = QSPI_DATA_4_LINES;

  QSPI_MemoryMappedTypeDef mmap_cfg;
  mmap_cfg.TimeOutActivation = QSPI_TIMEOUT_COUNTER_DISABLE;
  mmap_cfg.TimeOutPeriod = 0;

  const HAL_StatusTypeDef hal_status =
      HAL_QSPI_MemoryMapped(&W25Q_HQSPI, &cmd, &mmap_cfg);
  if (hal_status == HAL_OK) {
    s_mmapped = true;
  }
  return hal_status;
}

HAL_StatusTypeDef w25q_mmap_disable(void) {
  if (!s_mmapped) {
    return HAL_OK; // Already indirect.
  }

  // Abort tears down the memory-mapped decode and frees the peripheral for
  // indirect commands again.
  const HAL_StatusTypeDef hal_status = HAL_QSPI_Abort(&W25Q_HQSPI);
  if (hal_status == HAL_OK) {
    s_mmapped = false;
    s_op = W25Q_OP_NONE;
    s_state = W25Q_STATE_IDLE;
  }
  return hal_status;
}

bool w25q_is_memory_mapped(void) { return s_mmapped; }
