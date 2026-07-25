#include "lcd.h"

namespace pc1500 {

uint8_t Lcd::columnBits(int column, bool displayOn,
                         const std::function<uint8_t(uint16_t)>& readByte) const {
  if (!displayOn) return 0;
  int chip = column / kColumnsPerChip;        // 0-3
  int localCol = column % kColumnsPerChip;    // 0-38
  uint16_t base = (chip % 2 == 0) ? kLeftHalfBase : kRightHalfBase;
  bool highNibble = chip >= 2;
  uint8_t upperByte = readByte(static_cast<uint16_t>(base + 2 * localCol));      // rows 0-3
  uint8_t lowerByte = readByte(static_cast<uint16_t>(base + 2 * localCol + 1));  // rows 4-6
  uint8_t upper4 = highNibble ? static_cast<uint8_t>(upperByte >> 4) : (upperByte & 0x0F);
  uint8_t lower3 = highNibble ? static_cast<uint8_t>(lowerByte >> 4) : (lowerByte & 0x0F);
  return static_cast<uint8_t>((lower3 << 4) | (upper4 & 0x0F));
}

bool Lcd::dot(int column, int row, bool displayOn,
              const std::function<uint8_t(uint16_t)>& readByte) const {
  uint8_t bits = columnBits(column, displayOn, readByte);
  return (bits & (1 << row)) != 0;
}

}  // namespace pc1500
