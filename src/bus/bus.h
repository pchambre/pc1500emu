#pragma once

#include <array>
#include <cstdint>

namespace pc1500 {

// Placeholder memory/bus map. Address ranges and I/O port assignments are
// pending hardware research (docs/pc1500_hardware.md).
class Bus {
 public:
  uint8_t read(uint16_t addr);
  void write(uint16_t addr, uint8_t value);
};

}  // namespace pc1500
