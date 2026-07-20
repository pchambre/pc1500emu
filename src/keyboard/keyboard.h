#pragma once

#include <cstdint>

namespace pc1500 {

// Placeholder keyboard matrix. Row/column layout pending hardware research
// (docs/pc1500_hardware.md).
class Keyboard {
 public:
  void setKeyState(int row, int col, bool pressed);
  uint8_t scan(uint8_t driveLines) const;
};

}  // namespace pc1500
