#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pc1500 {

// The expansion board's BLE link as the firmware's keywords see it
// (2026-09-28): answers RP2350/pc_exp.h's EXP_COMMAND_BLE_* commands, and
// the WRITE/READ/CLOSE_SD_FILE of a BLSAVE/BLLOAD while a transfer is open
// -- the same contract as the firmware's ble_link.c. ExpansionMock forwards
// to one when it's set (setBleBackend()); otherwise its built-in fake peer
// answers, which is what the keyword tests use.
class BleBackend {
 public:
  virtual ~BleBackend() = default;

  // An EXP_COMMAND_BLE_* command (0x40-0x46). `window` is the data window
  // (offset 0 = 0x8000). Returns the status byte.
  virtual uint8_t command(uint8_t cmd, std::vector<uint8_t>& window) = 0;

  // True while a BLSAVE/BLLOAD transfer owns WRITE/READ/CLOSE_SD_FILE.
  virtual bool transferOpen() const = 0;
  virtual uint8_t write(std::vector<uint8_t>& window) = 0;
  virtual uint8_t read(std::vector<uint8_t>& window) = 0;
  virtual uint8_t close() = 0;

  // This PC-1500's name on the link (MCONF HOSTNAME), for HELLO.
  virtual void setName(const std::string& name) = 0;
};

}  // namespace pc1500
