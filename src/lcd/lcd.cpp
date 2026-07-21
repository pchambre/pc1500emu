#include "lcd.h"

namespace pc1500 {

uint16_t Lcd::addressForColumn(int column) {
  if (column < kColumnsPerHalf) {
    return static_cast<uint16_t>(kLeftHalfBase + column);
  }
  return static_cast<uint16_t>(kRightHalfBase + (column - kColumnsPerHalf));
}

uint8_t Lcd::columnBits(int column, bool displayOn,
                         const std::function<uint8_t(uint16_t)>& readByte) const {
  if (!displayOn) return 0;
  return readByte(addressForColumn(column));
}

bool Lcd::dot(int column, int row, bool displayOn,
              const std::function<uint8_t(uint16_t)>& readByte) const {
  uint8_t bits = columnBits(column, displayOn, readByte);
  return (bits & (1 << row)) != 0;
}

}  // namespace pc1500
