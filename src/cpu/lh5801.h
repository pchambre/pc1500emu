// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#pragma once

#include <cstdint>

namespace lh5801 {

// LH5801 has two independent 64KB memory spaces (ME0, ME1). Plain (Rreg)/(ab)
// addressing modes hit ME0; #(Rreg)/#(ab) hit ME1. On the PC-1500, ME1 holds
// only the LH5811 I/O port controller (F000H-F00FH); everything else the CPU
// can address lives in ME0. See docs/lh5801_opcode_reference.md and
// docs/pc1500_hardware_reference.md.
class MemoryBus {
 public:
  virtual ~MemoryBus() = default;
  virtual uint8_t readME0(uint16_t addr) = 0;
  virtual void writeME0(uint16_t addr, uint8_t value) = 0;
  virtual uint8_t readME1(uint16_t addr) = 0;
  virtual void writeME1(uint16_t addr, uint8_t value) = 0;

  // IN0-IN7: CPU-direct key input pins (not part of either memory space,
  // read only via the ITA instruction). Bit i = INi.
  virtual uint8_t readInputPort() { return 0xFF; }

  // PU/PV flip-flop levels (SPU/RPU/SPV/RPV), forwarded here every time
  // they change -- only meaningful to a bus that models a bank-switched
  // plug-in ROM module at 0x8000H-0xBFFFH (see pc1500::Bus::RomModule);
  // default no-op so other MemoryBus implementations don't need to care.
  virtual void setPu(bool) {}
  virtual void setPv(bool) {}
};

struct Flags {
  bool c = false;
  bool v = false;
  bool z = false;
  bool h = false;
  bool ie = false;
};

// Instruction-accurate LH5801 CPU core. Every opcode's behavior and byte
// encoding is taken from docs/lh5801_opcode_reference.md (cross-verified
// against two independent manual scans). The T (status) register's bit
// layout (used by ATT/TTA/RTI) is documented in manual section 2-2-3: low
// 5 bits are H,V,Z,IE,C from left to right (C in the 1s position, H in the
// 16s) -- see packFlags/unpackFlags in lh5801.cpp.
//
// One remaining honest simplification, flagged at its point of use:
//   - ROL/ROR/SHL/SHR are documented as changing C,V,H,Z, but the manual
//     doesn't specify V/H's exact meaning for a rotate/shift (unlike
//     CPA, where it's explicit that V/H "may change but have no meaning").
//     This core updates C and Z; V and H are left unchanged for these four
//     instructions rather than guess at undocumented semantics.
class CPU {
 public:
  explicit CPU(MemoryBus& bus) : bus_(bus) {}

  void reset();

  // Executes exactly one instruction. Returns the cycle count consumed
  // (per the opcode reference's cycle table), or 0 if halted (HLT).
  int step();

  // Accessors mainly for tests/debugging.
  uint8_t a() const { return a_; }
  uint16_t x() const { return x_; }
  uint16_t y() const { return y_; }
  uint16_t u() const { return u_; }
  uint16_t s() const { return s_; }
  uint16_t p() const { return p_; }
  const Flags& flags() const { return flags_; }
  bool halted() const { return halted_; }
  bool bf() const { return bf_; }
  bool disp() const { return disp_; }
  bool pu() const { return pu_; }
  bool pv() const { return pv_; }

  void setP(uint16_t p) { p_ = p; }
  void setHalted(bool h) { halted_ = h; }
  void setS(uint16_t s) { s_ = s; }
  void setX(uint16_t x) { x_ = x; }
  void setY(uint16_t y) { y_ = y; }
  void setU(uint16_t u) { u_ = u; }

  // Hardware-level ON-key press: sets BFI high, which (on real hardware)
  // supplies VCC via BFO. Independent of any CPU instruction; the OFF
  // instruction is software's way to reset it back low.
  void pressOnKey() { bf_ = true; }

  // External maskable interrupt pin (MI). Vector FFF8H, gated by IE.
  void requestMI() { miPending_ = true; }
  // Non-maskable interrupt pin (NMI). Vector FFFCH, always responds.
  void requestNMI() { nmiPending_ = true; }

  // Advances the internal 9-bit timer counter by one tick. Per the PC-1500
  // Technical Reference Manual §2-3-1: the timer is a free-running **9-bit
  // polynomial (LFSR) counter** -- NOT a linear up-counter -- driven by
  // ΦF, and a timer interrupt (vector FFFAH, gated by IE) is requested
  // when its content reaches 1FFH. The real tick rate comes from a
  // divider off the crystal (documented there as crystal/128 for the
  // PC-2's 4MHz crystal); the PC-1500 uses a 2.6MHz crystal and we don't
  // have its exact divider depth confirmed, so callers should treat the
  // rate as an approximation (see main.cpp) rather than a cycle-accurate
  // figure. The polynomial sequence itself (511 nonzero states, degree-9
  // primitive polynomial x^9+x^4+1, confirmed against the TRM's own
  // printed "POLINOMINAL COUNTER" table) is authoritative, though --
  // AM0/AM1 can set the register to an arbitrary value to control
  // interrupt timing precisely (confirmed: ROM1.BIN does this, e.g. at
  // E2A4H), so returning the wrong intermediate values for a linear
  // approximation (as this used to do) diverges from real hardware for
  // any code that relies on it, not just a cosmetic detail.
  void tickTimer();
  uint16_t timerCounter() const { return timerCounter_; }

 private:
  MemoryBus& bus_;

  uint8_t a_ = 0;
  uint16_t x_ = 0, y_ = 0, u_ = 0;
  uint16_t s_ = 0;
  uint16_t p_ = 0;
  Flags flags_;
  bool halted_ = false;

  // CPU-internal flip-flops exposed for the bus/host to read.
  bool bf_ = false;    // BF flip-flop (BFI/BFO wake latch)
  bool disp_ = false;  // LCD on/off control flip-flop (SDP/RDP)
  bool pu_ = false;    // general-purpose flip-flop PU (SPU/RPU)
  bool pv_ = false;    // general-purpose flip-flop PV (SPV/RPV)

  // Interrupts. MI/NMI are external-pin requests latched until dispatched;
  // the timer interrupt is generated internally by tickTimer(). All three
  // wake the CPU from HLT; MI/timer additionally require IE=1.
  bool miPending_ = false;
  bool nmiPending_ = false;
  bool timerInterruptPending_ = false;
  uint16_t timerCounter_ = 0;
  // Index into kPolyCounterTable (lh5801_timer_table.h) that timerCounter_
  // currently corresponds to; meaningless while timerCounter_ == 0 (the
  // LFSR's fixed/parked point -- see tickTimer()/setTimerCounter()).
  int timerStep_ = 0;
  void setTimerCounter(uint16_t v);
  void dispatchInterrupt(uint16_t vectorAddr, int& cycles);

  // Register-select family used by ADC/SBC/CPA/LDA/STA/etc: 0=X, 1=Y, 2=U.
  uint16_t regR16(int rsel) const;
  void setRegR16(int rsel, uint16_t v);
  uint8_t regRL(int rsel) const;
  void setRegRL(int rsel, uint8_t v);
  uint8_t regRH(int rsel) const;
  void setRegRH(int rsel, uint8_t v);

  uint8_t fetch8();
  uint16_t fetch16();  // high byte first, then low (matches (ab) notation)

  void push8(uint8_t v);
  uint8_t pop8();
  void push16(uint16_t v);  // low byte first, then high (matches manual)
  uint16_t pop16();

  // Shared add/subtract-as-add-of-complement ALU op; updates C,V,H,Z.
  uint8_t doAdd(uint8_t opA, uint8_t opB, bool carryIn);

  uint8_t packFlags() const;
  void unpackFlags(uint8_t v);

  void execPrimary(uint8_t opcode, int& cycles);
  void execFD(uint8_t opcode, int& cycles);
};

}  // namespace lh5801
