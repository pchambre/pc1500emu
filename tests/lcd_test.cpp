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

void testAddressForColumn() {
  CHECK(pc1500::Lcd::addressForColumn(0) == 0x7600);
  CHECK(pc1500::Lcd::addressForColumn(77) == 0x764D);
  CHECK(pc1500::Lcd::addressForColumn(78) == 0x7700);
  CHECK(pc1500::Lcd::addressForColumn(155) == 0x774D);
}

void testColumnBitsAndDotsReflectBuffer() {
  pc1500::Keyboard kb;
  pc1500::Bus bus(kb);
  pc1500::Lcd lcd;
  auto readByte = [&](uint16_t addr) { return bus.readME0(addr); };

  bus.writeME0(0x7600, 0b0000101);  // column 0: rows 0 and 2 lit
  CHECK(lcd.columnBits(0, /*displayOn=*/true, readByte) == 0b0000101);
  CHECK(lcd.dot(0, 0, true, readByte) == true);
  CHECK(lcd.dot(0, 1, true, readByte) == false);
  CHECK(lcd.dot(0, 2, true, readByte) == true);

  bus.writeME0(0x774D, 0b1000000);  // column 155 (last of the right half): row 6 lit
  CHECK(lcd.dot(155, 6, true, readByte) == true);
  CHECK(lcd.dot(155, 5, true, readByte) == false);
}

void testDisplayOffBlanksEverything() {
  pc1500::Keyboard kb;
  pc1500::Bus bus(kb);
  pc1500::Lcd lcd;
  auto readByte = [&](uint16_t addr) { return bus.readME0(addr); };

  bus.writeME0(0x7600, 0xFF);  // all 7 rows lit, if display were on
  CHECK(lcd.columnBits(0, /*displayOn=*/false, readByte) == 0);
  CHECK(lcd.dot(0, 0, false, readByte) == false);
}

}  // namespace

int main() {
  testAddressForColumn();
  testColumnBitsAndDotsReflectBuffer();
  testDisplayOffBlanksEverything();

  if (g_failures == 0) {
    std::printf("All tests passed.\n");
    return 0;
  }
  std::printf("%d check(s) failed.\n", g_failures);
  return 1;
}
