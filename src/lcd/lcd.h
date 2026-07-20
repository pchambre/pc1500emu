#pragma once

#include <array>
#include <cstdint>

namespace pc1500 {

// Placeholder dot-matrix LCD controller. Controller chip model, addressing
// scheme, and 156x7 layout split pending hardware research
// (docs/pc1500_hardware.md).
class Lcd {
 public:
  uint8_t read(uint8_t reg);
  void write(uint8_t reg, uint8_t value);
};

}  // namespace pc1500
