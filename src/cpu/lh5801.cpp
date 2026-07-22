#include "lh5801.h"

// Every opcode byte, addressing mode, and flag-effect below is taken from
// docs/lh5801_opcode_reference.md. Comments below only note the handful of
// places where that doc itself flags an assumption or open question.

namespace lh5801 {

namespace {

// Decimal-adjust byte for DCA/DCS, per the manual's C/H -> DA table.
uint8_t daForAdd(bool c, bool h) {
  if (!c && !h) return 0x9A;
  if (!c && h) return 0xA0;
  if (c && !h) return 0xFA;
  return 0x00;
}

}  // namespace

void CPU::reset() {
  a_ = 0;
  x_ = y_ = u_ = 0;
  s_ = 0;
  flags_ = Flags();
  halted_ = false;
  bf_ = disp_ = pu_ = pv_ = false;
  miPending_ = nmiPending_ = timerInterruptPending_ = false;
  timerCounter_ = 0;
  // "The contents of the address FFFEH are transferred to the PH register
  // and the contents of FFFFH to the PL register." (manual 2-3-3 Reset)
  uint8_t hi = bus_.readME0(0xFFFE);
  uint8_t lo = bus_.readME0(0xFFFF);
  p_ = static_cast<uint16_t>((hi << 8) | lo);
}

void CPU::tickTimer() {
  timerCounter_ = static_cast<uint16_t>((timerCounter_ + 1) & 0x1FF);
  if (timerCounter_ == 0x1FF) timerInterruptPending_ = true;
}

// Entry sequence for MI/NMI/timer interrupts. Derived from RTI's documented
// pop order -- (S+1)->PH, (S+2)->PL, (S+3)->T -- which, since pops happen in
// the reverse order of pushes, means entry must push T first, then PL, then
// PH last (so PH is popped first by RTI, matching its own listed order).
void CPU::dispatchInterrupt(uint16_t vectorAddr, int& cycles) {
  uint8_t t = packFlags();
  uint8_t pl = static_cast<uint8_t>(p_ & 0xFF);
  uint8_t ph = static_cast<uint8_t>(p_ >> 8);
  push8(t);
  push8(pl);
  push8(ph);
  flags_.ie = false;  // disable further interrupts until RTI restores T
  uint8_t hi = bus_.readME0(vectorAddr);
  uint8_t lo = bus_.readME0(static_cast<uint16_t>(vectorAddr + 1));
  p_ = static_cast<uint16_t>((hi << 8) | lo);
  halted_ = false;
  // No documented cycle cost for interrupt acknowledgment; approximated as
  // RTI's cycle count (14), since the work done (3 pushes + vector fetch)
  // is symmetric with RTI's 3 pops + resume.
  cycles = 14;
}

uint16_t CPU::regR16(int rsel) const {
  switch (rsel) {
    case 0: return x_;
    case 1: return y_;
    default: return u_;
  }
}

void CPU::setRegR16(int rsel, uint16_t v) {
  switch (rsel) {
    case 0: x_ = v; break;
    case 1: y_ = v; break;
    default: u_ = v; break;
  }
}

uint8_t CPU::regRL(int rsel) const { return static_cast<uint8_t>(regR16(rsel) & 0xFF); }

void CPU::setRegRL(int rsel, uint8_t v) {
  uint16_t r = regR16(rsel);
  r = static_cast<uint16_t>((r & 0xFF00) | v);
  setRegR16(rsel, r);
}

uint8_t CPU::regRH(int rsel) const { return static_cast<uint8_t>(regR16(rsel) >> 8); }

void CPU::setRegRH(int rsel, uint8_t v) {
  uint16_t r = regR16(rsel);
  r = static_cast<uint16_t>((r & 0x00FF) | (static_cast<uint16_t>(v) << 8));
  setRegR16(rsel, r);
}

uint8_t CPU::fetch8() { return bus_.readME0(p_++); }

uint16_t CPU::fetch16() {
  uint8_t hi = fetch8();
  uint8_t lo = fetch8();
  return static_cast<uint16_t>((hi << 8) | lo);
}

void CPU::push8(uint8_t v) {
  bus_.writeME0(s_, v);
  s_ = static_cast<uint16_t>(s_ - 1);
}

uint8_t CPU::pop8() {
  s_ = static_cast<uint16_t>(s_ + 1);
  return bus_.readME0(s_);
}

void CPU::push16(uint16_t v) {
  // Low byte pushed first (ends up at the higher address), then high byte --
  // matches the manual's SJP/PSH-register operation listings exactly.
  push8(static_cast<uint8_t>(v & 0xFF));
  push8(static_cast<uint8_t>(v >> 8));
}

uint16_t CPU::pop16() {
  uint8_t hi = pop8();
  uint8_t lo = pop8();
  return static_cast<uint16_t>((hi << 8) | lo);
}

uint8_t CPU::doAdd(uint8_t opA, uint8_t opB, bool carryIn) {
  int sum = opA + opB + (carryIn ? 1 : 0);
  uint8_t result = static_cast<uint8_t>(sum);
  flags_.c = sum > 0xFF;
  flags_.h = ((opA & 0xF) + (opB & 0xF) + (carryIn ? 1 : 0)) > 0xF;
  bool signA = (opA & 0x80) != 0;
  bool signB = (opB & 0x80) != 0;
  bool signR = (result & 0x80) != 0;
  flags_.v = (signA == signB) && (signR != signA);
  flags_.z = (result == 0);
  return result;
}

// T register bit layout, per manual section 2-2-3: low-order 5 bits are
// H,V,Z,IE,C from left to right -- C in the 1s position, H in the 16s.
uint8_t CPU::packFlags() const {
  uint8_t v = 0;
  if (flags_.c) v |= 0x01;
  if (flags_.ie) v |= 0x02;
  if (flags_.z) v |= 0x04;
  if (flags_.v) v |= 0x08;
  if (flags_.h) v |= 0x10;
  return v;
}

void CPU::unpackFlags(uint8_t v) {
  flags_.c = (v & 0x01) != 0;
  flags_.ie = (v & 0x02) != 0;
  flags_.z = (v & 0x04) != 0;
  flags_.v = (v & 0x08) != 0;
  flags_.h = (v & 0x10) != 0;
}

int CPU::step() {
  // NMI always responds; MI and the timer interrupt require IE=1. All
  // three wake the CPU from HLT (dispatchInterrupt clears halted_).
  int cycles = 0;
  if (nmiPending_) {
    nmiPending_ = false;
    dispatchInterrupt(0xFFFC, cycles);
    return cycles;
  }
  if (flags_.ie && miPending_) {
    miPending_ = false;
    dispatchInterrupt(0xFFF8, cycles);
    return cycles;
  }
  if (flags_.ie && timerInterruptPending_) {
    timerInterruptPending_ = false;
    dispatchInterrupt(0xFFFA, cycles);
    return cycles;
  }
  if (halted_) return 0;
  uint8_t opcode = fetch8();
  if (opcode == 0xFD) {
    uint8_t sub = fetch8();
    execFD(sub, cycles);
  } else {
    execPrimary(opcode, cycles);
  }
  return cycles;
}

void CPU::execPrimary(uint8_t opcode, int& cycles) {
  switch (opcode) {
    // ---- SBC (ME0 register/indirect forms) ----
    case 0x00: a_ = doAdd(a_, static_cast<uint8_t>(~regRL(0)), flags_.c); cycles = 6; break;
    case 0x10: a_ = doAdd(a_, static_cast<uint8_t>(~regRL(1)), flags_.c); cycles = 6; break;
    case 0x20: a_ = doAdd(a_, static_cast<uint8_t>(~regRL(2)), flags_.c); cycles = 6; break;
    case 0x80: a_ = doAdd(a_, static_cast<uint8_t>(~regRH(0)), flags_.c); cycles = 6; break;
    case 0x90: a_ = doAdd(a_, static_cast<uint8_t>(~regRH(1)), flags_.c); cycles = 6; break;
    case 0xA0: a_ = doAdd(a_, static_cast<uint8_t>(~regRH(2)), flags_.c); cycles = 6; break;
    case 0x01: a_ = doAdd(a_, static_cast<uint8_t>(~bus_.readME0(regR16(0))), flags_.c); cycles = 7; break;
    case 0x11: a_ = doAdd(a_, static_cast<uint8_t>(~bus_.readME0(regR16(1))), flags_.c); cycles = 7; break;
    case 0x21: a_ = doAdd(a_, static_cast<uint8_t>(~bus_.readME0(regR16(2))), flags_.c); cycles = 7; break;
    case 0xA1: { uint16_t addr = fetch16(); a_ = doAdd(a_, static_cast<uint8_t>(~bus_.readME0(addr)), flags_.c); cycles = 13; break; }

    // ---- ADC ----
    case 0x02: a_ = doAdd(a_, regRL(0), flags_.c); cycles = 6; break;
    case 0x12: a_ = doAdd(a_, regRL(1), flags_.c); cycles = 6; break;
    case 0x22: a_ = doAdd(a_, regRL(2), flags_.c); cycles = 6; break;
    case 0x82: a_ = doAdd(a_, regRH(0), flags_.c); cycles = 6; break;
    case 0x92: a_ = doAdd(a_, regRH(1), flags_.c); cycles = 6; break;
    case 0xA2: a_ = doAdd(a_, regRH(2), flags_.c); cycles = 6; break;
    case 0x03: a_ = doAdd(a_, bus_.readME0(regR16(0)), flags_.c); cycles = 7; break;
    case 0x13: a_ = doAdd(a_, bus_.readME0(regR16(1)), flags_.c); cycles = 7; break;
    case 0x23: a_ = doAdd(a_, bus_.readME0(regR16(2)), flags_.c); cycles = 7; break;
    case 0xA3: { uint16_t addr = fetch16(); a_ = doAdd(a_, bus_.readME0(addr), flags_.c); cycles = 13; break; }

    // ---- LDA ----
    case 0x04: a_ = regRL(0); flags_.z = (a_ == 0); cycles = 5; break;
    case 0x14: a_ = regRL(1); flags_.z = (a_ == 0); cycles = 5; break;
    case 0x24: a_ = regRL(2); flags_.z = (a_ == 0); cycles = 5; break;
    case 0x84: a_ = regRH(0); flags_.z = (a_ == 0); cycles = 5; break;
    case 0x94: a_ = regRH(1); flags_.z = (a_ == 0); cycles = 5; break;
    case 0xA4: a_ = regRH(2); flags_.z = (a_ == 0); cycles = 5; break;
    case 0x05: a_ = bus_.readME0(regR16(0)); flags_.z = (a_ == 0); cycles = 6; break;
    case 0x15: a_ = bus_.readME0(regR16(1)); flags_.z = (a_ == 0); cycles = 6; break;
    case 0x25: a_ = bus_.readME0(regR16(2)); flags_.z = (a_ == 0); cycles = 6; break;
    case 0xA5: { uint16_t addr = fetch16(); a_ = bus_.readME0(addr); flags_.z = (a_ == 0); cycles = 12; break; }

    // ---- CPA ----
    case 0x06: doAdd(a_, static_cast<uint8_t>(~regRL(0)), true); cycles = 6; break;
    case 0x16: doAdd(a_, static_cast<uint8_t>(~regRL(1)), true); cycles = 6; break;
    case 0x26: doAdd(a_, static_cast<uint8_t>(~regRL(2)), true); cycles = 6; break;
    case 0x86: doAdd(a_, static_cast<uint8_t>(~regRH(0)), true); cycles = 6; break;
    case 0x96: doAdd(a_, static_cast<uint8_t>(~regRH(1)), true); cycles = 6; break;
    case 0xA6: doAdd(a_, static_cast<uint8_t>(~regRH(2)), true); cycles = 6; break;
    case 0x07: doAdd(a_, static_cast<uint8_t>(~bus_.readME0(regR16(0))), true); cycles = 7; break;
    case 0x17: doAdd(a_, static_cast<uint8_t>(~bus_.readME0(regR16(1))), true); cycles = 7; break;
    case 0x27: doAdd(a_, static_cast<uint8_t>(~bus_.readME0(regR16(2))), true); cycles = 7; break;
    case 0xA7: { uint16_t addr = fetch16(); doAdd(a_, static_cast<uint8_t>(~bus_.readME0(addr)), true); cycles = 13; break; }

    // ---- STA ----
    case 0x0A: setRegRL(0, a_); cycles = 5; break;
    case 0x1A: setRegRL(1, a_); cycles = 5; break;
    case 0x2A: setRegRL(2, a_); cycles = 5; break;
    case 0x08: setRegRH(0, a_); cycles = 5; break;
    case 0x18: setRegRH(1, a_); cycles = 5; break;
    case 0x28: setRegRH(2, a_); cycles = 5; break;
    case 0x0E: bus_.writeME0(regR16(0), a_); cycles = 6; break;
    case 0x1E: bus_.writeME0(regR16(1), a_); cycles = 6; break;
    case 0x2E: bus_.writeME0(regR16(2), a_); cycles = 6; break;
    case 0xAE: { uint16_t addr = fetch16(); bus_.writeME0(addr, a_); cycles = 12; break; }

    // ---- AND / ANI ----
    case 0x09: a_ &= bus_.readME0(regR16(0)); flags_.z = (a_ == 0); cycles = 7; break;
    case 0x19: a_ &= bus_.readME0(regR16(1)); flags_.z = (a_ == 0); cycles = 7; break;
    case 0x29: a_ &= bus_.readME0(regR16(2)); flags_.z = (a_ == 0); cycles = 7; break;
    case 0xA9: { uint16_t addr = fetch16(); a_ &= bus_.readME0(addr); flags_.z = (a_ == 0); cycles = 13; break; }
    case 0xB9: { uint8_t i = fetch8(); a_ &= i; flags_.z = (a_ == 0); cycles = 7; break; }
    case 0x49: { uint8_t i = fetch8(); uint16_t addr = regR16(0); uint8_t v = static_cast<uint8_t>(bus_.readME0(addr) & i); bus_.writeME0(addr, v); flags_.z = (v == 0); cycles = 13; break; }
    case 0x59: { uint8_t i = fetch8(); uint16_t addr = regR16(1); uint8_t v = static_cast<uint8_t>(bus_.readME0(addr) & i); bus_.writeME0(addr, v); flags_.z = (v == 0); cycles = 13; break; }
    case 0x69: { uint8_t i = fetch8(); uint16_t addr = regR16(2); uint8_t v = static_cast<uint8_t>(bus_.readME0(addr) & i); bus_.writeME0(addr, v); flags_.z = (v == 0); cycles = 13; break; }
    case 0xE9: { uint16_t addr = fetch16(); uint8_t i = fetch8(); uint8_t v = static_cast<uint8_t>(bus_.readME0(addr) & i); bus_.writeME0(addr, v); flags_.z = (v == 0); cycles = 19; break; }

    // ---- BII / BIT ----
    case 0xBF: { uint8_t i = fetch8(); flags_.z = ((a_ & i) == 0); cycles = 7; break; }
    case 0x4D: { uint8_t i = fetch8(); flags_.z = ((bus_.readME0(regR16(0)) & i) == 0); cycles = 10; break; }
    case 0x5D: { uint8_t i = fetch8(); flags_.z = ((bus_.readME0(regR16(1)) & i) == 0); cycles = 10; break; }
    case 0x6D: { uint8_t i = fetch8(); flags_.z = ((bus_.readME0(regR16(2)) & i) == 0); cycles = 10; break; }
    case 0xED: { uint16_t addr = fetch16(); uint8_t i = fetch8(); flags_.z = ((bus_.readME0(addr) & i) == 0); cycles = 16; break; }
    case 0x0F: flags_.z = ((a_ & bus_.readME0(regR16(0))) == 0); cycles = 7; break;
    case 0x1F: flags_.z = ((a_ & bus_.readME0(regR16(1))) == 0); cycles = 7; break;
    case 0x2F: flags_.z = ((a_ & bus_.readME0(regR16(2))) == 0); cycles = 7; break;
    case 0xAF: { uint16_t addr = fetch16(); flags_.z = ((a_ & bus_.readME0(addr)) == 0); cycles = 13; break; }

    // ---- ORA / ORI ----
    case 0x0B: a_ |= bus_.readME0(regR16(0)); flags_.z = (a_ == 0); cycles = 7; break;
    case 0x1B: a_ |= bus_.readME0(regR16(1)); flags_.z = (a_ == 0); cycles = 7; break;
    case 0x2B: a_ |= bus_.readME0(regR16(2)); flags_.z = (a_ == 0); cycles = 7; break;
    case 0xAB: { uint16_t addr = fetch16(); a_ |= bus_.readME0(addr); flags_.z = (a_ == 0); cycles = 13; break; }
    case 0xBB: { uint8_t i = fetch8(); a_ |= i; flags_.z = (a_ == 0); cycles = 7; break; }
    case 0x4B: { uint8_t i = fetch8(); uint16_t addr = regR16(0); uint8_t v = static_cast<uint8_t>(bus_.readME0(addr) | i); bus_.writeME0(addr, v); flags_.z = (v == 0); cycles = 13; break; }
    case 0x5B: { uint8_t i = fetch8(); uint16_t addr = regR16(1); uint8_t v = static_cast<uint8_t>(bus_.readME0(addr) | i); bus_.writeME0(addr, v); flags_.z = (v == 0); cycles = 13; break; }
    case 0x6B: { uint8_t i = fetch8(); uint16_t addr = regR16(2); uint8_t v = static_cast<uint8_t>(bus_.readME0(addr) | i); bus_.writeME0(addr, v); flags_.z = (v == 0); cycles = 13; break; }
    case 0xEB: { uint16_t addr = fetch16(); uint8_t i = fetch8(); uint8_t v = static_cast<uint8_t>(bus_.readME0(addr) | i); bus_.writeME0(addr, v); flags_.z = (v == 0); cycles = 19; break; }

    // ---- EOR / EAI ----
    case 0x0D: a_ ^= bus_.readME0(regR16(0)); flags_.z = (a_ == 0); cycles = 7; break;
    case 0x1D: a_ ^= bus_.readME0(regR16(1)); flags_.z = (a_ == 0); cycles = 7; break;
    case 0x2D: a_ ^= bus_.readME0(regR16(2)); flags_.z = (a_ == 0); cycles = 7; break;
    case 0xAD: { uint16_t addr = fetch16(); a_ ^= bus_.readME0(addr); flags_.z = (a_ == 0); cycles = 13; break; }
    case 0xBD: { uint8_t i = fetch8(); a_ ^= i; flags_.z = (a_ == 0); cycles = 7; break; }

    // ---- CPI ----
    case 0xB7: { uint8_t i = fetch8(); doAdd(a_, static_cast<uint8_t>(~i), true); cycles = 7; break; }
    case 0x4E: { uint8_t i = fetch8(); doAdd(regRL(0), static_cast<uint8_t>(~i), true); cycles = 7; break; }
    case 0x5E: { uint8_t i = fetch8(); doAdd(regRL(1), static_cast<uint8_t>(~i), true); cycles = 7; break; }
    case 0x6E: { uint8_t i = fetch8(); doAdd(regRL(2), static_cast<uint8_t>(~i), true); cycles = 7; break; }
    case 0x4C: { uint8_t i = fetch8(); doAdd(regRH(0), static_cast<uint8_t>(~i), true); cycles = 7; break; }
    case 0x5C: { uint8_t i = fetch8(); doAdd(regRH(1), static_cast<uint8_t>(~i), true); cycles = 7; break; }
    case 0x6C: { uint8_t i = fetch8(); doAdd(regRH(2), static_cast<uint8_t>(~i), true); cycles = 7; break; }

    // ---- ADI ----
    case 0xB3: { uint8_t i = fetch8(); a_ = doAdd(a_, i, flags_.c); cycles = 7; break; }
    case 0x4F: { uint8_t i = fetch8(); uint16_t addr = regR16(0); uint8_t v = doAdd(bus_.readME0(addr), i, false); bus_.writeME0(addr, v); cycles = 13; break; }
    case 0x5F: { uint8_t i = fetch8(); uint16_t addr = regR16(1); uint8_t v = doAdd(bus_.readME0(addr), i, false); bus_.writeME0(addr, v); cycles = 13; break; }
    case 0x6F: { uint8_t i = fetch8(); uint16_t addr = regR16(2); uint8_t v = doAdd(bus_.readME0(addr), i, false); bus_.writeME0(addr, v); cycles = 13; break; }
    case 0xEF: { uint16_t addr = fetch16(); uint8_t i = fetch8(); uint8_t v = doAdd(bus_.readME0(addr), i, false); bus_.writeME0(addr, v); cycles = 19; break; }

    // ---- SBI ----
    case 0xB1: { uint8_t i = fetch8(); a_ = doAdd(a_, static_cast<uint8_t>(~i), flags_.c); cycles = 7; break; }

    // ---- DCA / DCS ----
    case 0x8C: { uint8_t op = bus_.readME0(regR16(0)); a_ = static_cast<uint8_t>(a_ + 0x66); a_ = doAdd(a_, op, flags_.c); a_ = static_cast<uint8_t>(a_ + daForAdd(flags_.c, flags_.h)); cycles = 15; break; }
    case 0x9C: { uint8_t op = bus_.readME0(regR16(1)); a_ = static_cast<uint8_t>(a_ + 0x66); a_ = doAdd(a_, op, flags_.c); a_ = static_cast<uint8_t>(a_ + daForAdd(flags_.c, flags_.h)); cycles = 15; break; }
    case 0xAC: { uint8_t op = bus_.readME0(regR16(2)); a_ = static_cast<uint8_t>(a_ + 0x66); a_ = doAdd(a_, op, flags_.c); a_ = static_cast<uint8_t>(a_ + daForAdd(flags_.c, flags_.h)); cycles = 15; break; }
    case 0x0C: { uint8_t op = bus_.readME0(regR16(0)); a_ = doAdd(a_, static_cast<uint8_t>(~op), flags_.c); a_ = static_cast<uint8_t>(a_ + daForAdd(flags_.c, flags_.h)); cycles = 13; break; }
    case 0x1C: { uint8_t op = bus_.readME0(regR16(1)); a_ = doAdd(a_, static_cast<uint8_t>(~op), flags_.c); a_ = static_cast<uint8_t>(a_ + daForAdd(flags_.c, flags_.h)); cycles = 13; break; }
    case 0x2C: { uint8_t op = bus_.readME0(regR16(2)); a_ = doAdd(a_, static_cast<uint8_t>(~op), flags_.c); a_ = static_cast<uint8_t>(a_ + daForAdd(flags_.c, flags_.h)); cycles = 13; break; }

    // ---- INC / DEC ----
    case 0xDD: a_ = doAdd(a_, 1, false); cycles = 5; break;
    case 0x40: setRegRL(0, doAdd(regRL(0), 1, false)); cycles = 5; break;
    case 0x50: setRegRL(1, doAdd(regRL(1), 1, false)); cycles = 5; break;
    case 0x60: setRegRL(2, doAdd(regRL(2), 1, false)); cycles = 5; break;
    case 0x44: setRegR16(0, static_cast<uint16_t>(regR16(0) + 1)); cycles = 5; break;
    case 0x54: setRegR16(1, static_cast<uint16_t>(regR16(1) + 1)); cycles = 5; break;
    case 0x64: setRegR16(2, static_cast<uint16_t>(regR16(2) + 1)); cycles = 5; break;
    case 0xDF: a_ = doAdd(a_, static_cast<uint8_t>(~1), true); cycles = 5; break;
    case 0x42: setRegRL(0, doAdd(regRL(0), static_cast<uint8_t>(~1), true)); cycles = 5; break;
    case 0x52: setRegRL(1, doAdd(regRL(1), static_cast<uint8_t>(~1), true)); cycles = 5; break;
    case 0x62: setRegRL(2, doAdd(regRL(2), static_cast<uint8_t>(~1), true)); cycles = 5; break;
    case 0x46: setRegR16(0, static_cast<uint16_t>(regR16(0) - 1)); cycles = 5; break;
    case 0x56: setRegR16(1, static_cast<uint16_t>(regR16(1) - 1)); cycles = 5; break;
    case 0x66: setRegR16(2, static_cast<uint16_t>(regR16(2) - 1)); cycles = 5; break;

    // ---- DRL / DRR (X only in primary form) ----
    // Reconstructed from the manual's worked numeric examples rather than
    // its (harder to parse unambiguously) prose description -- moderate
    // confidence, not independently re-verified against the source images
    // in this session. Worth a dedicated recheck before relying on these
    // for anything BCD-display-critical.
    case 0xD7: {
      uint16_t addr = regR16(0);
      uint8_t memOld = bus_.readME0(addr);
      uint8_t aOld = a_;
      a_ = memOld;
      uint8_t memNew = static_cast<uint8_t>(((memOld & 0x0F) << 4) | ((aOld >> 4) & 0x0F));
      bus_.writeME0(addr, memNew);
      cycles = 12;
      break;
    }
    case 0xD3: {
      uint16_t addr = regR16(0);
      uint8_t memOld = bus_.readME0(addr);
      a_ = memOld;
      uint8_t memNew = static_cast<uint8_t>(((memOld & 0x0F) << 4) | ((memOld >> 4) & 0x0F));
      bus_.writeME0(addr, memNew);
      cycles = 12;
      break;
    }

    // ---- AEX ----
    case 0xF1: a_ = static_cast<uint8_t>(((a_ & 0x0F) << 4) | ((a_ >> 4) & 0x0F)); cycles = 6; break;

    // ---- Rotate / shift ----
    case 0xDB: { bool oldC = flags_.c; bool newC = (a_ & 0x80) != 0; a_ = static_cast<uint8_t>((a_ << 1) | (oldC ? 1 : 0)); flags_.c = newC; flags_.z = (a_ == 0); cycles = 8; break; }
    case 0xD1: { bool oldC = flags_.c; bool newC = (a_ & 0x01) != 0; a_ = static_cast<uint8_t>((a_ >> 1) | (oldC ? 0x80 : 0)); flags_.c = newC; flags_.z = (a_ == 0); cycles = 9; break; }
    case 0xD9: { bool newC = (a_ & 0x80) != 0; a_ = static_cast<uint8_t>(a_ << 1); flags_.c = newC; flags_.z = (a_ == 0); cycles = 6; break; }
    case 0xD5: { bool newC = (a_ & 0x01) != 0; a_ = static_cast<uint8_t>(a_ >> 1); flags_.c = newC; flags_.z = (a_ == 0); cycles = 9; break; }

    // ---- Flip-flops / CPU control ----
    case 0xE3: pu_ = false; cycles = 4; break;
    case 0xE1: pu_ = true; cycles = 4; break;
    case 0xB8: pv_ = false; cycles = 4; break;
    case 0xA8: pv_ = true; cycles = 4; break;
    case 0xFB: flags_.c = true; cycles = 4; break;
    case 0xF9: flags_.c = false; cycles = 4; break;
    case 0x38: cycles = 5; break;  // NOP

    // ---- LDE / LIN / SDE / SIN ----
    case 0x47: a_ = bus_.readME0(regR16(0)); flags_.z = (a_ == 0); setRegR16(0, static_cast<uint16_t>(regR16(0) - 1)); cycles = 6; break;
    case 0x57: a_ = bus_.readME0(regR16(1)); flags_.z = (a_ == 0); setRegR16(1, static_cast<uint16_t>(regR16(1) - 1)); cycles = 6; break;
    case 0x67: a_ = bus_.readME0(regR16(2)); flags_.z = (a_ == 0); setRegR16(2, static_cast<uint16_t>(regR16(2) - 1)); cycles = 6; break;
    case 0x45: a_ = bus_.readME0(regR16(0)); flags_.z = (a_ == 0); setRegR16(0, static_cast<uint16_t>(regR16(0) + 1)); cycles = 6; break;
    case 0x55: a_ = bus_.readME0(regR16(1)); flags_.z = (a_ == 0); setRegR16(1, static_cast<uint16_t>(regR16(1) + 1)); cycles = 6; break;
    case 0x65: a_ = bus_.readME0(regR16(2)); flags_.z = (a_ == 0); setRegR16(2, static_cast<uint16_t>(regR16(2) + 1)); cycles = 6; break;
    case 0x43: bus_.writeME0(regR16(0), a_); setRegR16(0, static_cast<uint16_t>(regR16(0) - 1)); cycles = 6; break;
    case 0x53: bus_.writeME0(regR16(1), a_); setRegR16(1, static_cast<uint16_t>(regR16(1) - 1)); cycles = 6; break;
    case 0x63: bus_.writeME0(regR16(2), a_); setRegR16(2, static_cast<uint16_t>(regR16(2) - 1)); cycles = 6; break;
    case 0x41: bus_.writeME0(regR16(0), a_); setRegR16(0, static_cast<uint16_t>(regR16(0) + 1)); cycles = 6; break;
    case 0x51: bus_.writeME0(regR16(1), a_); setRegR16(1, static_cast<uint16_t>(regR16(1) + 1)); cycles = 6; break;
    case 0x61: bus_.writeME0(regR16(2), a_); setRegR16(2, static_cast<uint16_t>(regR16(2) + 1)); cycles = 6; break;

    // ---- LDI ----
    case 0xB5: { uint8_t i = fetch8(); a_ = i; flags_.z = (a_ == 0); cycles = 6; break; }
    case 0x4A: { uint8_t i = fetch8(); setRegRL(0, i); cycles = 6; break; }
    case 0x5A: { uint8_t i = fetch8(); setRegRL(1, i); cycles = 6; break; }
    case 0x6A: { uint8_t i = fetch8(); setRegRL(2, i); cycles = 6; break; }
    case 0x48: { uint8_t i = fetch8(); setRegRH(0, i); cycles = 5; break; }
    case 0x58: { uint8_t i = fetch8(); setRegRH(1, i); cycles = 5; break; }
    case 0x68: { uint8_t i = fetch8(); setRegRH(2, i); cycles = 5; break; }
    case 0xAA: { s_ = fetch16(); cycles = 12; break; }

    // ---- Jumps / calls / returns ----
    case 0xBA: { p_ = fetch16(); cycles = 12; break; }
    case 0xBE: { uint16_t addr = fetch16(); push16(p_); p_ = addr; cycles = 19; break; }
    case 0x9A: { p_ = pop16(); cycles = 11; break; }
    case 0x8A: { p_ = pop16(); uint8_t t = pop8(); unpackFlags(t); cycles = 14; break; }

    // ---- TIN / CIN / LOP ----
    case 0xF5: {
      uint16_t xa = regR16(0);
      uint16_t ya = regR16(1);
      uint8_t v = bus_.readME0(xa);
      bus_.writeME0(ya, v);
      setRegR16(0, static_cast<uint16_t>(xa + 1));
      setRegR16(1, static_cast<uint16_t>(ya + 1));
      cycles = 7;
      break;
    }
    case 0xF7: {
      uint16_t addr = regR16(0);
      doAdd(a_, static_cast<uint8_t>(~bus_.readME0(addr)), true);
      setRegR16(0, static_cast<uint16_t>(addr + 1));
      cycles = 7;
      break;
    }
    case 0x88: {
      uint8_t i = fetch8();
      uint8_t oldUL = regRL(2);
      setRegRL(2, static_cast<uint8_t>(oldUL - 1));
      bool borrow = (oldUL == 0);
      if (!borrow) { p_ = static_cast<uint16_t>(p_ - i); cycles = 11; } else { cycles = 8; }
      break;
    }

    // ---- Branches: BCH (unconditional) ----
    case 0x8E: { uint8_t i = fetch8(); p_ = static_cast<uint16_t>(p_ + i); cycles = 8; break; }
    case 0x9E: { uint8_t i = fetch8(); p_ = static_cast<uint16_t>(p_ - i); cycles = 9; break; }

    // ---- Branches: BCS (C=1) / BCR (C=0) ----
    case 0x83: { uint8_t i = fetch8(); if (flags_.c) { p_ = static_cast<uint16_t>(p_ + i); cycles = 10; } else { cycles = 8; } break; }
    case 0x93: { uint8_t i = fetch8(); if (flags_.c) { p_ = static_cast<uint16_t>(p_ - i); cycles = 11; } else { cycles = 8; } break; }
    case 0x81: { uint8_t i = fetch8(); if (!flags_.c) { p_ = static_cast<uint16_t>(p_ + i); cycles = 10; } else { cycles = 8; } break; }
    case 0x91: { uint8_t i = fetch8(); if (!flags_.c) { p_ = static_cast<uint16_t>(p_ - i); cycles = 11; } else { cycles = 8; } break; }

    // ---- Branches: BHS (H=1) / BHR (H=0) ----
    case 0x87: { uint8_t i = fetch8(); if (flags_.h) { p_ = static_cast<uint16_t>(p_ + i); cycles = 10; } else { cycles = 8; } break; }
    case 0x97: { uint8_t i = fetch8(); if (flags_.h) { p_ = static_cast<uint16_t>(p_ - i); cycles = 11; } else { cycles = 8; } break; }
    case 0x85: { uint8_t i = fetch8(); if (!flags_.h) { p_ = static_cast<uint16_t>(p_ + i); cycles = 10; } else { cycles = 8; } break; }
    case 0x95: { uint8_t i = fetch8(); if (!flags_.h) { p_ = static_cast<uint16_t>(p_ - i); cycles = 11; } else { cycles = 8; } break; }

    // ---- Branches: BVS (V=1) / BVR (V=0) ----
    case 0x8F: { uint8_t i = fetch8(); if (flags_.v) { p_ = static_cast<uint16_t>(p_ + i); cycles = 10; } else { cycles = 8; } break; }
    case 0x9F: { uint8_t i = fetch8(); if (flags_.v) { p_ = static_cast<uint16_t>(p_ - i); cycles = 11; } else { cycles = 8; } break; }
    case 0x8D: { uint8_t i = fetch8(); if (!flags_.v) { p_ = static_cast<uint16_t>(p_ + i); cycles = 10; } else { cycles = 8; } break; }
    case 0x9D: { uint8_t i = fetch8(); if (!flags_.v) { p_ = static_cast<uint16_t>(p_ - i); cycles = 11; } else { cycles = 8; } break; }

    // ---- Branches: BZS (Z=1) / BZR (Z=0) ----
    case 0x8B: { uint8_t i = fetch8(); if (flags_.z) { p_ = static_cast<uint16_t>(p_ + i); cycles = 10; } else { cycles = 8; } break; }
    case 0x9B: { uint8_t i = fetch8(); if (flags_.z) { p_ = static_cast<uint16_t>(p_ - i); cycles = 11; } else { cycles = 8; } break; }
    case 0x89: { uint8_t i = fetch8(); if (!flags_.z) { p_ = static_cast<uint16_t>(p_ + i); cycles = 10; } else { cycles = 8; } break; }
    case 0x99: { uint8_t i = fetch8(); if (!flags_.z) { p_ = static_cast<uint16_t>(p_ - i); cycles = 11; } else { cycles = 8; } break; }

    // ---- VEJ: one-byte vector call, FF00+opcode ----
    case 0xC0: case 0xC2: case 0xC4: case 0xC6: case 0xC8: case 0xCA: case 0xCC: case 0xCE:
    case 0xD0: case 0xD2: case 0xD4: case 0xD6: case 0xD8: case 0xDA: case 0xDC: case 0xDE:
    case 0xE0: case 0xE2: case 0xE4: case 0xE6: case 0xE8: case 0xEA: case 0xEC: case 0xEE:
    case 0xF0: case 0xF2: case 0xF4: case 0xF6: {
      push16(p_);
      p_ = static_cast<uint16_t>(0xFF00 | opcode);
      flags_.z = false;
      cycles = 17;
      break;
    }

    // ---- VMJ / VCS / VCR / VHS / VHR / VZS / VZR / VVS ----
    case 0xCD: { uint8_t i = fetch8(); push16(p_); p_ = static_cast<uint16_t>(0xFF00 | i); flags_.z = false; cycles = 20; break; }
    case 0xC3: { uint8_t i = fetch8(); if (flags_.c) { push16(p_); p_ = static_cast<uint16_t>(0xFF00 | i); flags_.z = false; cycles = 21; } else { cycles = 8; } break; }
    case 0xC1: { uint8_t i = fetch8(); if (!flags_.c) { push16(p_); p_ = static_cast<uint16_t>(0xFF00 | i); flags_.z = false; cycles = 21; } else { cycles = 8; } break; }
    case 0xC7: { uint8_t i = fetch8(); if (flags_.h) { push16(p_); p_ = static_cast<uint16_t>(0xFF00 | i); flags_.z = false; cycles = 21; } else { cycles = 8; } break; }
    case 0xC5: { uint8_t i = fetch8(); if (!flags_.h) { push16(p_); p_ = static_cast<uint16_t>(0xFF00 | i); flags_.z = false; cycles = 21; } else { cycles = 8; } break; }
    case 0xCB: { uint8_t i = fetch8(); if (flags_.z) { push16(p_); p_ = static_cast<uint16_t>(0xFF00 | i); flags_.z = false; cycles = 21; } else { cycles = 8; } break; }
    case 0xC9: { uint8_t i = fetch8(); if (!flags_.z) { push16(p_); p_ = static_cast<uint16_t>(0xFF00 | i); flags_.z = false; cycles = 21; } else { cycles = 8; } break; }
    case 0xCF: { uint8_t i = fetch8(); if (flags_.v) { push16(p_); p_ = static_cast<uint16_t>(0xFF00 | i); flags_.z = false; cycles = 21; } else { cycles = 8; } break; }

    default:
      // Undocumented/unused opcode byte. Real hardware behavior for these
      // is unspecified; treat as a 1-cycle no-op.
      cycles = 1;
      break;
  }
}

void CPU::execFD(uint8_t opcode, int& cycles) {
  switch (opcode) {
    // ---- ADC #(R)/#(ab) ----
    case 0x03: a_ = doAdd(a_, bus_.readME1(regR16(0)), flags_.c); cycles = 11; break;
    case 0x13: a_ = doAdd(a_, bus_.readME1(regR16(1)), flags_.c); cycles = 11; break;
    case 0x23: a_ = doAdd(a_, bus_.readME1(regR16(2)), flags_.c); cycles = 11; break;
    case 0xA3: { uint16_t addr = fetch16(); a_ = doAdd(a_, bus_.readME1(addr), flags_.c); cycles = 17; break; }

    // ---- SBC #(R)/#(ab) ----
    case 0x01: a_ = doAdd(a_, static_cast<uint8_t>(~bus_.readME1(regR16(0))), flags_.c); cycles = 13; break;
    case 0x11: a_ = doAdd(a_, static_cast<uint8_t>(~bus_.readME1(regR16(1))), flags_.c); cycles = 13; break;
    case 0x21: a_ = doAdd(a_, static_cast<uint8_t>(~bus_.readME1(regR16(2))), flags_.c); cycles = 13; break;
    case 0xA1: { uint16_t addr = fetch16(); a_ = doAdd(a_, static_cast<uint8_t>(~bus_.readME1(addr)), flags_.c); cycles = 17; break; }

    // ---- CPA #(R)/#(ab) ----
    case 0x07: doAdd(a_, static_cast<uint8_t>(~bus_.readME1(regR16(0))), true); cycles = 11; break;
    case 0x17: doAdd(a_, static_cast<uint8_t>(~bus_.readME1(regR16(1))), true); cycles = 11; break;
    case 0x27: doAdd(a_, static_cast<uint8_t>(~bus_.readME1(regR16(2))), true); cycles = 11; break;
    case 0xA7: { uint16_t addr = fetch16(); doAdd(a_, static_cast<uint8_t>(~bus_.readME1(addr)), true); cycles = 17; break; }

    // ---- LDA #(R)/#(ab) ----
    case 0x05: a_ = bus_.readME1(regR16(0)); flags_.z = (a_ == 0); cycles = 10; break;
    case 0x15: a_ = bus_.readME1(regR16(1)); flags_.z = (a_ == 0); cycles = 10; break;
    case 0x25: a_ = bus_.readME1(regR16(2)); flags_.z = (a_ == 0); cycles = 10; break;
    case 0xA5: { uint16_t addr = fetch16(); a_ = bus_.readME1(addr); flags_.z = (a_ == 0); cycles = 16; break; }

    // ---- STA #(R)/#(ab) ----
    case 0x0E: bus_.writeME1(regR16(0), a_); cycles = 10; break;
    case 0x1E: bus_.writeME1(regR16(1), a_); cycles = 10; break;
    case 0x2E: bus_.writeME1(regR16(2), a_); cycles = 10; break;
    case 0xAE: { uint16_t addr = fetch16(); bus_.writeME1(addr, a_); cycles = 16; break; }

    // ---- AND #(R)/#(ab), ANI #(R)/#(ab) ----
    case 0x09: a_ &= bus_.readME1(regR16(0)); flags_.z = (a_ == 0); cycles = 11; break;
    case 0x19: a_ &= bus_.readME1(regR16(1)); flags_.z = (a_ == 0); cycles = 11; break;
    case 0x29: a_ &= bus_.readME1(regR16(2)); flags_.z = (a_ == 0); cycles = 11; break;
    case 0xA9: { uint16_t addr = fetch16(); a_ &= bus_.readME1(addr); flags_.z = (a_ == 0); cycles = 17; break; }
    case 0x49: { uint8_t i = fetch8(); uint16_t addr = regR16(0); uint8_t v = static_cast<uint8_t>(bus_.readME1(addr) & i); bus_.writeME1(addr, v); flags_.z = (v == 0); cycles = 17; break; }
    case 0x59: { uint8_t i = fetch8(); uint16_t addr = regR16(1); uint8_t v = static_cast<uint8_t>(bus_.readME1(addr) & i); bus_.writeME1(addr, v); flags_.z = (v == 0); cycles = 17; break; }
    case 0x69: { uint8_t i = fetch8(); uint16_t addr = regR16(2); uint8_t v = static_cast<uint8_t>(bus_.readME1(addr) & i); bus_.writeME1(addr, v); flags_.z = (v == 0); cycles = 17; break; }
    case 0xE9: { uint16_t addr = fetch16(); uint8_t i = fetch8(); uint8_t v = static_cast<uint8_t>(bus_.readME1(addr) & i); bus_.writeME1(addr, v); flags_.z = (v == 0); cycles = 23; break; }

    // ---- BII #(R)/#(ab), BIT #(R)/#(ab) ----
    case 0x4D: { uint8_t i = fetch8(); flags_.z = ((bus_.readME1(regR16(0)) & i) == 0); cycles = 14; break; }
    case 0x5D: { uint8_t i = fetch8(); flags_.z = ((bus_.readME1(regR16(1)) & i) == 0); cycles = 14; break; }
    case 0x6D: { uint8_t i = fetch8(); flags_.z = ((bus_.readME1(regR16(2)) & i) == 0); cycles = 14; break; }
    case 0xED: { uint16_t addr = fetch16(); uint8_t i = fetch8(); flags_.z = ((bus_.readME1(addr) & i) == 0); cycles = 20; break; }
    case 0x0F: flags_.z = ((a_ & bus_.readME1(regR16(0))) == 0); cycles = 11; break;
    case 0x1F: flags_.z = ((a_ & bus_.readME1(regR16(1))) == 0); cycles = 11; break;
    case 0x2F: flags_.z = ((a_ & bus_.readME1(regR16(2))) == 0); cycles = 11; break;
    case 0xAF: { uint16_t addr = fetch16(); flags_.z = ((a_ & bus_.readME1(addr)) == 0); cycles = 17; break; }

    // ---- ORA #(R)/#(ab), ORI #(R)/#(ab) ----
    case 0x0B: a_ |= bus_.readME1(regR16(0)); flags_.z = (a_ == 0); cycles = 11; break;
    case 0x1B: a_ |= bus_.readME1(regR16(1)); flags_.z = (a_ == 0); cycles = 11; break;
    case 0x2B: a_ |= bus_.readME1(regR16(2)); flags_.z = (a_ == 0); cycles = 11; break;
    case 0xAB: { uint16_t addr = fetch16(); a_ |= bus_.readME1(addr); flags_.z = (a_ == 0); cycles = 17; break; }
    case 0x4B: { uint8_t i = fetch8(); uint16_t addr = regR16(0); uint8_t v = static_cast<uint8_t>(bus_.readME1(addr) | i); bus_.writeME1(addr, v); flags_.z = (v == 0); cycles = 17; break; }
    case 0x5B: { uint8_t i = fetch8(); uint16_t addr = regR16(1); uint8_t v = static_cast<uint8_t>(bus_.readME1(addr) | i); bus_.writeME1(addr, v); flags_.z = (v == 0); cycles = 17; break; }
    case 0x6B: { uint8_t i = fetch8(); uint16_t addr = regR16(2); uint8_t v = static_cast<uint8_t>(bus_.readME1(addr) | i); bus_.writeME1(addr, v); flags_.z = (v == 0); cycles = 17; break; }
    case 0xEB: { uint16_t addr = fetch16(); uint8_t i = fetch8(); uint8_t v = static_cast<uint8_t>(bus_.readME1(addr) | i); bus_.writeME1(addr, v); flags_.z = (v == 0); cycles = 23; break; }

    // ---- EOR #(R)/#(ab) ----
    case 0x0D: a_ ^= bus_.readME1(regR16(0)); flags_.z = (a_ == 0); cycles = 11; break;
    case 0x1D: a_ ^= bus_.readME1(regR16(1)); flags_.z = (a_ == 0); cycles = 11; break;
    case 0x2D: a_ ^= bus_.readME1(regR16(2)); flags_.z = (a_ == 0); cycles = 11; break;
    case 0xAD: { uint16_t addr = fetch16(); a_ ^= bus_.readME1(addr); flags_.z = (a_ == 0); cycles = 17; break; }

    // ---- ADI #(R)/#(ab) ----
    case 0x4F: { uint8_t i = fetch8(); uint16_t addr = regR16(0); uint8_t v = doAdd(bus_.readME1(addr), i, false); bus_.writeME1(addr, v); cycles = 17; break; }
    case 0x5F: { uint8_t i = fetch8(); uint16_t addr = regR16(1); uint8_t v = doAdd(bus_.readME1(addr), i, false); bus_.writeME1(addr, v); cycles = 17; break; }
    case 0x6F: { uint8_t i = fetch8(); uint16_t addr = regR16(2); uint8_t v = doAdd(bus_.readME1(addr), i, false); bus_.writeME1(addr, v); cycles = 17; break; }
    case 0xEF: { uint16_t addr = fetch16(); uint8_t i = fetch8(); uint8_t v = doAdd(bus_.readME1(addr), i, false); bus_.writeME1(addr, v); cycles = 23; break; }

    // ---- ADR ----
    case 0xCA: { uint8_t newRl = doAdd(regRL(0), a_, false); setRegRL(0, newRl); if (flags_.c) setRegRH(0, static_cast<uint8_t>(regRH(0) + 1)); cycles = 11; break; }
    case 0xDA: { uint8_t newRl = doAdd(regRL(1), a_, false); setRegRL(1, newRl); if (flags_.c) setRegRH(1, static_cast<uint8_t>(regRH(1) + 1)); cycles = 11; break; }
    case 0xEA: { uint8_t newRl = doAdd(regRL(2), a_, false); setRegRL(2, newRl); if (flags_.c) setRegRH(2, static_cast<uint8_t>(regRH(2) + 1)); cycles = 11; break; }

    // ---- DCA / DCS #(R) ----
    case 0x8C: { uint8_t op = bus_.readME1(regR16(0)); a_ = static_cast<uint8_t>(a_ + 0x66); a_ = doAdd(a_, op, flags_.c); a_ = static_cast<uint8_t>(a_ + daForAdd(flags_.c, flags_.h)); cycles = 19; break; }
    case 0x9C: { uint8_t op = bus_.readME1(regR16(1)); a_ = static_cast<uint8_t>(a_ + 0x66); a_ = doAdd(a_, op, flags_.c); a_ = static_cast<uint8_t>(a_ + daForAdd(flags_.c, flags_.h)); cycles = 19; break; }
    case 0xAC: { uint8_t op = bus_.readME1(regR16(2)); a_ = static_cast<uint8_t>(a_ + 0x66); a_ = doAdd(a_, op, flags_.c); a_ = static_cast<uint8_t>(a_ + daForAdd(flags_.c, flags_.h)); cycles = 19; break; }
    case 0x0C: { uint8_t op = bus_.readME1(regR16(0)); a_ = doAdd(a_, static_cast<uint8_t>(~op), flags_.c); a_ = static_cast<uint8_t>(a_ + daForAdd(flags_.c, flags_.h)); cycles = 17; break; }
    case 0x1C: { uint8_t op = bus_.readME1(regR16(1)); a_ = doAdd(a_, static_cast<uint8_t>(~op), flags_.c); a_ = static_cast<uint8_t>(a_ + daForAdd(flags_.c, flags_.h)); cycles = 17; break; }
    case 0x2C: { uint8_t op = bus_.readME1(regR16(2)); a_ = doAdd(a_, static_cast<uint8_t>(~op), flags_.c); a_ = static_cast<uint8_t>(a_ + daForAdd(flags_.c, flags_.h)); cycles = 17; break; }

    // ---- INC/DEC XH/YH/UH ----
    case 0x40: setRegRH(0, doAdd(regRH(0), 1, false)); cycles = 9; break;
    case 0x50: setRegRH(1, doAdd(regRH(1), 1, false)); cycles = 9; break;
    case 0x60: setRegRH(2, doAdd(regRH(2), 1, false)); cycles = 9; break;
    case 0x42: setRegRH(0, doAdd(regRH(0), static_cast<uint8_t>(~1), true)); cycles = 9; break;
    case 0x52: setRegRH(1, doAdd(regRH(1), static_cast<uint8_t>(~1), true)); cycles = 9; break;
    case 0x62: setRegRH(2, doAdd(regRH(2), static_cast<uint8_t>(~1), true)); cycles = 9; break;

    // ---- DRL / DRR #(X) ---- (see primary-form comment re: confidence)
    case 0xD7: {
      uint16_t addr = regR16(0);
      uint8_t memOld = bus_.readME1(addr);
      uint8_t aOld = a_;
      a_ = memOld;
      uint8_t memNew = static_cast<uint8_t>(((memOld & 0x0F) << 4) | ((aOld >> 4) & 0x0F));
      bus_.writeME1(addr, memNew);
      cycles = 16;
      break;
    }
    case 0xD3: {
      uint16_t addr = regR16(0);
      uint8_t memOld = bus_.readME1(addr);
      a_ = memOld;
      uint8_t memNew = static_cast<uint8_t>(((memOld & 0x0F) << 4) | ((memOld >> 4) & 0x0F));
      bus_.writeME1(addr, memNew);
      cycles = 16;
      break;
    }

    // ---- LDX (source R -> X) ----
    case 0x08: x_ = x_; cycles = 11; break;
    case 0x18: x_ = y_; cycles = 11; break;
    case 0x28: x_ = u_; cycles = 11; break;
    case 0x48: x_ = s_; cycles = 11; break;
    case 0x58: x_ = p_; cycles = 11; break;

    // ---- STX (X -> destination R) ----
    case 0x4A: x_ = x_; cycles = 11; break;
    case 0x5A: y_ = x_; cycles = 11; break;
    case 0x6A: u_ = x_; cycles = 11; break;
    case 0x4E: s_ = x_; cycles = 11; break;
    case 0x5E: p_ = x_; cycles = 11; break;

    // ---- PSH / POP ----
    case 0xC8: push8(a_); cycles = 11; break;
    case 0x88: push16(x_); cycles = 14; break;
    case 0x98: push16(y_); cycles = 14; break;
    case 0xA8: push16(u_); cycles = 14; break;
    case 0x8A: a_ = pop8(); flags_.z = (a_ == 0); cycles = 12; break;
    case 0x0A: x_ = pop16(); cycles = 15; break;
    case 0x1A: y_ = pop16(); cycles = 15; break;
    case 0x2A: u_ = pop16(); cycles = 15; break;

    // ---- ATT / TTA ----
    case 0xEC: unpackFlags(a_); cycles = 9; break;
    case 0xAA: { a_ = packFlags(); flags_.z = (a_ == 0); cycles = 9; break; }

    // ---- AM0 / AM1: load the 9-bit timer counter from A ----
    case 0xCE: timerCounter_ = static_cast<uint16_t>(a_); cycles = 9; break;        // AM0: MSB=0
    case 0xDE: timerCounter_ = static_cast<uint16_t>(a_ | 0x100); cycles = 9; break; // AM1: MSB=1

    // ---- ATP: output port hardware not modeled yet ----
    case 0xCC: cycles = 9; break;

    // ---- CDV: divider reset, no CPU-visible effect modeled ----
    case 0x8E: cycles = 8; break;

    // ---- Flip-flops ----
    case 0xC0: disp_ = false; cycles = 8; break;  // RDP
    case 0xC1: disp_ = true; cycles = 8; break;   // SDP
    case 0xBE: flags_.ie = false; cycles = 8; break;  // RIE
    case 0x81: flags_.ie = true; cycles = 8; break;   // SIE
    case 0x4C: bf_ = false; cycles = 8; break;        // OFF

    // ---- HLT / ITA ----
    case 0xB1: halted_ = true; cycles = 9; break;
    case 0xBA: a_ = bus_.readInputPort(); flags_.z = (a_ == 0); cycles = 9; break;

    default:
      cycles = 1;
      break;
  }
}

}  // namespace lh5801
