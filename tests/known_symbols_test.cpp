// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#include <cstdio>
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

}  // namespace

int main() {
  testFindKnownSymbol();
  testFormatListingAnnotatesKnownAddress();

  if (g_failures == 0) {
    std::printf("All tests passed.\n");
    return 0;
  }
  std::printf("%d check(s) failed.\n", g_failures);
  return 1;
}
