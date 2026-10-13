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

  // Right after BLCON (2026-10-08, RP2350/time_sync.h): the host app's
  // clock -- UTC in Unix ms and its UTC offset in seconds -- for setting
  // the PC-1500's. False with no link, a link to another PC-1500 (never
  // asked), or an app that can't tell.
  virtual bool hostTime(int64_t* utcMs, int32_t* offsetS) {
    (void)utcMs, (void)offsetS;
    return false;
  }
};

}  // namespace pc1500
