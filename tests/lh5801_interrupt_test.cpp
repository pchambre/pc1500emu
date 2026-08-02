#include <array>
#include <cstdio>
#include <vector>

#include "lh5801.h"
#include "lh5801_timer_table.h"

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
    if (!(cond)) {                                                        \
      std::printf("FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__);       \
      g_failures++;                                                       \
    }                                                                     \
  } while (0)

void setVector(TestBus& bus, uint16_t vectorAddr, uint16_t target) {
  bus.writeME0(vectorAddr, static_cast<uint8_t>(target >> 8));
  bus.writeME0(static_cast<uint16_t>(vectorAddr + 1), static_cast<uint8_t>(target & 0xFF));
}

void testMiRequiresIe() {
  TestBus bus;
  lh5801::CPU cpu(bus);
  cpu.reset();
  setVector(bus, 0xFFF8, 0x8000);
  cpu.setP(0x4000);
  cpu.setS(0x4700);
  bus.writeME0(0x4000, 0x38);  // NOP, in case IE=0 case doesn't dispatch

  // IE=0: MI request should NOT be serviced.
  cpu.requestMI();
  cpu.step();
  CHECK(cpu.p() == 0x4001);  // just executed the NOP normally

  // Enable IE (via ATT, packing a flags byte with IE bit set -- bit1=IE
  // per the confirmed T layout), then request MI again.
  bus.writeME0(0x4001, 0xFD);
  bus.writeME0(0x4002, 0xEC);  // ATT
  bus.writeME0(0x4003, 0x38);  // NOP (should be skipped/replaced by dispatch)
  // Need A=0x02 (IE bit set, everything else clear) before ATT.
  cpu.setP(0x4001);
  // Poke A via LDI first.
  bus.writeME0(0x4001, 0xB5);
  bus.writeME0(0x4002, 0x02);
  bus.writeME0(0x4003, 0xFD);
  bus.writeME0(0x4004, 0xEC);
  cpu.setP(0x4001);
  cpu.step();  // LDI A,02H
  cpu.step();  // ATT -- unpacks IE=1
  CHECK(cpu.flags().ie == true);

  cpu.requestMI();
  uint16_t pBefore = cpu.p();
  int cycles = cpu.step();
  CHECK(cpu.p() == 0x8000);  // dispatched to MI vector
  CHECK(cpu.flags().ie == false);  // disabled on entry
  CHECK(cycles > 0);
  (void)pBefore;
}

void testNmiIgnoresIe() {
  TestBus bus;
  lh5801::CPU cpu(bus);
  cpu.reset();
  setVector(bus, 0xFFFC, 0x9000);
  cpu.setP(0x4000);
  cpu.setS(0x4700);
  CHECK(cpu.flags().ie == false);  // freshly reset, IE off

  cpu.requestNMI();
  cpu.step();
  CHECK(cpu.p() == 0x9000);  // NMI dispatched despite IE=0
}

void testInterruptEntryAndRtiRoundTrip() {
  TestBus bus;
  lh5801::CPU cpu(bus);
  cpu.reset();
  setVector(bus, 0xFFFC, 0x9000);
  bus.writeME0(0x9000, 0x9A);  // RTN's opcode is 9A but RTI's is 8A -- write RTI
  bus.writeME0(0x9000, 0x8A);  // RTI

  cpu.setP(0x4321);
  cpu.setS(0x4700);
  cpu.setX(0);
  // Set a recognizable flag pattern before the interrupt (via ATT).
  bus.writeME0(0x4321, 0xB5);
  bus.writeME0(0x4322, 0x1D);  // C=1,IE=0,Z=1,V=1,H=1 (bit layout: C=0,IE=1,Z=2,V=3,H=4)
  bus.writeME0(0x4323, 0xFD);
  bus.writeME0(0x4324, 0xEC);  // ATT
  cpu.step();  // LDI A,1DH
  cpu.step();  // ATT
  CHECK(cpu.flags().c && !cpu.flags().ie && cpu.flags().z && cpu.flags().v && cpu.flags().h);

  uint16_t returnAddr = cpu.p();  // where execution would resume after the interrupt point
  cpu.requestNMI();
  cpu.step();  // dispatch: pushes T,PL,PH; jumps to 9000H
  CHECK(cpu.p() == 0x9000);
  CHECK(cpu.flags().ie == false);  // cleared on entry

  cpu.step();  // RTI at 9000H: pops PH,PL,T
  CHECK(cpu.p() == returnAddr);
  CHECK(cpu.flags().c && cpu.flags().z && cpu.flags().v && cpu.flags().h);
  CHECK(cpu.flags().ie == false);  // T's IE bit was 0 at interrupt time -- restored, not left set
  CHECK(cpu.s() == 0x4700);  // stack balanced back to where it started
}

void testHaltWakesOnInterrupt() {
  TestBus bus;
  lh5801::CPU cpu(bus);
  cpu.reset();
  setVector(bus, 0xFFFC, 0xA000);
  bus.writeME0(0x4000, 0xFD);
  bus.writeME0(0x4001, 0xB1);  // HLT
  cpu.setP(0x4000);
  cpu.setS(0x4700);

  cpu.step();  // HLT
  CHECK(cpu.halted() == true);
  CHECK(cpu.step() == 0);  // still halted, no progress
  CHECK(cpu.halted() == true);

  cpu.requestNMI();
  cpu.step();  // should wake and dispatch
  CHECK(cpu.halted() == false);
  CHECK(cpu.p() == 0xA000);
}

// Build-time safety net for lh5801_timer_table.h: verifies the table is a
// genuine maximal-length 511-state cycle (every value 1..511 exactly
// once) and independently re-derives each transition from the LFSR
// recurrence itself (feedback = bit8 XOR bit3, i.e. x^9+x^4+1 -- fitted
// against the TRM's own printed table this session, see
// lh5801_timer_table.h's own comment), so a future hand-edit to the
// generated header that breaks the sequence gets caught here rather than
// only surfacing as a mysterious hardware-timing bug much later.
void testPolyCounterTableIntegrity() {
  std::vector<bool> seen(512, false);
  for (uint16_t v : lh5801::kPolyCounterTable) {
    CHECK(v >= 1 && v <= 511);
    CHECK(!seen[v]);
    seen[v] = true;
  }
  for (int v = 1; v <= 511; v++) CHECK(seen[v]);

  for (int i = 0; i < 511; i++) {
    CHECK(lh5801::kPolyCounterReverse[lh5801::kPolyCounterTable[i]] == i);
  }

  CHECK(lh5801::kPolyCounterTable[510] == 0x1FF);  // count 511 -> interrupt state

  for (int i = 0; i < 511; i++) {
    uint16_t s = lh5801::kPolyCounterTable[i];
    uint16_t fb = ((s >> 8) ^ (s >> 3)) & 1;
    uint16_t next = static_cast<uint16_t>(((s << 1) & 0x1FE) | fb);
    CHECK(next == lh5801::kPolyCounterTable[(i + 1) % 511]);
  }
}

// The timer register is a 9-bit polynomial (LFSR) counter, not a linear
// one -- see lh5801_timer_table.h. 0x000 is the LFSR's fixed point, so
// ticking without ever setting a nonzero value (via AM0/AM1) must never
// advance it or fire an interrupt, no matter how many ticks happen --
// unlike the old linear model, where free-running from 0 would
// eventually reach 0x1FF on its own.
void testTimerParkedAtZeroNeverFires() {
  TestBus bus;
  lh5801::CPU cpu(bus);
  cpu.reset();
  CHECK(cpu.timerCounter() == 0);
  for (int i = 0; i < 5000; i++) cpu.tickTimer();
  CHECK(cpu.timerCounter() == 0);
}

// 0x0FF is the real polynomial-table value one step before the
// interrupt-triggering 0x1FF (TRM "count 510" -> "count 511"). Set it via
// a real AM0 instruction (opcode FD CE, MSB=0) rather than poking
// timerCounter_ directly, so this also exercises AM0's own dispatch.
void testTimerInterruptFiresAt1FF() {
  TestBus bus;
  lh5801::CPU cpu(bus);
  cpu.reset();
  setVector(bus, 0xFFFA, 0xB000);
  bus.writeME0(0x4000, 0xB5);
  bus.writeME0(0x4001, 0x02);  // LDI A,02H (IE bit set)
  bus.writeME0(0x4002, 0xFD);
  bus.writeME0(0x4003, 0xEC);  // ATT -- enables IE
  bus.writeME0(0x4004, 0xB5);
  bus.writeME0(0x4005, 0xFF);  // LDI A,0FFH
  bus.writeME0(0x4006, 0xFD);
  bus.writeME0(0x4007, 0xCE);  // AM0 -- timerCounter = 0x0FF
  bus.writeME0(0x4008, 0x38);  // NOP
  cpu.setP(0x4000);
  cpu.setS(0x4700);
  cpu.step();  // LDI A,02H
  cpu.step();  // ATT, IE=1
  CHECK(cpu.flags().ie == true);
  CHECK(cpu.timerCounter() == 0);

  cpu.step();  // LDI A,0FFH
  cpu.step();  // AM0
  CHECK(cpu.timerCounter() == 0x0FF);

  cpu.step();  // NOP -- no interrupt pending yet
  CHECK(cpu.p() == 0x4009);

  bus.writeME0(0x4009, 0x38);  // another NOP, in case
  cpu.tickTimer();             // 0x0FF -> 0x1FF per the real polynomial sequence
  CHECK(cpu.timerCounter() == 0x1FF);
  cpu.step();                  // should dispatch to timer vector instead of executing the NOP
  CHECK(cpu.p() == 0xB000);
}

// Setting the counter mid-sequence (not just one step from the end) must
// resume the polynomial sequence at the right point -- i.e. AM0/AM1's
// reverse-table lookup, not just the single-step case above. TRM "count
// 500" is 0x178H, 11 steps before "count 511" (0x1FF).
void testTimerResumesSequenceFromArbitraryValue() {
  TestBus bus;
  lh5801::CPU cpu(bus);
  cpu.reset();
  cpu.setP(0x4000);
  bus.writeME0(0x4000, 0xB5);
  bus.writeME0(0x4001, 0x78);  // LDI A,78H
  bus.writeME0(0x4002, 0xFD);
  bus.writeME0(0x4003, 0xDE);  // AM1 (MSB=1) -- timerCounter = 0x178
  cpu.step();
  cpu.step();
  CHECK(cpu.timerCounter() == 0x178);

  for (int i = 0; i < 10; i++) cpu.tickTimer();
  CHECK(cpu.timerCounter() != 0x1FF);  // not there yet

  cpu.tickTimer();  // 11th tick after the set
  CHECK(cpu.timerCounter() == 0x1FF);
}

}  // namespace

int main() {
  testMiRequiresIe();
  testNmiIgnoresIe();
  testInterruptEntryAndRtiRoundTrip();
  testHaltWakesOnInterrupt();
  testPolyCounterTableIntegrity();
  testTimerParkedAtZeroNeverFires();
  testTimerInterruptFiresAt1FF();
  testTimerResumesSequenceFromArbitraryValue();

  if (g_failures == 0) {
    std::printf("All tests passed.\n");
    return 0;
  }
  std::printf("%d check(s) failed.\n", g_failures);
  return 1;
}
