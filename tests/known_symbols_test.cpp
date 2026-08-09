// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "analyzer.h"
#include "formatter.h"
#include "known_symbols.h"

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

void testFindKnownSymbol() {
  const KnownSymbol* s = findKnownSymbol(0x764E, /*me1=*/false);
  CHECK(s != nullptr);
  if (s != nullptr) CHECK(std::string(s->name) == "STATUS1");

  // ME0 and ME1 are separate spaces -- F00FH is a real I/O register only
  // in ME1 (OPB); as a plain ME0 byte it's unrelated ordinary memory and
  // must not match.
  CHECK(findKnownSymbol(0xF00F, /*me1=*/true) != nullptr);
  CHECK(findKnownSymbol(0xF00F, /*me1=*/false) == nullptr);

  CHECK(findKnownSymbol(0xE2AA, /*me1=*/false) != nullptr);
  CHECK(findKnownSymbol(0x1234, /*me1=*/false) == nullptr);

  // PC-2 Assembly Language manual entries (Elliott, TRS-80 MC News 1983-84)
  // -- the two routines MLGetKeystrokesAndDisplay.bin calls.
  const KnownSymbol* keyscan = findKnownSymbol(0xE243, /*me1=*/false);
  CHECK(keyscan != nullptr);
  if (keyscan != nullptr) CHECK(std::string(keyscan->name) == "KEYSCAN_WAIT");
  const KnownSymbol* dispchar = findKnownSymbol(0xED4D, /*me1=*/false);
  CHECK(dispchar != nullptr);
  if (dispchar != nullptr) CHECK(std::string(dispchar->name) == "DISP_CHAR_ADV");
}

// Integration check: a synthetic module-mode image whose only instruction
// is the real SML-dispatch fragment from E36AH (ORI (7B0EH),0x01,
// hand-disassembled this session and confirmed against ROM1.BIN) --
// confirms the annotation actually reaches formatListing's output text,
// not just findKnownSymbol in isolation.
void testFormatListingAnnotatesKnownAddress() {
  constexpr uint16_t kBase = 0x9000;
  std::vector<uint8_t> image(0x800, 0xFF);
  // A 2-entry keyword table (kMinEntriesForConfidence) whose first entry's
  // address is the instruction under test, so it gets traversed.
  auto put = [&](uint16_t addr, std::initializer_list<uint8_t> bytes) {
    uint16_t a = addr;
    for (uint8_t b : bytes) image[a++ - kBase] = b;
  };
  image[0x9000 - kBase] = 0x55;
  put(0x9001, {0x91, 'A', 0xE1, 0x00, 0x90, 0x20});  // "A" -> addr 9020
  put(0x9007, {0x91, 'B', 0xE1, 0x01, 0x90, 0x30});  // "B" -> addr 9030
  put(0x9020, {0xEB, 0x7B, 0x0E, 0x01});             // ori (0x7B0E),0x01
  put(0x9024, {0x9A});                               // rtn
  put(0x9030, {0x9A});                               // rtn

  AnalysisResult r = analyzeModuleRom(image, kBase);
  std::string listing = formatListing(image, r);
  CHECK(listing.find("KEYGATE") != std::string::npos);
  CHECK(listing.find("ori (0x7B0E),0x01") != std::string::npos);
}

// A call target (SJP's Imm16 operand, resolved via d.branchTarget) is a
// different code path from a direct memory reference (Me0Abs/Me1Abs,
// resolved via d.value1) -- confirms symbolComment covers both. Without
// this, an SJP to a known ROM routine outside the disassembled image's own
// address range (the common case for a small program-mode file, which is
// exactly what exposed this) would never get annotated at all.
void testSjpTargetAnnotated() {
  constexpr uint16_t kBase = 0x4268;
  std::vector<uint8_t> image = {0xBE, 0xE2, 0x43};  // sjp 0xE243 (KEYSCAN_WAIT)
  AnalysisResult r = analyzeProgram(image, kBase);
  std::string listing = formatListing(image, r);
  CHECK(listing.find("sjp LE243  ; KEYSCAN_WAIT --") != std::string::npos);
}

// Regression test tied directly to the motivating real file: both SJPs
// (to E243H/ED4DH) must be annotated end to end through the real CLI
// pipeline (analyzeProgram + formatListing), not just via findKnownSymbol
// in isolation.
void testRealProgramFileAnnotated() {
  const std::string path = "C:/Users/paulc/Documents/PC1500/MLGetKeystrokesAndDisplay.bin";
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    std::printf("SKIP: testRealProgramFileAnnotated -- file not found at its known location.\n");
    return;
  }
  std::vector<uint8_t> image((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  constexpr uint16_t kBase = 0x4268;
  AnalysisResult r = analyzeProgram(image, kBase);
  std::string listing = formatListing(image, r);
  CHECK(listing.find("sjp LE243  ; KEYSCAN_WAIT --") != std::string::npos);
  CHECK(listing.find("sjp LED4D  ; DISP_CHAR_ADV --") != std::string::npos);
}

std::string writeTempFile(const std::string& contents) {
  std::string path = std::tmpnam(nullptr);
  std::ofstream f(path, std::ios::binary);
  f << contents;
  return path;
}

void testLoadUserSymbolsFileValid() {
  std::string path = writeTempFile(
      "# a comment line, ignored\n"
      "\n"                                       // blank line, ignored
      "0xABCD MYSYM a comment with   spaces\n"
      "1234 OTHERSYM\n");                         // no 0x prefix, no comment
  std::vector<UserSymbol> out;
  std::string error;
  bool ok = loadUserSymbolsFile(path, &out, &error);
  CHECK(ok);
  CHECK(error.empty());
  CHECK(out.size() == 2);
  if (out.size() == 2) {
    CHECK(out[0].addr == 0xABCD);
    CHECK(out[0].name == "MYSYM");
    CHECK(out[0].comment == "a comment with   spaces");
    CHECK(out[1].addr == 0x1234);
    CHECK(out[1].name == "OTHERSYM");
    CHECK(out[1].comment.empty());
  }
  std::remove(path.c_str());
}

void testLoadUserSymbolsFileMalformedLineSkipped() {
  std::string path = writeTempFile(
      "0xGGGG BADHEX not valid hex\n"   // malformed, skipped
      "onlyonetoken\n"                  // malformed, skipped
      "0x1000 GOODSYM this one is fine\n");
  std::vector<UserSymbol> out;
  std::string error;
  bool ok = loadUserSymbolsFile(path, &out, &error);
  CHECK(ok);  // malformed lines are non-fatal
  CHECK(out.size() == 1);
  if (out.size() == 1) {
    CHECK(out[0].addr == 0x1000);
    CHECK(out[0].name == "GOODSYM");
  }
  std::remove(path.c_str());
}

void testLoadUserSymbolsFileMissing() {
  std::vector<UserSymbol> out;
  std::string error;
  bool ok = loadUserSymbolsFile("C:/this/path/does/not/exist.txt", &out, &error);
  CHECK(!ok);
  CHECK(!error.empty());
}

void testLookupSymbolPrecedence() {
  std::vector<UserSymbol> userSymbols;
  UserSymbol override;
  override.addr = 0x764E;  // same address as the built-in STATUS1 entry
  override.me1 = false;
  override.name = "MY_OVERRIDE";
  override.comment = "user's own note";
  userSymbols.push_back(override);

  // User entry wins over the built-in one at the same address.
  auto overridden = lookupSymbol(0x764E, /*me1=*/false, userSymbols);
  CHECK(overridden.has_value());
  if (overridden) CHECK(overridden->name == "MY_OVERRIDE");

  // Falls back to the built-in table when no user entry matches.
  auto builtin = lookupSymbol(0xE2AA, /*me1=*/false, userSymbols);
  CHECK(builtin.has_value());
  if (builtin) CHECK(builtin->name == "IDLE");

  // Neither table has this address.
  CHECK(!lookupSymbol(0x1234, /*me1=*/false, userSymbols).has_value());
}

}  // namespace

int main() {
  testFindKnownSymbol();
  testFormatListingAnnotatesKnownAddress();
  testSjpTargetAnnotated();
  testRealProgramFileAnnotated();
  testLoadUserSymbolsFileValid();
  testLoadUserSymbolsFileMalformedLineSkipped();
  testLoadUserSymbolsFileMissing();
  testLookupSymbolPrecedence();

  if (g_failures == 0) {
    std::printf("All tests passed.\n");
    return 0;
  }
  std::printf("%d check(s) failed.\n", g_failures);
  return 1;
}
