#pragma once

#include <cstdint>
#include <functional>

namespace pc1500 {

// PC-1500's dot-matrix LCD: 156 columns x 7 rows, driven by four SC882G
// chips in two pairs. See docs/pc1500_hardware_reference.md "LCD" section
// for the full derivation.
//
// Unlike a typical memory-mapped peripheral, nothing writes to the LCD
// through a dedicated register -- the display buffer is plain ME0 RAM
// (7600H-77FFH), and this class only interprets that RAM's content. One
// byte = one column's 7 dots (bit0=top row ... bit6=bottom row, bit7
// unused); only the first 78 bytes of each 256-byte half
// (7600H-764DH, 7700H-774DH) are real columns -- confirmed via the
// manual's own worked "display reverse" example (see lh5801_test.cpp).
// Column left/right ordering (7600H-half = left vs right) is an
// unconfirmed-but-likely assumption.
class Lcd {
 public:
  static constexpr int kColumns = 156;
  static constexpr int kRows = 7;
  static constexpr int kColumnsPerHalf = 78;
  static constexpr uint16_t kLeftHalfBase = 0x7600;
  static constexpr uint16_t kRightHalfBase = 0x7700;

  // ME0 address holding the given column's (0-155) 7-dot byte.
  static uint16_t addressForColumn(int column);

  // The given column's raw 7-dot byte, or 0 (all dots off) if the display
  // is off (SDP/RDP flip-flop reset -- see lh5801::CPU::disp()).
  // `readByte` reads one byte from ME0, typically Bus::readME0.
  uint8_t columnBits(int column, bool displayOn,
                      const std::function<uint8_t(uint16_t)>& readByte) const;

  // Convenience single-dot accessor built on columnBits().
  bool dot(int column, int row, bool displayOn,
           const std::function<uint8_t(uint16_t)>& readByte) const;
};

}  // namespace pc1500
