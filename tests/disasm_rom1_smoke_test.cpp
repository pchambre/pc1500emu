// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
//
// Integration smoke test against the real base ROM. Skip-if-missing,
// matching basic_load_roundtrip_test.cpp's pattern -- Sharp's ROM is
// copyrighted and doesn't ship in this repo, so this only exercises
// anything on a machine that already has a real dump.
#include <cstdio>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "analyzer.h"
#include "opcode_table.h"

namespace {

using namespace pc1500::disasm;

int g_failures = 0;

#define CHECK(cond)                                                \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      g_failures++;                                                 \
    }                                                                \
  } while (0)

std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

void testRom1Bin() {
  const std::string path = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  std::vector<uint8_t> rom = readFile(path);
  if (rom.empty()) {
    std::printf("SKIP: testRom1Bin -- ROM1.BIN not found at its known location on this machine.\n");
    return;
  }
  constexpr uint16_t kBase = 0xC000;
  CHECK(rom.size() == 0x4000);  // the real base ROM's known size

  AnalysisResult r = analyzeBaseRom(rom, kBase);

  // The reset vector (FFFEH-FFFFH) must resolve into real, decoded code --
  // confirmed live this session: the ROM settles into its idle loop at
  // E2AAH after boot, so the reset vector's target must be reachable code,
  // not data.
  const VectorTableEntry* resetVec = nullptr;
  for (const auto& v : r.vectorEntries) {
    if (v.name != nullptr && std::string(v.name) == "Reset") resetVec = &v;
  }
  CHECK(resetVec != nullptr);
  if (resetVec != nullptr) {
    CHECK(r.kind[resetVec->target - kBase] == ByteKind::CodeStart);
  }

  // The built-in keyword table: confirmed this session (cross-checked
  // against src/basic/basic_tokens.cpp's independently-transcribed keyword
  // codes) to hold 85 entries (the mechanism doc's "~119" was an estimate)
  // starting at C054H, 2 bytes past where the 52-byte index at C01EH ends
  // -- not immediately adjacent, which is why analyzeBaseRom scans a small
  // window rather than assuming a fixed offset (see analyzer.cpp's
  // comment) -- ending in a genuine low-nibble-0 terminator at C34EH, not
  // a parse failure.
  CHECK(r.baseKeywordTable.tableAddr == 0xC054);
  CHECK(r.baseKeywordTable.entries.size() == 85);
  if (!r.baseKeywordTable.entries.empty()) {
    const auto& first = r.baseKeywordTable.entries[0];
    CHECK(first.name == "AREAD");
    CHECK(first.code == 0xF180);
    // Every entry's address field is a real code entry point -- spot-check
    // a handful (AND/ABS/ATN/ASN/ACS, all independently verified against
    // basic_tokens.cpp this session) actually got traversed into code.
  }
  // Four entries out of 85 are confirmed real anomalies, not parser bugs
  // (their marker/name/code fields all match basic_tokens.cpp exactly, so
  // the table-walk itself is correctly aligned through and past them):
  // NOT and STATUS's stored addresses (0x599EH, 0x5A44H) point below
  // 0xC000H, outside the base ROM chip entirely -- plausibly evaluated
  // inline by the expression parser rather than dispatched to a routine.
  // NEW and STR$'s addresses (0xC80AH, 0xD9CEH) *are* in range but their
  // first byte (0x30H, 0x72H) isn't a defined opcode in either of
  // lh5801.cpp's own opcode tables (confirmed by grepping for those case
  // labels -- absent, not just unseen) -- something other than a plain
  // code entry point going on for these two specifically, not yet
  // understood.
  const std::set<std::string> kKnownExceptions = {"NOT", "STATUS", "NEW", "STR$"};
  for (const auto& e : r.baseKeywordTable.entries) {
    if (kKnownExceptions.count(e.name)) continue;
    CHECK(e.address >= kBase);
    if (e.address < kBase) continue;
    CHECK(r.kind[e.address - kBase] == ByteKind::CodeStart);
  }

  // The SML-key dispatch routine (E366H) this whole investigation was
  // built on this session: LIH/LIL UH/UL,0x76/0x4E (U <- 0x764EH), then
  // OIM (7B0EH),0x01 -- hand-disassembled byte-for-byte earlier this
  // session and now cross-checked against the tool's own decoder.
  CHECK(r.kind[0xE366 - kBase] == ByteKind::CodeStart);
  {
    DecodedInstruction d = decodeOne(&rom[0xE366 - kBase], rom.size() - (0xE366 - kBase), 0xE366);
    CHECK(d.valid);
    CHECK(d.mnemonic == "ldi");
    CHECK(d.op1 == Operand::RegUH);
    CHECK(d.value2 == 0x76);
  }
  {
    DecodedInstruction d = decodeOne(&rom[0xE36A - kBase], rom.size() - (0xE36A - kBase), 0xE36A);
    CHECK(d.valid);
    CHECK(d.mnemonic == "ori");
    CHECK(d.op1 == Operand::Me0Abs);
    CHECK(d.value1 == 0x7B0E);
    CHECK(d.value2 == 0x01);
  }
}

}  // namespace

int main() {
  testRom1Bin();

  if (g_failures == 0) {
    std::printf("All tests passed.\n");
    return 0;
  }
  std::printf("%d check(s) failed.\n", g_failures);
  return 1;
}
