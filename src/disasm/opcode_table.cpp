// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#include "opcode_table.h"

#include <array>

namespace pc1500::disasm {

namespace {

// Per-opcode-byte static metadata used to build DecodedInstruction. Not
// exposed outside this file -- decodeOne() is the only public surface.
struct OpcodeInfo {
  const char* mnemonic = nullptr;  // nullptr = undefined opcode
  Operand op1 = Operand::None;
  Operand op2 = Operand::None;
  ControlFlow flow = ControlFlow::Sequential;
  // Only meaningful when op1 or op2 is Branch8: true for the
  // always-backward LOP form and for opcodes whose byte value is the
  // "backward" half of a forward/backward pair (e.g. BCH's 0x9E).
  bool branchBackward = false;
};

int operandByteLength(Operand op) {
  switch (op) {
    case Operand::Me0Abs:
    case Operand::Me1Abs:
    case Operand::Imm16:
      return 2;
    case Operand::Imm8:
    case Operand::Branch8:
    case Operand::VecIdx8:
      return 1;
    default:
      return 0;
  }
}

using Table = std::array<OpcodeInfo, 256>;

// Builds the primary (unprefixed) opcode table. Transcribed directly from
// src/cpu/lh5801.cpp's execPrimary(), cross-checked opcode-by-opcode
// against sdcc-pc1500/sdcc/sdas/aslh5801/test_lh5801.py's CASES for exact
// mnemonic/operand spelling.
Table buildPrimaryTable() {
  Table t{};
  auto set = [&](uint8_t op, const char* mnem, Operand o1 = Operand::None,
                 Operand o2 = Operand::None, ControlFlow flow = ControlFlow::Sequential,
                 bool backward = false) {
    t[op] = OpcodeInfo{mnem, o1, o2, flow, backward};
  };

  // SBC
  set(0x00, "sbc", Operand::RegXL); set(0x10, "sbc", Operand::RegYL); set(0x20, "sbc", Operand::RegUL);
  set(0x80, "sbc", Operand::RegXH); set(0x90, "sbc", Operand::RegYH); set(0xA0, "sbc", Operand::RegUH);
  set(0x01, "sbc", Operand::Me0IndX); set(0x11, "sbc", Operand::Me0IndY); set(0x21, "sbc", Operand::Me0IndU);
  set(0xA1, "sbc", Operand::Me0Abs);

  // ADC
  set(0x02, "adc", Operand::RegXL); set(0x12, "adc", Operand::RegYL); set(0x22, "adc", Operand::RegUL);
  set(0x82, "adc", Operand::RegXH); set(0x92, "adc", Operand::RegYH); set(0xA2, "adc", Operand::RegUH);
  set(0x03, "adc", Operand::Me0IndX); set(0x13, "adc", Operand::Me0IndY); set(0x23, "adc", Operand::Me0IndU);
  set(0xA3, "adc", Operand::Me0Abs);

  // LDA
  set(0x04, "lda", Operand::RegXL); set(0x14, "lda", Operand::RegYL); set(0x24, "lda", Operand::RegUL);
  set(0x84, "lda", Operand::RegXH); set(0x94, "lda", Operand::RegYH); set(0xA4, "lda", Operand::RegUH);
  set(0x05, "lda", Operand::Me0IndX); set(0x15, "lda", Operand::Me0IndY); set(0x25, "lda", Operand::Me0IndU);
  set(0xA5, "lda", Operand::Me0Abs);

  // CPA
  set(0x06, "cpa", Operand::RegXL); set(0x16, "cpa", Operand::RegYL); set(0x26, "cpa", Operand::RegUL);
  set(0x86, "cpa", Operand::RegXH); set(0x96, "cpa", Operand::RegYH); set(0xA6, "cpa", Operand::RegUH);
  set(0x07, "cpa", Operand::Me0IndX); set(0x17, "cpa", Operand::Me0IndY); set(0x27, "cpa", Operand::Me0IndU);
  set(0xA7, "cpa", Operand::Me0Abs);

  // STA
  set(0x0A, "sta", Operand::RegXL); set(0x1A, "sta", Operand::RegYL); set(0x2A, "sta", Operand::RegUL);
  set(0x08, "sta", Operand::RegXH); set(0x18, "sta", Operand::RegYH); set(0x28, "sta", Operand::RegUH);
  set(0x0E, "sta", Operand::Me0IndX); set(0x1E, "sta", Operand::Me0IndY); set(0x2E, "sta", Operand::Me0IndU);
  set(0xAE, "sta", Operand::Me0Abs);

  // AND / ANI
  set(0x09, "and", Operand::Me0IndX); set(0x19, "and", Operand::Me0IndY); set(0x29, "and", Operand::Me0IndU);
  set(0xA9, "and", Operand::Me0Abs);
  set(0xB9, "ani", Operand::RegA, Operand::Imm8);
  set(0x49, "ani", Operand::Me0IndX, Operand::Imm8); set(0x59, "ani", Operand::Me0IndY, Operand::Imm8);
  set(0x69, "ani", Operand::Me0IndU, Operand::Imm8); set(0xE9, "ani", Operand::Me0Abs, Operand::Imm8);

  // BII / BIT
  set(0xBF, "bii", Operand::RegA, Operand::Imm8);
  set(0x4D, "bii", Operand::Me0IndX, Operand::Imm8); set(0x5D, "bii", Operand::Me0IndY, Operand::Imm8);
  set(0x6D, "bii", Operand::Me0IndU, Operand::Imm8); set(0xED, "bii", Operand::Me0Abs, Operand::Imm8);
  set(0x0F, "bit", Operand::Me0IndX); set(0x1F, "bit", Operand::Me0IndY); set(0x2F, "bit", Operand::Me0IndU);
  set(0xAF, "bit", Operand::Me0Abs);

  // ORA / ORI
  set(0x0B, "ora", Operand::Me0IndX); set(0x1B, "ora", Operand::Me0IndY); set(0x2B, "ora", Operand::Me0IndU);
  set(0xAB, "ora", Operand::Me0Abs);
  set(0xBB, "ori", Operand::RegA, Operand::Imm8);
  set(0x4B, "ori", Operand::Me0IndX, Operand::Imm8); set(0x5B, "ori", Operand::Me0IndY, Operand::Imm8);
  set(0x6B, "ori", Operand::Me0IndU, Operand::Imm8); set(0xEB, "ori", Operand::Me0Abs, Operand::Imm8);

  // EOR / EAI
  set(0x0D, "eor", Operand::Me0IndX); set(0x1D, "eor", Operand::Me0IndY); set(0x2D, "eor", Operand::Me0IndU);
  set(0xAD, "eor", Operand::Me0Abs);
  set(0xBD, "eai", Operand::RegA, Operand::Imm8);

  // CPI
  set(0xB7, "cpi", Operand::RegA, Operand::Imm8);
  set(0x4E, "cpi", Operand::RegXL, Operand::Imm8); set(0x5E, "cpi", Operand::RegYL, Operand::Imm8);
  set(0x6E, "cpi", Operand::RegUL, Operand::Imm8);
  set(0x4C, "cpi", Operand::RegXH, Operand::Imm8); set(0x5C, "cpi", Operand::RegYH, Operand::Imm8);
  set(0x6C, "cpi", Operand::RegUH, Operand::Imm8);

  // ADI
  set(0xB3, "adi", Operand::RegA, Operand::Imm8);
  set(0x4F, "adi", Operand::Me0IndX, Operand::Imm8); set(0x5F, "adi", Operand::Me0IndY, Operand::Imm8);
  set(0x6F, "adi", Operand::Me0IndU, Operand::Imm8); set(0xEF, "adi", Operand::Me0Abs, Operand::Imm8);

  // SBI
  set(0xB1, "sbi", Operand::RegA, Operand::Imm8);

  // DCA / DCS
  set(0x8C, "dca", Operand::Me0IndX); set(0x9C, "dca", Operand::Me0IndY); set(0xAC, "dca", Operand::Me0IndU);
  set(0x0C, "dcs", Operand::Me0IndX); set(0x1C, "dcs", Operand::Me0IndY); set(0x2C, "dcs", Operand::Me0IndU);

  // INC / DEC
  set(0xDD, "inc", Operand::RegA);
  set(0x40, "inc", Operand::RegXL); set(0x50, "inc", Operand::RegYL); set(0x60, "inc", Operand::RegUL);
  set(0x44, "inc", Operand::RegX); set(0x54, "inc", Operand::RegY); set(0x64, "inc", Operand::RegU);
  set(0xDF, "dec", Operand::RegA);
  set(0x42, "dec", Operand::RegXL); set(0x52, "dec", Operand::RegYL); set(0x62, "dec", Operand::RegUL);
  set(0x46, "dec", Operand::RegX); set(0x56, "dec", Operand::RegY); set(0x66, "dec", Operand::RegU);

  // DRL / DRR
  set(0xD7, "drl", Operand::Me0IndX);
  set(0xD3, "drr", Operand::Me0IndX);

  // AEX
  set(0xF1, "aex");

  // Rotate / shift
  set(0xDB, "rol"); set(0xD1, "ror"); set(0xD9, "shl"); set(0xD5, "shr");

  // Flip-flops / control
  set(0xE3, "rpu"); set(0xE1, "spu"); set(0xB8, "rpv"); set(0xA8, "spv");
  set(0xFB, "sec"); set(0xF9, "rec"); set(0x38, "nop");

  // LDE / LIN / SDE / SIN
  set(0x47, "lde", Operand::RegX); set(0x57, "lde", Operand::RegY); set(0x67, "lde", Operand::RegU);
  set(0x45, "lin", Operand::RegX); set(0x55, "lin", Operand::RegY); set(0x65, "lin", Operand::RegU);
  set(0x43, "sde", Operand::RegX); set(0x53, "sde", Operand::RegY); set(0x63, "sde", Operand::RegU);
  set(0x41, "sin", Operand::RegX); set(0x51, "sin", Operand::RegY); set(0x61, "sin", Operand::RegU);

  // LDI
  set(0xB5, "ldi", Operand::RegA, Operand::Imm8);
  set(0x4A, "ldi", Operand::RegXL, Operand::Imm8); set(0x5A, "ldi", Operand::RegYL, Operand::Imm8);
  set(0x6A, "ldi", Operand::RegUL, Operand::Imm8);
  set(0x48, "ldi", Operand::RegXH, Operand::Imm8); set(0x58, "ldi", Operand::RegYH, Operand::Imm8);
  set(0x68, "ldi", Operand::RegUH, Operand::Imm8);
  set(0xAA, "ldi", Operand::RegS, Operand::Imm16);

  // Jumps / calls / returns
  set(0xBA, "jmp", Operand::Imm16, Operand::None, ControlFlow::Jump);
  set(0xBE, "sjp", Operand::Imm16, Operand::None, ControlFlow::Call);
  set(0x9A, "rtn", Operand::None, Operand::None, ControlFlow::Return);
  set(0x8A, "rti", Operand::None, Operand::None, ControlFlow::Return);

  // TIN / CIN / LOP
  set(0xF5, "tin"); set(0xF7, "cin");
  set(0x88, "lop", Operand::RegUL, Operand::Branch8, ControlFlow::ConditionalBranch, /*backward=*/true);

  // Branches: BCH (unconditional)
  set(0x8E, "bch", Operand::Branch8, Operand::None, ControlFlow::Jump, false);
  set(0x9E, "bch", Operand::Branch8, Operand::None, ControlFlow::Jump, true);
  // BCS (C=1) / BCR (C=0)
  set(0x83, "bcs", Operand::Branch8, Operand::None, ControlFlow::ConditionalBranch, false);
  set(0x93, "bcs", Operand::Branch8, Operand::None, ControlFlow::ConditionalBranch, true);
  set(0x81, "bcr", Operand::Branch8, Operand::None, ControlFlow::ConditionalBranch, false);
  set(0x91, "bcr", Operand::Branch8, Operand::None, ControlFlow::ConditionalBranch, true);
  // BHS (H=1) / BHR (H=0)
  set(0x87, "bhs", Operand::Branch8, Operand::None, ControlFlow::ConditionalBranch, false);
  set(0x97, "bhs", Operand::Branch8, Operand::None, ControlFlow::ConditionalBranch, true);
  set(0x85, "bhr", Operand::Branch8, Operand::None, ControlFlow::ConditionalBranch, false);
  set(0x95, "bhr", Operand::Branch8, Operand::None, ControlFlow::ConditionalBranch, true);
  // BVS (V=1) / BVR (V=0)
  set(0x8F, "bvs", Operand::Branch8, Operand::None, ControlFlow::ConditionalBranch, false);
  set(0x9F, "bvs", Operand::Branch8, Operand::None, ControlFlow::ConditionalBranch, true);
  set(0x8D, "bvr", Operand::Branch8, Operand::None, ControlFlow::ConditionalBranch, false);
  set(0x9D, "bvr", Operand::Branch8, Operand::None, ControlFlow::ConditionalBranch, true);
  // BZS (Z=1) / BZR (Z=0)
  set(0x8B, "bzs", Operand::Branch8, Operand::None, ControlFlow::ConditionalBranch, false);
  set(0x9B, "bzs", Operand::Branch8, Operand::None, ControlFlow::ConditionalBranch, true);
  set(0x89, "bzr", Operand::Branch8, Operand::None, ControlFlow::ConditionalBranch, false);
  set(0x99, "bzr", Operand::Branch8, Operand::None, ControlFlow::ConditionalBranch, true);

  // VEJ: one-byte vector call, FF00H+opcode -- no operand bytes, rendered
  // operand is the opcode's own value.
  for (uint8_t op : {0xC0, 0xC2, 0xC4, 0xC6, 0xC8, 0xCA, 0xCC, 0xCE, 0xD0, 0xD2, 0xD4, 0xD6, 0xD8, 0xDA,
                      0xDC, 0xDE, 0xE0, 0xE2, 0xE4, 0xE6, 0xE8, 0xEA, 0xEC, 0xEE, 0xF0, 0xF2, 0xF4, 0xF6}) {
    set(op, "vej", Operand::VejSelf, Operand::None, ControlFlow::Call);
  }

  // VMJ (unconditional) / VCS / VCR / VHS / VHR / VZS / VZR / VVS
  // (conditional) -- 1-byte FF00H-page index operand.
  set(0xCD, "vmj", Operand::VecIdx8, Operand::None, ControlFlow::Call);
  set(0xC3, "vcs", Operand::VecIdx8, Operand::None, ControlFlow::Call);
  set(0xC1, "vcr", Operand::VecIdx8, Operand::None, ControlFlow::Call);
  set(0xC7, "vhs", Operand::VecIdx8, Operand::None, ControlFlow::Call);
  set(0xC5, "vhr", Operand::VecIdx8, Operand::None, ControlFlow::Call);
  set(0xCB, "vzs", Operand::VecIdx8, Operand::None, ControlFlow::Call);
  set(0xC9, "vzr", Operand::VecIdx8, Operand::None, ControlFlow::Call);
  set(0xCF, "vvs", Operand::VecIdx8, Operand::None, ControlFlow::Call);

  return t;
}

// Builds the FD-prefixed sub-opcode table. Transcribed from
// src/cpu/lh5801.cpp's execFD().
Table buildFdTable() {
  Table t{};
  auto set = [&](uint8_t op, const char* mnem, Operand o1 = Operand::None,
                 Operand o2 = Operand::None, ControlFlow flow = ControlFlow::Sequential) {
    t[op] = OpcodeInfo{mnem, o1, o2, flow, false};
  };

  // ADC #(R)/#(ab)
  set(0x03, "adc", Operand::Me1IndX); set(0x13, "adc", Operand::Me1IndY); set(0x23, "adc", Operand::Me1IndU);
  set(0xA3, "adc", Operand::Me1Abs);
  // SBC #(R)/#(ab)
  set(0x01, "sbc", Operand::Me1IndX); set(0x11, "sbc", Operand::Me1IndY); set(0x21, "sbc", Operand::Me1IndU);
  set(0xA1, "sbc", Operand::Me1Abs);
  // CPA #(R)/#(ab)
  set(0x07, "cpa", Operand::Me1IndX); set(0x17, "cpa", Operand::Me1IndY); set(0x27, "cpa", Operand::Me1IndU);
  set(0xA7, "cpa", Operand::Me1Abs);
  // LDA #(R)/#(ab)
  set(0x05, "lda", Operand::Me1IndX); set(0x15, "lda", Operand::Me1IndY); set(0x25, "lda", Operand::Me1IndU);
  set(0xA5, "lda", Operand::Me1Abs);
  // STA #(R)/#(ab)
  set(0x0E, "sta", Operand::Me1IndX); set(0x1E, "sta", Operand::Me1IndY); set(0x2E, "sta", Operand::Me1IndU);
  set(0xAE, "sta", Operand::Me1Abs);
  // AND/ANI #(R)/#(ab)
  set(0x09, "and", Operand::Me1IndX); set(0x19, "and", Operand::Me1IndY); set(0x29, "and", Operand::Me1IndU);
  set(0xA9, "and", Operand::Me1Abs);
  set(0x49, "ani", Operand::Me1IndX, Operand::Imm8); set(0x59, "ani", Operand::Me1IndY, Operand::Imm8);
  set(0x69, "ani", Operand::Me1IndU, Operand::Imm8); set(0xE9, "ani", Operand::Me1Abs, Operand::Imm8);
  // BII/BIT #(R)/#(ab)
  set(0x4D, "bii", Operand::Me1IndX, Operand::Imm8); set(0x5D, "bii", Operand::Me1IndY, Operand::Imm8);
  set(0x6D, "bii", Operand::Me1IndU, Operand::Imm8); set(0xED, "bii", Operand::Me1Abs, Operand::Imm8);
  set(0x0F, "bit", Operand::Me1IndX); set(0x1F, "bit", Operand::Me1IndY); set(0x2F, "bit", Operand::Me1IndU);
  set(0xAF, "bit", Operand::Me1Abs);
  // ORA/ORI #(R)/#(ab)
  set(0x0B, "ora", Operand::Me1IndX); set(0x1B, "ora", Operand::Me1IndY); set(0x2B, "ora", Operand::Me1IndU);
  set(0xAB, "ora", Operand::Me1Abs);
  set(0x4B, "ori", Operand::Me1IndX, Operand::Imm8); set(0x5B, "ori", Operand::Me1IndY, Operand::Imm8);
  set(0x6B, "ori", Operand::Me1IndU, Operand::Imm8); set(0xEB, "ori", Operand::Me1Abs, Operand::Imm8);
  // EOR #(R)/#(ab)
  set(0x0D, "eor", Operand::Me1IndX); set(0x1D, "eor", Operand::Me1IndY); set(0x2D, "eor", Operand::Me1IndU);
  set(0xAD, "eor", Operand::Me1Abs);
  // ADI #(R)/#(ab)
  set(0x4F, "adi", Operand::Me1IndX, Operand::Imm8); set(0x5F, "adi", Operand::Me1IndY, Operand::Imm8);
  set(0x6F, "adi", Operand::Me1IndU, Operand::Imm8); set(0xEF, "adi", Operand::Me1Abs, Operand::Imm8);
  // ADR
  set(0xCA, "adr", Operand::RegX); set(0xDA, "adr", Operand::RegY); set(0xEA, "adr", Operand::RegU);
  // DCA/DCS #(R)
  set(0x8C, "dca", Operand::Me1IndX); set(0x9C, "dca", Operand::Me1IndY); set(0xAC, "dca", Operand::Me1IndU);
  set(0x0C, "dcs", Operand::Me1IndX); set(0x1C, "dcs", Operand::Me1IndY); set(0x2C, "dcs", Operand::Me1IndU);
  // INC/DEC XH/YH/UH
  set(0x40, "inc", Operand::RegXH); set(0x50, "inc", Operand::RegYH); set(0x60, "inc", Operand::RegUH);
  set(0x42, "dec", Operand::RegXH); set(0x52, "dec", Operand::RegYH); set(0x62, "dec", Operand::RegUH);
  // DRL/DRR #(X)
  set(0xD7, "drl", Operand::Me1IndX);
  set(0xD3, "drr", Operand::Me1IndX);
  // LDX (source R -> X)
  set(0x08, "ldx", Operand::RegX); set(0x18, "ldx", Operand::RegY); set(0x28, "ldx", Operand::RegU);
  set(0x48, "ldx", Operand::RegS); set(0x58, "ldx", Operand::RegP);
  // STX (X -> destination R)
  set(0x4A, "stx", Operand::RegX); set(0x5A, "stx", Operand::RegY); set(0x6A, "stx", Operand::RegU);
  set(0x4E, "stx", Operand::RegS);
  // STX P (X -> P) is an indirect jump -- P *is* the program counter, so
  // this unconditionally redirects control to whatever X holds. The target
  // isn't statically resolvable in general (X's value only exists at
  // runtime), so this is classified like RTN: no fall-through, no known
  // target. Custom BASIC keyword routines are confirmed (real-hardware
  // testing, this session) to use exactly this -- LDI X,<addr>; STX P --
  // as their dispatch-back mechanism instead of RTN; without this
  // classification the traverser fell through into whatever follows in
  // memory (often padding) and decoded garbage as more instructions.
  set(0x5E, "stx", Operand::RegP, Operand::None, ControlFlow::Return);
  // PSH / POP
  set(0xC8, "psh", Operand::RegA); set(0x88, "psh", Operand::RegX); set(0x98, "psh", Operand::RegY);
  set(0xA8, "psh", Operand::RegU);
  set(0x8A, "pop", Operand::RegA); set(0x0A, "pop", Operand::RegX); set(0x1A, "pop", Operand::RegY);
  set(0x2A, "pop", Operand::RegU);
  // ATT / TTA
  set(0xEC, "att"); set(0xAA, "tta");
  // AM0 / AM1
  set(0xCE, "am0"); set(0xDE, "am1");
  // ATP
  set(0xCC, "atp");
  // CDV
  set(0x8E, "cdv");
  // Flip-flops
  set(0xC0, "rdp"); set(0xC1, "sdp"); set(0xBE, "rie"); set(0x81, "sie"); set(0x4C, "off");
  // HLT / ITA
  set(0xB1, "hlt", Operand::None, Operand::None, ControlFlow::Halt);
  set(0xBA, "ita");

  return t;
}

const Table& primaryTable() {
  static const Table t = buildPrimaryTable();
  return t;
}

const Table& fdTable() {
  static const Table t = buildFdTable();
  return t;
}

// Renders op1/op2's byte-consuming values and computes flow/branchTarget/
// vectorSlot -- shared by the primary and FD decode paths below.
DecodedInstruction fillOperands(const OpcodeInfo& info, const uint8_t* bytes, size_t available,
                                 int consumedSoFar, uint16_t pc) {
  DecodedInstruction d;
  d.valid = true;
  d.mnemonic = info.mnemonic;
  d.op1 = info.op1;
  d.op2 = info.op2;
  d.flow = info.flow;

  int need = consumedSoFar + operandByteLength(info.op1) + operandByteLength(info.op2);
  if (static_cast<size_t>(need) > available) {
    d.valid = false;
    d.length = static_cast<int>(available);
    return d;
  }

  int pos = consumedSoFar;
  auto fetch16 = [&]() {
    uint16_t v = static_cast<uint16_t>((bytes[pos] << 8) | bytes[pos + 1]);
    pos += 2;
    return v;
  };
  auto fetch8 = [&]() { return bytes[pos++]; };

  // Branch8 can appear in either op1 (all the plain branch mnemonics) or
  // op2 (LOP, after its RegUL destination) -- fetch its raw displacement
  // now, but resolve it to an absolute address only once `pos` reflects
  // the *whole* instruction's length (below), since the displacement is
  // always relative to the address right after the complete instruction
  // regardless of which operand slot it lives in.
  uint8_t branchDisp = 0;
  bool hasBranch = false;

  auto fetchOperand = [&](Operand kind, uint32_t& value) {
    switch (kind) {
      case Operand::Me0Abs:
      case Operand::Me1Abs:
      case Operand::Imm16:
        value = fetch16();
        break;
      case Operand::VejSelf:
        value = bytes[0];  // the opcode byte itself, no bytes fetched
        break;
      case Operand::VecIdx8:
      case Operand::Imm8:
        value = fetch8();
        break;
      case Operand::Branch8:
        branchDisp = fetch8();
        hasBranch = true;
        break;
      default:
        break;
    }
  };
  fetchOperand(info.op1, d.value1);
  fetchOperand(info.op2, d.value2);

  d.length = pos;

  if (hasBranch) {
    uint16_t afterInstr = static_cast<uint16_t>(pc + pos);
    d.branchTarget = info.branchBackward ? static_cast<uint16_t>(afterInstr - branchDisp)
                                          : static_cast<uint16_t>(afterInstr + branchDisp);
  }

  // Jump/call target resolution for the non-branch, non-vector forms
  // (JMP/SJP use a bare Imm16 op1 as the absolute target).
  if ((d.flow == ControlFlow::Jump || d.flow == ControlFlow::Call) && info.op1 == Operand::Imm16) {
    d.branchTarget = static_cast<uint16_t>(d.value1);
  }
  if (d.flow == ControlFlow::Call &&
      (info.op1 == Operand::VejSelf || info.op1 == Operand::VecIdx8)) {
    d.isVectorCall = true;
    d.vectorSlot = static_cast<uint16_t>(0xFF00 | (d.value1 & 0xFF));
  }
  return d;
}

}  // namespace

DecodedInstruction decodeOne(const uint8_t* bytes, size_t available, uint16_t pc) {
  if (available == 0) {
    DecodedInstruction d;
    d.valid = false;
    d.length = 0;
    return d;
  }
  uint8_t opcode = bytes[0];
  if (opcode == 0xFD) {
    if (available < 2) {
      DecodedInstruction d;
      d.valid = false;
      d.length = static_cast<int>(available);
      return d;
    }
    const OpcodeInfo& info = fdTable()[bytes[1]];
    if (info.mnemonic == nullptr) {
      DecodedInstruction d;
      d.valid = false;
      d.length = 2;
      return d;
    }
    return fillOperands(info, bytes, available, /*consumedSoFar=*/2, pc);
  }
  const OpcodeInfo& info = primaryTable()[opcode];
  if (info.mnemonic == nullptr) {
    DecodedInstruction d;
    d.valid = false;
    d.length = 1;
    return d;
  }
  return fillOperands(info, bytes, available, /*consumedSoFar=*/1, pc);
}

}  // namespace pc1500::disasm
