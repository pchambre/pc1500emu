#include <cstdio>

#include "bus.h"
#include "keyboard.h"
#include "lcd.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                        \
      std::printf("FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__);       \
      g_failures++;                                                       \
    }                                                                     \
  } while (0)

// Real-hardware-confirmed bit walkthrough (isolating one bit at a time,
// clearing between pokes): 7600H bit0 -> row0, leftmost column (chip1,
// col0); 7600H bit4 -> row0, ~halfway across (chip3, col0 = global col
// 78); 7601H bit0 -> row4 ("third from bottom"), leftmost column (odd
// offset = rows 4-6); 7601H bit4 -> row4, ~halfway across; 7700H bit0 ->
// row0, ~1/4 across (chip2, col0 = global col 39); 7700H bit4 -> row0,
// ~3/4 across (chip4, col0 = global col 117).
void testConfirmedBitMapping() {
  pc1500::Keyboard kb;
  pc1500::Bus bus(kb);
  pc1500::Lcd lcd;
  auto readByte = [&](uint16_t addr) { return bus.readME0(addr); };

  bus.writeME0(0x7600, 0x01);
  CHECK(lcd.dot(0, 0, true, readByte) == true);  // chip1 col0, row0

  bus.writeME0(0x7600, 0x10);
  CHECK(lcd.dot(78, 0, true, readByte) == true);  // chip3 col0 (global 78), row0

  bus.writeME0(0x7600, 0x00);
  bus.writeME0(0x7601, 0x01);
  CHECK(lcd.dot(0, 4, true, readByte) == true);  // chip1 col0, row4 (odd offset)

  bus.writeME0(0x7601, 0x10);
  CHECK(lcd.dot(78, 4, true, readByte) == true);  // chip3 col0, row4

  bus.writeME0(0x7601, 0x00);
  bus.writeME0(0x7700, 0x01);
  CHECK(lcd.dot(39, 0, true, readByte) == true);  // chip2 col0 (global 39), row0

  bus.writeME0(0x7700, 0x10);
  CHECK(lcd.dot(117, 0, true, readByte) == true);  // chip4 col0 (global 117), row0
}

void testColumnBitsAssemblesTwoBytesIntoOne7BitColumn() {
  pc1500::Keyboard kb;
  pc1500::Bus bus(kb);
  pc1500::Lcd lcd;
  auto readByte = [&](uint16_t addr) { return bus.readME0(addr); };

  // 'N''s first font column is 0x7F (all 7 rows lit): confirmed via our
  // own render trace that the ROM writes 0x0F to the even offset (rows
  // 0-3) and 0x07 to the odd offset (rows 4-6), which should reassemble
  // to 0x7F.
  bus.writeME0(0x7600, 0x0F);
  bus.writeME0(0x7601, 0x07);
  CHECK(lcd.columnBits(0, /*displayOn=*/true, readByte) == 0x7F);
  for (int row = 0; row < 7; row++) CHECK(lcd.dot(0, row, true, readByte) == true);

  // Last column of chip4 (global 155 = chip4 col38), high nibble, last
  // byte pair of the right half (7700H+2*38=774C, +1=774D).
  bus.writeME0(0x774C, 0x60);  // high nibble = 0x6 -> rows 1,2 of the upper group
  bus.writeME0(0x774D, 0x00);
  CHECK(lcd.dot(155, 1, true, readByte) == true);
  CHECK(lcd.dot(155, 2, true, readByte) == true);
  CHECK(lcd.dot(155, 0, true, readByte) == false);
}

void testDisplayOffBlanksEverything() {
  pc1500::Keyboard kb;
  pc1500::Bus bus(kb);
  pc1500::Lcd lcd;
  auto readByte = [&](uint16_t addr) { return bus.readME0(addr); };

  bus.writeME0(0x7600, 0xFF);  // all 8 bits set, if display were on
  bus.writeME0(0x7601, 0xFF);
  CHECK(lcd.columnBits(0, /*displayOn=*/false, readByte) == 0);
  CHECK(lcd.dot(0, 0, false, readByte) == false);
}

}  // namespace

int main() {
  testConfirmedBitMapping();
  testColumnBitsAssemblesTwoBytesIntoOne7BitColumn();
  testDisplayOffBlanksEverything();

  if (g_failures == 0) {
    std::printf("All tests passed.\n");
    return 0;
  }
  std::printf("%d check(s) failed.\n", g_failures);
  return 1;
}
