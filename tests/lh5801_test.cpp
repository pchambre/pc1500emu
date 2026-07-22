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

void loadProgram(TestBus& bus, uint16_t base, const uint8_t* program, size_t len) {
  for (size_t i = 0; i < len; i++) {
    bus.writeME0(static_cast<uint16_t>(base + i), program[i]);
  }
}

// Fakes a return address on the stack, runs `cpu` from `entry` until it
// returns there (or a step budget is exhausted), and checks that it did
// so with the stack balanced back to where it started. Shared by every
// "run one of the manual's worked ML examples end-to-end" test below.
void runSubroutineToCompletion(lh5801::CPU& cpu, TestBus& bus, uint16_t entry,
                                uint16_t stackTop = 0x4700, int maxSteps = 5000) {
  const uint16_t returnSentinel = 0x9999;
  cpu.setS(stackTop);
  bus.writeME0(static_cast<uint16_t>(stackTop + 1), static_cast<uint8_t>(returnSentinel >> 8));
  bus.writeME0(static_cast<uint16_t>(stackTop + 2), static_cast<uint8_t>(returnSentinel & 0xFF));
  cpu.setP(entry);

  int steps = 0;
  while (cpu.p() != returnSentinel && steps < maxSteps) {
    cpu.step();
    steps++;
  }
  CHECK(cpu.p() == returnSentinel);
  CHECK(cpu.s() == static_cast<uint16_t>(stackTop + 2));
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
  const uint16_t base = 0x40C5;
  loadProgram(bus, base, program, sizeof(program));

  for (int i = 0; i <= 0x4D; i++) {
    bus.writeME0(static_cast<uint16_t>(0x7600 + i), static_cast<uint8_t>(i));
    bus.writeME0(static_cast<uint16_t>(0x7700 + i), static_cast<uint8_t>(0x80 + i));
  }

  runSubroutineToCompletion(cpu, bus, base);

  for (int i = 0; i <= 0x4D; i++) {
    uint8_t expectedLow = static_cast<uint8_t>(~static_cast<uint8_t>(i));
    uint8_t expectedHigh = static_cast<uint8_t>(~static_cast<uint8_t>(0x80 + i));
    CHECK(bus.readME0(static_cast<uint16_t>(0x7600 + i)) == expectedLow);
    CHECK(bus.readME0(static_cast<uint16_t>(0x7700 + i)) == expectedHigh);
  }
}

// Section 6-1 "Binary to hexadecimal conversion" (labeled page 144):
// converts Xreg's 4 nibbles to ASCII hex digits, stored starting at U.
// Chosen test input (Xreg=0x1234, all nibbles 0-9) deliberately avoids
// the branch for nibbles >=0x0A: the manual's mnemonic column says that
// branch should do `ADI A,30H`, but the machine-language column shows
// byte `36`, and neither actually produces correct ASCII 'A'-'F' (`37H`
// would) -- likely a manual typo/scan artifact, not independently
// resolved. Everything else in the program (SJP/RTN nesting via two
// entry points into the same subroutine, AEX, ANI in both
// accumulator-immediate and memory-immediate forms, CPI, the
// not-taken BCS path, SIN) is exercised and checked exactly.
void testManualHexConversionExample() {
  TestBus bus;
  lh5801::CPU cpu(bus);
  cpu.reset();

  const uint8_t program[] = {
      0x68, 0x77,        // LDI UH,77H
      0x6A, 0xE0,        // LDI UL,E0H
      0x84,              // LDA XH
      0xBE, 0x40, 0xE0,  // SJP 40E0H
      0x61,              // SIN U
      0x84,              // LDA XH
      0xBE, 0x40, 0xE1,  // SJP 40E1H
      0x61,              // SIN U
      0x04,              // LDA XL
      0xBE, 0x40, 0xE0,  // SJP 40E0H
      0x61,              // SIN U
      0x04,              // LDA XL
      0xBE, 0x40, 0xE1,  // SJP 40E1H
      0x61,              // SIN U
      0x69, 0x00,        // ANI (U),00H
      0x9A,              // RTN
  };
  const uint16_t base = 0x40C5;
  loadProgram(bus, base, program, sizeof(program));

  const uint8_t subroutine[] = {
      0xF1,        // AEX             (40E0)
      0xB9, 0x0F,  // ANI A,0FH       (40E1)
      0xB7, 0x0A,  // CPI A,0AH
      0x83, 0x03,  // BCS +03H
      0xB3, 0x30,  // ADI A,30H
      0x9A,        // RTN
      0xB3, 0x36,  // ADI A,36H (see comment above re: this byte)
      0x9A,        // RTN
  };
  loadProgram(bus, 0x40E0, subroutine, sizeof(subroutine));

  cpu.setX(0x1234);

  runSubroutineToCompletion(cpu, bus, base);

  CHECK(bus.readME0(0x77E0) == '1');
  CHECK(bus.readME0(0x77E1) == '2');
  CHECK(bus.readME0(0x77E2) == '3');
  CHECK(bus.readME0(0x77E3) == '4');
  CHECK(bus.readME0(0x77E4) == 0x00);  // ANI (U),00H terminator
}

// Sections 6-3 "Single display dot left shift" and 6-4 "...right shift"
// (labeled pages 146-147). Both use a two-level loop (outer advances
// across the two 78-column halves, inner walks each half) built on
// LOP/LIN/SDE/SIN/CPI/BCS or BCR, plus PSH/POP X/Y/U bracketing the
// whole routine and absolute (ab) addressing at both ends of the
// buffer. The branch/loop-target arithmetic was checked by hand and is
// exercised for real here, but the exact bit-level shift semantics
// were not independently re-derived (nibble-level AEX/ANI manipulation
// across column boundaries -- enough additional tracing that hand
// errors were a real risk). Test input is an all-zero buffer: shifting
// zeros in either direction still produces zeros regardless of the
// exact algorithm, so this validates control flow, addressing, and
// stack balance across the full instruction sequence without claiming
// to prove the shift direction/semantics themselves are correct.
void testManualDisplayShiftLeftExample() {
  TestBus bus;
  lh5801::CPU cpu(bus);
  cpu.reset();

  const uint8_t program[] = {
      0xFD, 0x88,        // PSH X            (40C5)
      0xFD, 0x98,        // PSH Y
      0xFD, 0xA8,        // PSH U
      0xA5, 0x76, 0x00,  // LDA 7600H
      0xF1,              // AEX
      0xB9, 0x0F,        // ANI A,0FH
      0x0A,              // STA XL
      0xA5, 0x76, 0x01,  // LDA 7601H
      0xF1,              // AEX
      0xB9, 0x0F,        // ANI A,0FH
      0x08,              // STA XH
      0x68, 0x78,        // LDI UH,78H       (40D9)
      0xFD, 0x62,        // DEC UH           (40DB, outer loop target)
      0x6A, 0x4D,        // LDI UL,4DH
      0x66,              // DEC U            (40DF, inner loop target)
      0x65,              // LIN U
      0x1A,              // STA YL
      0x25,              // LDA U
      0x18,              // STA YH
      0x84,              // LDA XH
      0x63,              // SDE U
      0x04,              // LDA XL
      0x2E,              // STA U
      0xFD, 0x18,        // LDX Y
      0x88, 0x0D,        // LOP -0DH (-> 40DF)
      0x6C, 0x77,        // CPI UH,77H
      0x93, 0x15,        // BCS -15H (-> 40DB)
      0x04,              // LDA XL
      0xF1,              // AEX
      0xAE, 0x77, 0x4E,  // STA 774EH
      0x84,              // LDA XH
      0xF1,              // AEX
      0xAE, 0x77, 0x4F,  // STA 774FH
      0xFD, 0x2A,        // POP U
      0xFD, 0x1A,        // POP Y
      0xFD, 0x0A,        // POP X
      0xF9,              // REC
      0x9A,              // RTN
  };
  const uint16_t base = 0x40C5;
  loadProgram(bus, base, program, sizeof(program));

  runSubroutineToCompletion(cpu, bus, base);

  CHECK(cpu.flags().c == false);  // REC ran right before RTN
  for (int i = 0; i <= 0x4D; i++) {
    CHECK(bus.readME0(static_cast<uint16_t>(0x7600 + i)) == 0);
    CHECK(bus.readME0(static_cast<uint16_t>(0x7700 + i)) == 0);
  }
}

void testManualDisplayShiftRightExample() {
  TestBus bus;
  lh5801::CPU cpu(bus);
  cpu.reset();

  const uint8_t program[] = {
      0xFD, 0x88,        // PSH X            (40C5)
      0xFD, 0x98,        // PSH Y
      0xFD, 0xA8,        // PSH U
      0xA5, 0x77, 0x4C,  // LDA 774CH
      0xF1,              // AEX
      0xB9, 0xF0,        // ANI A,F0H
      0x0A,              // STA XL
      0xA5, 0x77, 0x4D,  // LDA 774DH
      0xF1,              // AEX
      0xB9, 0xF0,        // ANI A,F0H
      0x08,              // STA XH
      0x68, 0x75,        // LDI UH,75H
      0x6A, 0xFF,        // LDI UL,FFH       (40DB, outer loop target)
      0x64,              // INC U            (40DD, inner loop target)
      0x65,              // LIN U
      0x1A,              // STA YL
      0x25,              // LDA U
      0x18,              // STA YH
      0x84,              // LDA XH
      0x63,              // SDE U
      0x04,              // LDA XL
      0x61,              // SIN U
      0xFD, 0x18,        // LDX Y
      0x6E, 0x4D,        // CPI UL,4DH
      0x91, 0x0F,        // BCR -0FH (-> 40DD)
      0x6C, 0x77,        // CPI UH,77H
      0x91, 0x15,        // BCR -15H (-> 40DB)
      0x64,              // INC U
      0x04,              // LDA XL
      0xF1,              // AEX
      0x61,              // SIN U
      0x84,              // LDA XH
      0xF1,              // AEX
      0x2E,              // STA U
      0xFD, 0x2A,        // POP U
      0xFD, 0x1A,        // POP Y
      0xFD, 0x0A,        // POP X
      0xF9,              // REC
      0x9A,              // RTN
  };
  const uint16_t base = 0x40C5;
  loadProgram(bus, base, program, sizeof(program));

  runSubroutineToCompletion(cpu, bus, base);

  CHECK(cpu.flags().c == false);
  for (int i = 0; i <= 0x4D; i++) {
    CHECK(bus.readME0(static_cast<uint16_t>(0x7600 + i)) == 0);
    CHECK(bus.readME0(static_cast<uint16_t>(0x7700 + i)) == 0);
  }
}

}  // namespace

int main() {
  testReset();
  testAdcCarry();
  testCpaConvention();
  testAttTtaBitLayout();
  testBranchPolarityBothDirectionsSameCondition();
  testManualHexConversionExample();
  testManualDisplayShiftLeftExample();
  testManualDisplayShiftRightExample();
  testManualDisplayReverseExample();

  if (g_failures == 0) {
    std::printf("All tests passed.\n");
    return 0;
  }
  std::printf("%d check(s) failed.\n", g_failures);
  return 1;
}
