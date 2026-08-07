// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// LH5801 instruction decoder for the pc1500emu disassembler. Ground truth
// is src/cpu/lh5801.cpp's own execPrimary()/execFD() switch statements (the
// emulator's own verified semantics), cross-checked opcode-by-opcode
// against sdcc-pc1500's sdaslh5801 test suite
// (sdcc/sdas/aslh5801/test_lh5801.py) for exact output syntax, so this
// disassembler's text is directly reassemblable by that tool. Deliberately
// independent of lh5801.cpp/CPU/Bus -- this operates purely on raw bytes,
// no live execution needed.
namespace pc1500::disasm {

// One operand slot's rendering kind. Register-direct/memory-indirect kinds
// consume no instruction bytes (the addressing mode is baked into the
// opcode byte itself); Me0Abs/Me1Abs/Imm8/Imm16/Branch8/VecIdx8 each
// consume the stated number of trailing bytes. VejSelf consumes none --
// its rendered value is the opcode byte itself, not a fetched operand.
enum class Operand {
  None,
  RegXL, RegYL, RegUL, RegXH, RegYH, RegUH, RegX, RegY, RegU, RegA, RegS, RegP,
  Me0IndX, Me0IndY, Me0IndU,  // (x) / (y) / (u)
  Me1IndX, Me1IndY, Me1IndU,  // #(x) / #(y) / #(u)
  Me0Abs,                     // (0xNNNN) -- 2 bytes
  Me1Abs,                     // #(0xNNNN) -- 2 bytes
  Imm8,                       // 0xNN -- 1 byte
  Imm16,                      // 0xNNNN, bare (jmp/sjp/ldi s) -- 2 bytes
  Branch8,                    // relative branch, rendered as a label -- 1 byte
  VejSelf,                    // 0xNN rendered from the opcode's own value -- 0 bytes
  VecIdx8,                    // 0xNN, an FF00H-page index -- 1 byte
};

// How an instruction affects control flow, for the analyzer's traversal.
enum class ControlFlow {
  Sequential,         // falls through only
  Jump,               // unconditional, no return -- BCH, JMP
  Call,               // pushes a return address; follow target AND fall through --
                       // SJP, VEJ, VMJ, and the conditional vector-call forms
  ConditionalBranch,  // follow target AND fall through -- BCS/BCR/etc, LOP
  Return,             // RTN, RTI -- no fall-through, no target
  Halt,               // HLT -- falls through once an interrupt wakes it (confirmed
                       // live this session: the ROM's own E2AAH idle loop resumes
                       // right after its own HLT)
};

struct DecodedInstruction {
  bool valid = false;   // false = undefined opcode/sub-opcode byte
  int length = 1;        // total bytes consumed, including any FD prefix
  std::string mnemonic;  // empty when !valid
  Operand op1 = Operand::None;
  Operand op2 = Operand::None;
  // value1: meaningful for Me0Abs/Me1Abs (the address)/VejSelf (the opcode
  // byte)/VecIdx8 (the fetched index) op1 kinds. value2: meaningful for
  // Imm8/Imm16 op2 (the trailing immediate in two-operand forms).
  uint32_t value1 = 0;
  uint32_t value2 = 0;
  ControlFlow flow = ControlFlow::Sequential;
  // Resolved absolute target address, valid when flow is Jump/
  // ConditionalBranch (from a Branch8 operand) or Call via a plain 16-bit
  // address (SJP). Not used for vector calls -- see isVectorCall below.
  uint16_t branchTarget = 0;
  // True for VEJ/VMJ/conditional-vector Call forms: the real target isn't
  // known from this instruction's own bytes alone, it's 2 bytes stored at
  // vectorSlot (0xFF00 + the VEJ opcode's own value, or 0xFF00 + the
  // fetched VecIdx8 operand) -- the analyzer reads it from the ROM image.
  bool isVectorCall = false;
  uint16_t vectorSlot = 0;
};

// Decodes one instruction starting at bytes[0], which represents ROM
// address `pc`. `available` bounds how many bytes may be read (the caller
// must ensure it reflects how much ROM actually remains from `pc`).
// Returns valid=false (length still >=1) if the opcode/sub-opcode is
// undefined, or if `available` is too small to hold the full instruction.
DecodedInstruction decodeOne(const uint8_t* bytes, size_t available, uint16_t pc);

}  // namespace pc1500::disasm
