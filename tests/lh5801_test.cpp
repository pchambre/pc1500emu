#include <array>
#include <cstdio>

#include "lh5801.h"

namespace {

class TestBus : public lh5801::MemoryBus {
 public:
  uint8_t readME0(uint16_t addr) override { return me0_[addr]; }
  void writeME0(uint16_t addr, uint8_t value) override { me0_[addr] = value; }
  uint8_t readME1(uint16_t addr) override { return me1_[addr]; }
  void writeME1(uint16_t addr, uint8_t value) override { me1_[addr] = value; }

  std::array<uint8_t, 65536> me0_{};
  std::array<uint8_t, 65536> me1_{};
};

int g_failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::printf("FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__);        \
      g_failures++;                                                        \
    }                                                                      \
  } while (0)

void testReset() {
  TestBus bus;
  bus.writeME0(0xFFFE, 0x12);
  bus.writeME0(0xFFFF, 0x34);
  lh5801::CPU cpu(bus);
  cpu.reset();
  CHECK(cpu.p() == 0x1234);
}

void testAdcCarry() {
  TestBus bus;
  lh5801::CPU cpu(bus);
  cpu.reset();
  // LDI A,0FFH ; ADI A,01H  -- expect A=0x00, C=1, Z=1
  bus.writeME0(0, 0xB5);
  bus.writeME0(1, 0xFF);
  bus.writeME0(2, 0xB3);
  bus.writeME0(3, 0x01);
  cpu.setP(0);
  cpu.step();
  cpu.step();
  CHECK(cpu.a() == 0x00);
  CHECK(cpu.flags().c == true);
  CHECK(cpu.flags().z == true);
}

void testCpaConvention() {
  TestBus bus;
  lh5801::CPU cpu(bus);
  cpu.reset();
  // LDI A,05H ; CPI A,03H -- expect A>i: C=1,Z=0
  bus.writeME0(0, 0xB5);
  bus.writeME0(1, 0x05);
  bus.writeME0(2, 0xB7);
  bus.writeME0(3, 0x03);
  cpu.setP(0);
  cpu.step();
  cpu.step();
  CHECK(cpu.flags().c == true);
  CHECK(cpu.flags().z == false);
  CHECK(cpu.a() == 0x05);  // CPI must not modify A

  // LDI A,03H ; CPI A,05H -- expect A<i: C=0,Z=0
  bus.writeME0(4, 0xB5);
  bus.writeME0(5, 0x03);
  bus.writeME0(6, 0xB7);
  bus.writeME0(7, 0x05);
  cpu.step();
  cpu.step();
  CHECK(cpu.flags().c == false);
  CHECK(cpu.flags().z == false);
}

void testAttTtaBitLayout() {
  TestBus bus;
  lh5801::CPU cpu(bus);
  cpu.reset();
  // T layout per manual 2-2-3: H,V,Z,IE,C from left to right (C=bit0,
  // IE=bit1, Z=bit2, V=bit3, H=bit4). LDI A,i ; ATT (FD EC) ; then TTA
  // (FD AA) should round-trip and unpack into the right individual flags.
  bus.writeME0(0, 0xB5);  // LDI A,i
  bus.writeME0(1, 0x1D);  // 0b00011101: C=1,IE=0,Z=1,V=1,H=1
  bus.writeME0(2, 0xFD);
  bus.writeME0(3, 0xEC);  // ATT
  cpu.setP(0);
  cpu.step();  // LDI
  cpu.step();  // ATT
  CHECK(cpu.flags().c == true);
  CHECK(cpu.flags().ie == false);
  CHECK(cpu.flags().z == true);
  CHECK(cpu.flags().v == true);
  CHECK(cpu.flags().h == true);

  bus.writeME0(4, 0xFD);
  bus.writeME0(5, 0xAA);  // TTA
  cpu.step();
  CHECK(cpu.a() == 0x1D);
}

void testBranchPolarityBothDirectionsSameCondition() {
  TestBus bus;
  lh5801::CPU cpu(bus);
  cpu.reset();
  // SEC ; BCS+05H  -- C=1, so BOTH +i and -i forms of BCS must branch.
  bus.writeME0(0, 0xFB);        // SEC
  bus.writeME0(1, 0x83);        // BCS+i
  bus.writeME0(2, 0x05);
  cpu.setP(0);
  cpu.step();  // SEC
  uint16_t before = cpu.p();
  cpu.step();  // BCS+5
  CHECK(cpu.p() == static_cast<uint16_t>(before + 2 + 5));

  bus.writeME0(0, 0xFB);        // SEC
  bus.writeME0(1, 0x93);        // BCS-i
  bus.writeME0(2, 0x05);
  cpu.setP(0);
  cpu.step();  // SEC
  before = cpu.p();
  cpu.step();  // BCS-5
  CHECK(cpu.p() == static_cast<uint16_t>(before + 2 - 5));
}

// The manual's own worked "display reverse" example (chapter 1, page 2
// label) -- identical byte-for-byte to section 6-2 "Display inversion"
// (labeled page 145), which makes sense: on a 1-bit-per-pixel display,
// bitwise-complementing the buffer (EAI FFH) *is* inverting on/off
// pixels, just described from two angles in two different chapters.
// Traced through by hand: it reverses two 78-byte chunks (0x7700-0x774D
// and 0x7600-0x764D -- the two halves of the 156-column LCD buffer) and
// falls through to RTN, making it a deterministic, bounded end-to-end
// exercise of LDI/DEC/LDA/EAI/STA/LOP/CPI/BCS/RTN together.
void testManualDisplayReverseExample() {
  TestBus bus;
  lh5801::CPU cpu(bus);
  cpu.reset();

  const uint8_t program[] = {
      0x68, 0x78,  // LDI UH,78H
      0x6A, 0x4D,  // LDI UL,4DH
      0xFD, 0x62,  // DEC UH
      0x25,        // LDA U
      0xBD, 0xFF,  // EAI FFH
      0x2E,        // STA U
      0x88, 0x06,  // LOP 06H
      0x6C, 0x77,  // CPI UH,77H
      0x93, 0x0E,  // BCS -0EH
      0x9A,        // RTN
  };
  const uint16_t base = 0x4000;
  for (size_t i = 0; i < sizeof(program); i++) {
    bus.writeME0(static_cast<uint16_t>(base + i), program[i]);
  }

  for (int i = 0; i <= 0x4D; i++) {
    bus.writeME0(static_cast<uint16_t>(0x7600 + i), static_cast<uint8_t>(i));
    bus.writeME0(static_cast<uint16_t>(0x7700 + i), static_cast<uint8_t>(0x80 + i));
  }

  // Fake a "return address" of 0x9999 on the stack so we can detect RTN.
  const uint16_t returnSentinel = 0x9999;
  cpu.setS(0x4700);
  bus.writeME0(0x4701, static_cast<uint8_t>(returnSentinel >> 8));
  bus.writeME0(0x4702, static_cast<uint8_t>(returnSentinel & 0xFF));
  cpu.setP(base);

  int steps = 0;
  const int maxSteps = 2000;
  while (cpu.p() != returnSentinel && steps < maxSteps) {
    cpu.step();
    steps++;
  }
  CHECK(cpu.p() == returnSentinel);
  CHECK(cpu.s() == 0x4702);  // stack balanced back to where it started

  for (int i = 0; i <= 0x4D; i++) {
    uint8_t expectedLow = static_cast<uint8_t>(~static_cast<uint8_t>(i));
    uint8_t expectedHigh = static_cast<uint8_t>(~static_cast<uint8_t>(0x80 + i));
    CHECK(bus.readME0(static_cast<uint16_t>(0x7600 + i)) == expectedLow);
    CHECK(bus.readME0(static_cast<uint16_t>(0x7700 + i)) == expectedHigh);
  }
}

}  // namespace

int main() {
  testReset();
  testAdcCarry();
  testCpaConvention();
  testAttTtaBitLayout();
  testBranchPolarityBothDirectionsSameCondition();
  testManualDisplayReverseExample();

  if (g_failures == 0) {
    std::printf("All tests passed.\n");
    return 0;
  }
  std::printf("%d check(s) failed.\n", g_failures);
  return 1;
}
