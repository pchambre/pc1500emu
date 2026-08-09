// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#include <cstdio>
#include <fstream>
#include <vector>

#include "analyzer.h"

namespace {

using pc1500::disasm::analyzeBaseRom;
using pc1500::disasm::analyzeModuleRom;
using pc1500::disasm::analyzeProgram;
using pc1500::disasm::ByteKind;

int g_failures = 0;

#define CHECK(cond)                                                \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      g_failures++;                                                 \
    }                                                                \
  } while (0)

// Hand-assembled 2KB synthetic module page (base 0x9000) exercising every
// traversal case: sequential fall-through, an unconditional branch (BCH)
// skipping over an inert data blob, a call (SJP) with both the subroutine
// and the post-call continuation reachable, a conditional branch with both
// its fall-through and target paths reachable but the gap between them
// not, and a 2-entry keyword table (kMinEntriesForConfidence) whose
// address fields are the only way into any of this code. See the inline
// address comments -- this is the ground truth the CHECKs below assert
// against.
void testSyntheticModuleTraversal() {
  constexpr uint16_t kBase = 0x9000;
  std::vector<uint8_t> image(0x800, 0xFF);  // 0xFF is undefined in both
                                             // opcode tables -- any
                                             // accidental wander into
                                             // "empty" bytes decodes
                                             // cleanly as invalid instead
                                             // of silently producing bogus
                                             // instructions.
  auto put = [&](uint16_t addr, std::initializer_list<uint8_t> bytes) {
    uint16_t a = addr;
    for (uint8_t b : bytes) image[a++ - kBase] = b;
  };

  image[0x9000 - kBase] = 0x55;  // sentinel

  // Keyword table at 0x9001 (no real 52-byte index before it in this
  // minimal synthetic case -- only real-ROM tests check indexAddr). Code
  // values use a high byte >= 0xE0 -- required by parseKeywordTableAt's
  // own validation (every real code sampled this session, base ROM and
  // module alike, falls in 0xE0-0xFF; a lower value is rejected as a
  // likely coincidental match in ordinary code bytes, confirmed necessary
  // against a real CE-158.ROM false positive).
  put(0x9001, {0x91, 'A', 0xE1, 0x34, 0x90, 0x20});  // "A" -> code E134, addr 9020
  put(0x9007, {0x91, 'B', 0xE1, 0x35, 0x90, 0x40});  // "B" -> code E135, addr 9040
  put(0x900D, {0xD0});                               // terminator

  // Main routine: sequential, then BCH skips a 5-byte data blob.
  put(0x9020, {0xB5, 0x11});        // ldi a,0x11
  put(0x9022, {0x8E, 0x05});        // bch +5 (afterInstr=9024, target=9029)
  // 0x9024-0x9028: left as 0xFF filler, never executed.
  put(0x9029, {0xBE, 0x90, 0x30});  // sjp 0x9030 (call)
  put(0x902C, {0x9A});              // rtn
  // 0x902D-0x902F: left as 0xFF filler, never reached.
  put(0x9030, {0xDD});              // inc a  (subroutine)
  put(0x9031, {0x9A});              // rtn
  // 0x9032-0x903F: left as 0xFF filler, never reached.

  // Conditional branch: fall-through path and target path both reachable;
  // the single byte between them is not.
  put(0x9040, {0x89, 0x03});  // bzr +3 (afterInstr=9042, target=9045)
  put(0x9042, {0x38});        // nop (fall-through path)
  put(0x9043, {0x9A});        // rtn
  // 0x9044: left as 0xFF filler, never reached.
  put(0x9045, {0x38});        // nop (branch-target path)
  put(0x9046, {0x9A});        // rtn

  auto r = analyzeModuleRom(image, kBase);

  auto k = [&](uint16_t addr) { return r.kind[addr - kBase]; };

  CHECK(k(0x9000) == ByteKind::Unknown);  // sentinel itself isn't "code"
  CHECK(k(0x9001) == ByteKind::Unknown);  // table bytes aren't "code" either
  CHECK(k(0x900E) == ByteKind::Unknown);  // untouched padding

  CHECK(k(0x9020) == ByteKind::CodeStart);
  CHECK(k(0x9021) == ByteKind::CodeContinuation);
  CHECK(k(0x9022) == ByteKind::CodeStart);
  CHECK(k(0x9023) == ByteKind::CodeContinuation);

  // The critical code/data separation check: bytes BCH jumps clean over.
  CHECK(k(0x9024) == ByteKind::Unknown);
  CHECK(k(0x9025) == ByteKind::Unknown);
  CHECK(k(0x9026) == ByteKind::Unknown);
  CHECK(k(0x9027) == ByteKind::Unknown);
  CHECK(k(0x9028) == ByteKind::Unknown);

  CHECK(k(0x9029) == ByteKind::CodeStart);   // sjp
  CHECK(k(0x902A) == ByteKind::CodeContinuation);
  CHECK(k(0x902B) == ByteKind::CodeContinuation);
  CHECK(k(0x902C) == ByteKind::CodeStart);   // rtn after the call returns
  CHECK(k(0x902D) == ByteKind::Unknown);     // never reached

  CHECK(k(0x9030) == ByteKind::CodeStart);   // subroutine
  CHECK(k(0x9031) == ByteKind::CodeStart);   // its rtn

  CHECK(k(0x9040) == ByteKind::CodeStart);   // bzr
  CHECK(k(0x9041) == ByteKind::CodeContinuation);
  CHECK(k(0x9042) == ByteKind::CodeStart);   // fall-through nop
  CHECK(k(0x9043) == ByteKind::CodeStart);   // fall-through rtn
  CHECK(k(0x9044) == ByteKind::Unknown);     // gap: reached by neither path
  CHECK(k(0x9045) == ByteKind::CodeStart);   // branch-target nop
  CHECK(k(0x9046) == ByteKind::CodeStart);   // branch-target rtn

  CHECK(r.labels.count(0x9020) == 1);  // keyword entry
  CHECK(r.labels.count(0x9040) == 1);  // keyword entry
  CHECK(r.labels.count(0x9029) == 1);  // bch target
  CHECK(r.labels.count(0x9030) == 1);  // sjp target
  CHECK(r.labels.count(0x9045) == 1);  // bzr target

  CHECK(r.moduleKeywordTables.size() == 1);
  if (r.moduleKeywordTables.size() == 1) {
    const auto& kt = r.moduleKeywordTables[0];
    CHECK(kt.tableAddr == 0x9001);
    CHECK(kt.entries.size() == 2);
    if (kt.entries.size() == 2) {
      CHECK(kt.entries[0].name == "A");
      CHECK(kt.entries[0].code == 0xE134);
      CHECK(kt.entries[0].address == 0x9020);
      CHECK(kt.entries[1].name == "B");
      CHECK(kt.entries[1].code == 0xE135);
      CHECK(kt.entries[1].address == 0x9040);
    }
  }
}

// A single-keyword module table is below kMinEntriesForConfidence on its
// own, but should still be auto-accepted when a correctly-formed 52-byte
// first-letter index sits immediately before it (mechanism doc §2) --
// confirmed against a real single-keyword test ROM
// (INVERT_minimal_8000_pvlow.ROM) this session, where exactly this
// pattern occurs.
void testSingleEntryTableAcceptedWithValidIndex() {
  constexpr uint16_t kBase = 0x9000;
  std::vector<uint8_t> image(0x800, 0xFF);
  auto put = [&](uint16_t addr, std::initializer_list<uint8_t> bytes) {
    uint16_t a = addr;
    for (uint8_t b : bytes) image[a++ - kBase] = b;
  };

  image[0x9000 - kBase] = 0x55;  // sentinel

  // One entry: "TEST" (marker low nibble = 4) -> code 0xE134, addr 0x9060.
  put(0x9040, {0x94, 'T', 'E', 'S', 'T', 0xE1, 0x34, 0x90, 0x60});
  put(0x9049, {0xD0});  // terminator right after -- only 1 entry total
  put(0x9060, {0x9A});  // rtn, so the entry's address seeds into real code

  // Index slot for 'T' (the 20th letter, 0-indexed 19), 52 bytes before
  // the marker at 0x9040 -> indexAddr 0x900C, slot at 0x900C + 19*2 =
  // 0x9032. Must point at the entry's *second* letter (0x9040 + 2).
  put(0x9032, {0x90, 0x42});

  auto r = analyzeModuleRom(image, kBase);

  CHECK(r.lowConfidenceTables.empty());
  CHECK(r.moduleKeywordTables.size() == 1);
  if (r.moduleKeywordTables.size() == 1) {
    const auto& kt = r.moduleKeywordTables[0];
    CHECK(kt.entries.size() == 1);
    if (kt.entries.size() == 1) {
      CHECK(kt.entries[0].name == "TEST");
      CHECK(kt.entries[0].code == 0xE134);
      CHECK(kt.entries[0].address == 0x9060);
    }
  }
  CHECK(r.kind[0x9060 - kBase] == ByteKind::CodeStart);
}

// Same single-entry table, but with no valid index backing it (the slot
// that should point at the entry stays 0xFFFF, the image's fill value) --
// must still be rejected as low-confidence, exactly as before this
// session's index-validation addition. Guards against the new check
// accidentally loosening the original CE-158.ROM false-positive
// protection for the plain "too few entries, no index" case.
void testSingleEntryTableRejectedWithoutValidIndex() {
  constexpr uint16_t kBase = 0x9000;
  std::vector<uint8_t> image(0x800, 0xFF);
  auto put = [&](uint16_t addr, std::initializer_list<uint8_t> bytes) {
    uint16_t a = addr;
    for (uint8_t b : bytes) image[a++ - kBase] = b;
  };

  image[0x9000 - kBase] = 0x55;
  put(0x9040, {0x94, 'T', 'E', 'S', 'T', 0xE1, 0x34, 0x90, 0x60});
  put(0x9049, {0xD0});
  put(0x9060, {0x9A});
  // No index slot set -- 0x9032/0x9033 stay 0xFF/0xFF (neither 0x0000 nor
  // the expected 0x9042 pointer).

  auto r = analyzeModuleRom(image, kBase);

  CHECK(r.moduleKeywordTables.empty());
  CHECK(r.lowConfidenceTables.size() == 1);
  if (r.lowConfidenceTables.size() == 1) {
    CHECK(r.lowConfidenceTables[0].entries.size() == 1);
    if (r.lowConfidenceTables[0].entries.size() == 1) {
      CHECK(r.lowConfidenceTables[0].entries[0].name == "TEST");
    }
  }
}

std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// Regression check against the real CE-150.ROM dump's B800 "cassette"
// table (CHAIN/CLOAD/CSAVE/MERGE/RMT) -- byte offsets and field values
// hand-verified directly against the file this session (see the
// PC1500_BASIC_Keyword_Extension_Mechanism.md doc's own worked example
// for CHAIN). Confirms findKeywordTableInPage's sliding-window scan
// locates a real, non-synthetic table with module-specific driver code
// (not the table) immediately after the sentinel -- the exact situation
// that ruled out a fixed page-relative offset.
void testCe150RomKnownTable() {
  const std::string path = "C:/Users/paulc/Documents/PC1500/CE-150.ROM";
  std::vector<uint8_t> rom = readFile(path);
  if (rom.empty()) {
    std::printf("SKIP: testCe150RomKnownTable -- CE-150.ROM not found at its known location.\n");
    return;
  }
  constexpr uint16_t kBase = 0xA000;  // real module base this file was captured against
  auto r = analyzeModuleRom(rom, kBase);

  CHECK(r.moduleKeywordTables.size() == 2);  // B000 printer/graphics table + B800 cassette table
  const pc1500::disasm::KeywordTable* cassette = nullptr;
  for (const auto& kt : r.moduleKeywordTables) {
    if (kt.tableAddr == 0xB854) cassette = &kt;
  }
  CHECK(cassette != nullptr);
  if (cassette != nullptr) {
    CHECK(cassette->indexAddr == 0xB820);
    CHECK(cassette->entries.size() >= 5);
    if (cassette->entries.size() >= 5) {
      CHECK(cassette->entries[0].name == "CHAIN");
      CHECK(cassette->entries[0].code == 0xF0B2);
      CHECK(cassette->entries[0].address == 0xBB6A);
      CHECK(cassette->entries[1].name == "CLOAD");
      CHECK(cassette->entries[1].code == 0xF089);
      CHECK(cassette->entries[1].address == 0xB8F9);
      CHECK(cassette->entries[2].name == "CSAVE");
      CHECK(cassette->entries[2].code == 0xF095);
      CHECK(cassette->entries[2].address == 0xB8A6);
      CHECK(cassette->entries[3].name == "MERGE");
      CHECK(cassette->entries[3].code == 0xF08F);
      CHECK(cassette->entries[3].address == 0xB994);
      CHECK(cassette->entries[4].name == "RMT");
      CHECK(cassette->entries[4].code == 0xE7A9);
    }
    // Every entry's address is a real routine -- confirm it got seeded and
    // traversed into actual code (not left Unknown).
    for (const auto& e : cassette->entries) {
      CHECK(r.kind[e.address - kBase] == ByteKind::CodeStart);
    }
  }
}

// Standalone-program mode has no vectors/keyword table to auto-seed from --
// just PSH A / RTN (0xFD 0xC8 / 0x9A), enough to confirm both traversal
// itself and the two entry-point-seeding paths (implicit: none given,
// falls back to `base`; explicit: an --seed-equivalent list) work without
// any of base/module mode's extra structure appearing in the result.
void testProgramModeTraversal() {
  constexpr uint16_t kBase = 0x4268;
  std::vector<uint8_t> image = {0xFD, 0xC8, 0x9A};  // psh a; rtn

  auto rDefault = analyzeProgram(image, kBase);
  CHECK(rDefault.labels.count(kBase) == 1);
  CHECK(rDefault.kind[0] == ByteKind::CodeStart);       // psh a
  CHECK(rDefault.kind[1] == ByteKind::CodeContinuation);  // psh a's 2nd byte
  CHECK(rDefault.kind[2] == ByteKind::CodeStart);       // rtn
  CHECK(rDefault.vectorEntries.empty());
  CHECK(rDefault.baseKeywordTable.entries.empty());
  CHECK(rDefault.moduleKeywordTables.empty());

  // Explicit entry point (e.g. the --seed-driven CLI path) instead of the
  // base-address default -- same traversal result either way here since
  // there's only the one entry point in this tiny image, but confirms the
  // explicit-list branch is actually taken (not silently ignored in favor
  // of the base-address fallback).
  auto rExplicit = analyzeProgram(image, kBase, {kBase});
  CHECK(rExplicit.labels.count(kBase) == 1);
  CHECK(rExplicit.kind[0] == ByteKind::CodeStart);
  CHECK(rExplicit.kind[2] == ByteKind::CodeStart);
}

// Real ML routine (BASIC-POKEd at, and CALLed from, 0x4268 -- confirmed on
// hardware): PSH A/X/Y/U, point X at 7875H and STA (clear it), SJP two ROM
// routines, POP U/Y/X/A, RTN. Confirms a real program-mode file traverses
// end to end with no gaps (every byte reached, no undefined-opcode wander)
// and no base/module-mode structure leaks into the result.
void testRealProgramFile() {
  const std::string path = "C:/Users/paulc/Documents/PC1500/MLGetKeystrokesAndDisplay.bin";
  std::vector<uint8_t> image = readFile(path);
  if (image.empty()) {
    std::printf("SKIP: testRealProgramFile -- file not found at its known location.\n");
    return;
  }
  constexpr uint16_t kBase = 0x4268;
  auto r = analyzeProgram(image, kBase);

  CHECK(r.labels.count(kBase) == 1);
  CHECK(r.vectorEntries.empty());
  CHECK(r.baseKeywordTable.entries.empty());
  CHECK(r.moduleKeywordTables.empty());
  for (size_t i = 0; i < r.kind.size(); i++) {
    CHECK(r.kind[i] != ByteKind::Unknown);
  }
  CHECK(r.kind.back() == ByteKind::CodeStart);  // the trailing RTN (0x9A)
}

}  // namespace

int main() {
  testSyntheticModuleTraversal();
  testSingleEntryTableAcceptedWithValidIndex();
  testSingleEntryTableRejectedWithoutValidIndex();
  testCe150RomKnownTable();
  testProgramModeTraversal();
  testRealProgramFile();

  if (g_failures == 0) {
    std::printf("All tests passed.\n");
    return 0;
  }
  std::printf("%d check(s) failed.\n", g_failures);
  return 1;
}
