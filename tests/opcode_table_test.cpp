// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
//
// Decodes every (asm text, hex bytes) case transcribed from
// sdcc-pc1500/sdcc/sdas/aslh5801/test_lh5801.py's own CASES and
// BRANCH_EXPECTED lists (that project's independent oracle for sdaslh5801's
// encoding, re-derived from docs/lh5801_opcode_reference.md rather than
// copy-pasted from its own opcode table) and checks the decoded mnemonic,
// operand text, and instruction length match -- the same ground truth in
// reverse, so this disassembler's output is confirmed reassemblable by
// that tool.
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "opcode_table.h"

namespace {

using pc1500::disasm::ControlFlow;
using pc1500::disasm::decodeOne;
using pc1500::disasm::DecodedInstruction;
using pc1500::disasm::Operand;

int g_failures = 0;

#define CHECK(cond)                                                \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      g_failures++;                                                 \
    }                                                                \
  } while (0)

std::vector<uint8_t> parseHex(const std::string& hex) {
  std::vector<uint8_t> out;
  std::istringstream iss(hex);
  std::string byteStr;
  while (iss >> byteStr) out.push_back(static_cast<uint8_t>(std::stoul(byteStr, nullptr, 16)));
  return out;
}

// Minimal, test-only text renderer -- deliberately not part of the public
// opcode_table.h API (which only decodes; label-aware full rendering lives
// in formatter.h/.cpp). Branch8 operands aren't rendered here at all --
// those are checked via decoded numeric branchTarget instead, matching
// test_lh5801.py's own separate BRANCH_EXPECTED handling.
std::string hex2(uint32_t v) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "0x%02X", v & 0xFF);
  return buf;
}
std::string hex4(uint32_t v) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "0x%04X", v & 0xFFFF);
  return buf;
}

std::string renderOperand(Operand kind, uint32_t value) {
  switch (kind) {
    case Operand::None: return "";
    case Operand::RegXL: return "xl"; case Operand::RegYL: return "yl"; case Operand::RegUL: return "ul";
    case Operand::RegXH: return "xh"; case Operand::RegYH: return "yh"; case Operand::RegUH: return "uh";
    case Operand::RegX: return "x"; case Operand::RegY: return "y"; case Operand::RegU: return "u";
    case Operand::RegA: return "a"; case Operand::RegS: return "s"; case Operand::RegP: return "p";
    case Operand::Me0IndX: return "(x)"; case Operand::Me0IndY: return "(y)"; case Operand::Me0IndU: return "(u)";
    case Operand::Me1IndX: return "#(x)"; case Operand::Me1IndY: return "#(y)"; case Operand::Me1IndU: return "#(u)";
    case Operand::Me0Abs: return "(" + hex4(value) + ")";
    case Operand::Me1Abs: return "#(" + hex4(value) + ")";
    case Operand::Imm8: return hex2(value);
    case Operand::Imm16: return hex4(value);
    case Operand::VejSelf: return hex2(value);
    case Operand::VecIdx8: return hex2(value);
    case Operand::Branch8: return "<branch>";  // not used in text-comparison cases
  }
  return "?";
}

std::string renderText(const DecodedInstruction& d) {
  std::string text = d.mnemonic;
  if (d.op1 != Operand::None) {
    text += " " + renderOperand(d.op1, d.value1);
    if (d.op2 != Operand::None) text += "," + renderOperand(d.op2, d.value2);
  }
  return text;
}

std::string normalizeExpected(std::string asmLine) {
  // CASES lines are already lowercase, single-spaced ("adi (x),0x11") --
  // just collapse any incidental double spaces for a robust compare.
  std::string out;
  bool lastSpace = false;
  for (char c : asmLine) {
    if (c == ' ') {
      if (!lastSpace) out += c;
      lastSpace = true;
    } else {
      out += c;
      lastSpace = false;
    }
  }
  return out;
}

struct Case {
  const char* asmText;
  const char* hex;
};

// Transcribed verbatim from test_lh5801.py's CASES (branch-direction cases
// excluded -- those need label context, checked separately below).
const Case kCases[] = {
    {"adc xl", "02"}, {"adc yl", "12"}, {"adc ul", "22"},
    {"adc xh", "82"}, {"adc yh", "92"}, {"adc uh", "A2"},
    {"adc (x)", "03"}, {"adc (y)", "13"}, {"adc (u)", "23"},
    {"adc (0x1234)", "A3 12 34"},
    {"adc #(x)", "FD 03"}, {"adc #(y)", "FD 13"}, {"adc #(u)", "FD 23"},
    {"adc #(0x1234)", "FD A3 12 34"},
    {"sbc xl", "00"}, {"sbc yl", "10"}, {"sbc ul", "20"},
    {"sbc xh", "80"}, {"sbc yh", "90"}, {"sbc uh", "A0"},
    {"sbc (x)", "01"}, {"sbc (y)", "11"}, {"sbc (u)", "21"},
    {"sbc (0x1234)", "A1 12 34"},
    {"sbc #(x)", "FD 01"}, {"sbc #(y)", "FD 11"}, {"sbc #(u)", "FD 21"},
    {"sbc #(0x1234)", "FD A1 12 34"},
    {"cpa xl", "06"}, {"cpa yl", "16"}, {"cpa ul", "26"},
    {"cpa xh", "86"}, {"cpa yh", "96"}, {"cpa uh", "A6"},
    {"cpa (x)", "07"}, {"cpa (y)", "17"}, {"cpa (u)", "27"},
    {"cpa (0x1234)", "A7 12 34"},
    {"cpa #(x)", "FD 07"}, {"cpa #(y)", "FD 17"}, {"cpa #(u)", "FD 27"},
    {"cpa #(0x1234)", "FD A7 12 34"},
    {"lda xl", "04"}, {"lda yl", "14"}, {"lda ul", "24"},
    {"lda xh", "84"}, {"lda yh", "94"}, {"lda uh", "A4"},
    {"lda (x)", "05"}, {"lda (y)", "15"}, {"lda (u)", "25"},
    {"lda (0x1234)", "A5 12 34"},
    {"lda #(x)", "FD 05"}, {"lda #(y)", "FD 15"}, {"lda #(u)", "FD 25"},
    {"lda #(0x1234)", "FD A5 12 34"},
    {"sta xl", "0A"}, {"sta yl", "1A"}, {"sta ul", "2A"},
    {"sta xh", "08"}, {"sta yh", "18"}, {"sta uh", "28"},
    {"sta (x)", "0E"}, {"sta (y)", "1E"}, {"sta (u)", "2E"},
    {"sta (0x1234)", "AE 12 34"},
    {"sta #(x)", "FD 0E"}, {"sta #(y)", "FD 1E"}, {"sta #(u)", "FD 2E"},
    {"sta #(0x1234)", "FD AE 12 34"},
    {"and (x)", "09"}, {"and (y)", "19"}, {"and (u)", "29"},
    {"and (0x1234)", "A9 12 34"},
    {"and #(x)", "FD 09"}, {"and #(y)", "FD 19"}, {"and #(u)", "FD 29"},
    {"and #(0x1234)", "FD A9 12 34"},
    {"ora (x)", "0B"}, {"ora (y)", "1B"}, {"ora (u)", "2B"},
    {"ora (0x1234)", "AB 12 34"},
    {"ora #(x)", "FD 0B"}, {"ora #(y)", "FD 1B"}, {"ora #(u)", "FD 2B"},
    {"ora #(0x1234)", "FD AB 12 34"},
    {"eor (x)", "0D"}, {"eor (y)", "1D"}, {"eor (u)", "2D"},
    {"eor (0x1234)", "AD 12 34"},
    {"eor #(x)", "FD 0D"}, {"eor #(y)", "FD 1D"}, {"eor #(u)", "FD 2D"},
    {"eor #(0x1234)", "FD AD 12 34"},
    {"bit (x)", "0F"}, {"bit (y)", "1F"}, {"bit (u)", "2F"},
    {"bit (0x1234)", "AF 12 34"},
    {"bit #(x)", "FD 0F"}, {"bit #(y)", "FD 1F"}, {"bit #(u)", "FD 2F"},
    {"bit #(0x1234)", "FD AF 12 34"},
    {"dca (x)", "8C"}, {"dca (y)", "9C"}, {"dca (u)", "AC"},
    {"dca #(x)", "FD 8C"}, {"dca #(y)", "FD 9C"}, {"dca #(u)", "FD AC"},
    {"dcs (x)", "0C"}, {"dcs (y)", "1C"}, {"dcs (u)", "2C"},
    {"dcs #(x)", "FD 0C"}, {"dcs #(y)", "FD 1C"}, {"dcs #(u)", "FD 2C"},
    {"adi a,0x11", "B3 11"}, {"adi (x),0x11", "4F 11"}, {"adi (y),0x11", "5F 11"},
    {"adi (u),0x11", "6F 11"}, {"adi (0x1234),0x11", "EF 12 34 11"},
    {"adi #(x),0x11", "FD 4F 11"}, {"adi #(y),0x11", "FD 5F 11"},
    {"adi #(u),0x11", "FD 6F 11"}, {"adi #(0x1234),0x11", "FD EF 12 34 11"},
    {"ani a,0x11", "B9 11"}, {"ani (x),0x11", "49 11"}, {"ani (y),0x11", "59 11"},
    {"ani (u),0x11", "69 11"}, {"ani (0x1234),0x11", "E9 12 34 11"},
    {"ani #(x),0x11", "FD 49 11"}, {"ani #(y),0x11", "FD 59 11"},
    {"ani #(u),0x11", "FD 69 11"}, {"ani #(0x1234),0x11", "FD E9 12 34 11"},
    {"ori a,0x11", "BB 11"}, {"ori (x),0x11", "4B 11"}, {"ori (y),0x11", "5B 11"},
    {"ori (u),0x11", "6B 11"}, {"ori (0x1234),0x11", "EB 12 34 11"},
    {"ori #(x),0x11", "FD 4B 11"}, {"ori #(y),0x11", "FD 5B 11"},
    {"ori #(u),0x11", "FD 6B 11"}, {"ori #(0x1234),0x11", "FD EB 12 34 11"},
    {"bii a,0x11", "BF 11"}, {"bii (x),0x11", "4D 11"}, {"bii (y),0x11", "5D 11"},
    {"bii (u),0x11", "6D 11"}, {"bii (0x1234),0x11", "ED 12 34 11"},
    {"bii #(x),0x11", "FD 4D 11"}, {"bii #(y),0x11", "FD 5D 11"},
    {"bii #(u),0x11", "FD 6D 11"}, {"bii #(0x1234),0x11", "FD ED 12 34 11"},
    {"cpi a,0x11", "B7 11"}, {"cpi xl,0x11", "4E 11"}, {"cpi yl,0x11", "5E 11"},
    {"cpi ul,0x11", "6E 11"}, {"cpi xh,0x11", "4C 11"}, {"cpi yh,0x11", "5C 11"},
    {"cpi uh,0x11", "6C 11"},
    {"sbi a,0x11", "B1 11"}, {"eai a,0x11", "BD 11"},
    {"inc a", "DD"}, {"inc xl", "40"}, {"inc yl", "50"}, {"inc ul", "60"},
    {"inc xh", "FD 40"}, {"inc yh", "FD 50"}, {"inc uh", "FD 60"},
    {"inc x", "44"}, {"inc y", "54"}, {"inc u", "64"},
    {"dec a", "DF"}, {"dec xl", "42"}, {"dec yl", "52"}, {"dec ul", "62"},
    {"dec xh", "FD 42"}, {"dec yh", "FD 52"}, {"dec uh", "FD 62"},
    {"dec x", "46"}, {"dec y", "56"}, {"dec u", "66"},
    {"drl (x)", "D7"}, {"drl #(x)", "FD D7"},
    {"drr (x)", "D3"}, {"drr #(x)", "FD D3"},
    {"hlt", "FD B1"}, {"ita", "FD BA"}, {"jmp 0x1234", "BA 12 34"},
    {"nop", "38"}, {"off", "FD 4C"},
    {"rdp", "FD C0"}, {"rec", "F9"}, {"rie", "FD BE"},
    {"rol", "DB"}, {"ror", "D1"}, {"rpu", "E3"}, {"rpv", "B8"},
    {"rti", "8A"}, {"rtn", "9A"}, {"sdp", "FD C1"}, {"sec", "FB"},
    {"shl", "D9"}, {"shr", "D5"}, {"sie", "FD 81"}, {"sjp 0x1234", "BE 12 34"},
    {"spu", "E1"}, {"spv", "A8"}, {"tin", "F5"}, {"tta", "FD AA"},
    {"aex", "F1"}, {"am0", "FD CE"}, {"am1", "FD DE"}, {"atp", "FD CC"},
    {"att", "FD EC"}, {"cdv", "FD 8E"}, {"cin", "F7"},
    {"ldi xl,0x11", "4A 11"}, {"ldi yl,0x11", "5A 11"}, {"ldi ul,0x11", "6A 11"},
    {"ldi xh,0x11", "48 11"}, {"ldi yh,0x11", "58 11"}, {"ldi uh,0x11", "68 11"},
    {"ldi a,0x11", "B5 11"}, {"ldi s,0x1234", "AA 12 34"},
    {"lde x", "47"}, {"lde y", "57"}, {"lde u", "67"},
    {"lin x", "45"}, {"lin y", "55"}, {"lin u", "65"},
    {"sde x", "43"}, {"sde y", "53"}, {"sde u", "63"},
    {"sin x", "41"}, {"sin y", "51"}, {"sin u", "61"},
    {"ldx x", "FD 08"}, {"ldx y", "FD 18"}, {"ldx u", "FD 28"},
    {"ldx s", "FD 48"}, {"ldx p", "FD 58"},
    {"stx x", "FD 4A"}, {"stx y", "FD 5A"}, {"stx u", "FD 6A"},
    {"stx s", "FD 4E"}, {"stx p", "FD 5E"},
    {"adr x", "FD CA"}, {"adr y", "FD DA"}, {"adr u", "FD EA"},
    {"psh a", "FD C8"}, {"psh x", "FD 88"}, {"psh y", "FD 98"}, {"psh u", "FD A8"},
    {"pop a", "FD 8A"}, {"pop x", "FD 0A"}, {"pop y", "FD 1A"}, {"pop u", "FD 2A"},
    {"vcs 0xC0", "C3 C0"}, {"vcr 0xC0", "C1 C0"},
    {"vmj 0xC2", "CD C2"}, {"vvs 0xC2", "CF C2"}, {"vzs 0xC2", "CB C2"},
    {"vzr 0xC2", "C9 C2"}, {"vhr 0xC2", "C5 C2"}, {"vhs 0xC2", "C7 C2"},
    {"vej 0xC0", "C0"}, {"vej 0xF6", "F6"},
};

void testCases() {
  for (const Case& c : kCases) {
    std::vector<uint8_t> bytes = parseHex(c.hex);
    DecodedInstruction d = decodeOne(bytes.data(), bytes.size(), 0x1000);
    CHECK(d.valid);
    CHECK(d.length == static_cast<int>(bytes.size()));
    std::string got = renderText(d);
    std::string want = normalizeExpected(c.asmText);
    if (got != want) {
      std::printf("FAIL: hex [%s]: expected %s got %s\n", c.hex, want.c_str(), got.c_str());
      g_failures++;
    }
  }
}

// LOP: "lop ul,5" -> "88 05" per test_lh5801.py (the immediate there is a
// raw literal displacement, not resolved via a label -- see
// opcode_table.h's ControlFlow::ConditionalBranch/branchBackward comment).
// Decode-only check: mnemonic/op1 plus the resolved backward branch target.
void testLop() {
  std::vector<uint8_t> bytes = {0x88, 0x05};
  DecodedInstruction d = decodeOne(bytes.data(), bytes.size(), 0x1000);
  CHECK(d.valid);
  CHECK(d.length == 2);
  CHECK(d.mnemonic == "lop");
  CHECK(d.op1 == Operand::RegUL);
  CHECK(d.op2 == Operand::Branch8);
  CHECK(d.flow == ControlFlow::ConditionalBranch);
  // afterInstr = 0x1000+2 = 0x1002, backward by 5 -> 0x0FFD
  CHECK(d.branchTarget == 0x0FFD);
}

// Explicit forward/backward/self-branch cases, hand-computed exactly like
// test_lh5801.py's own BRANCH_EXPECTED (same source program, same
// addresses) -- verifies decodeOne's branchTarget math independently of
// text rendering.
void testBranchDirections() {
  struct BranchCase {
    uint16_t pc;
    const char* hex;
    uint16_t expectedTarget;
  };
  const BranchCase cases[] = {
      {0x1000, "8E 02", 0x1004},  // bch p2: target 1004, A+2=1002, diff=+2
      {0x1002, "8E 02", 0x1006},  // bch p3: target 1006, A+2=1004, diff=+2
      {0x1007, "9E 02", 0x1007},  // bch p4 (self): target 1007, A+2=1009, diff=-2
      {0x1009, "9E 0B", 0x1000},  // bch p1: target 1000, A+2=100B, diff=-0x0B
      {0x100B, "9B 09", 0x1004},  // bzs p2: target 1004, A+2=100D, diff=-9
  };
  for (const auto& c : cases) {
    std::vector<uint8_t> bytes = parseHex(c.hex);
    DecodedInstruction d = decodeOne(bytes.data(), bytes.size(), c.pc);
    CHECK(d.valid);
    if (d.branchTarget != c.expectedTarget) {
      std::printf("FAIL: pc=0x%04X hex=[%s]: expected target 0x%04X got 0x%04X\n", c.pc, c.hex,
                  c.expectedTarget, d.branchTarget);
      g_failures++;
    }
  }
}

void testVejVectorSlot() {
  std::vector<uint8_t> bytes = {0xC0};
  DecodedInstruction d = decodeOne(bytes.data(), bytes.size(), 0x1000);
  CHECK(d.valid);
  CHECK(d.isVectorCall);
  CHECK(d.vectorSlot == 0xFF00 + 0xC0);
  CHECK(d.flow == ControlFlow::Call);
}

void testUndefinedOpcode() {
  // 0xFF is not assigned in the primary table.
  std::vector<uint8_t> bytes = {0xFF};
  DecodedInstruction d = decodeOne(bytes.data(), bytes.size(), 0x1000);
  CHECK(!d.valid);
}

void testTruncatedInstruction() {
  // ADC (0x1234) needs 3 bytes total; only 2 available.
  std::vector<uint8_t> bytes = {0xA3, 0x12};
  DecodedInstruction d = decodeOne(bytes.data(), bytes.size(), 0x1000);
  CHECK(!d.valid);
}

}  // namespace

int main() {
  testCases();
  testLop();
  testBranchDirections();
  testVejVectorSlot();
  testUndefinedOpcode();
  testTruncatedInstruction();

  if (g_failures == 0) {
    std::printf("All tests passed.\n");
    return 0;
  }
  std::printf("%d check(s) failed.\n", g_failures);
  return 1;
}
