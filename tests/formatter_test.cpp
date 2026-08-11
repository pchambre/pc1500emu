// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#include <cstdio>
#include <string>
#include <vector>

#include "analyzer.h"
#include "formatter.h"

namespace {

using pc1500::disasm::analyzeModuleRom;
using pc1500::disasm::formatListing;

int g_failures = 0;

#define CHECK(cond)                                                \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      g_failures++;                                                 \
    }                                                                \
  } while (0)

// A keyword table entry's routine address should get a "; NAME keyword"
// back-link comment on its own label line -- the gap this closes: before
// this, a reader could see e.g. "WAIT" -> LE86A in the keyword table but
// find no indication at LE86A itself that this is WAIT's implementation
// (had to cross-reference the table by hand). Uses the same
// hand-assembled synthetic module pattern as
// disasm_analyzer_test.cpp's testSyntheticModuleTraversal.
void testKeywordBackLink() {
  constexpr uint16_t kBase = 0x9000;
  std::vector<uint8_t> image(0x800, 0xFF);
  auto put = [&](uint16_t addr, std::initializer_list<uint8_t> bytes) {
    uint16_t a = addr;
    for (uint8_t b : bytes) image[a++ - kBase] = b;
  };

  image[0x9000 - kBase] = 0x55;  // sentinel
  // Two keyword entries (kMinEntriesForConfidence), same as the analyzer
  // test: "A" -> 0x9020, "B" -> 0x9040. Both routine addresses are also
  // each other's fall-through-adjacent, so this doubles as a check that
  // the back-link doesn't spuriously appear on the *wrong* address.
  put(0x9001, {0x91, 'A', 0xE1, 0x34, 0x90, 0x20});  // "A" -> code E134, addr 9020
  put(0x9007, {0x91, 'B', 0xE1, 0x35, 0x90, 0x40});  // "B" -> code E135, addr 9040
  put(0x900D, {0xD0});                               // terminator

  put(0x9020, {0xB5, 0x11});  // ldi a,0x11  (keyword "A"'s own routine)
  put(0x9022, {0x9A});        // rtn
  put(0x9040, {0xB5, 0x22});  // ldi a,0x22  (keyword "B"'s own routine)
  put(0x9042, {0x9A});        // rtn

  auto r = analyzeModuleRom(image, kBase);
  std::string listing = formatListing(image, r);

  CHECK(listing.find("L9020:  ; A keyword\n") != std::string::npos);
  CHECK(listing.find("L9040:  ; B keyword\n") != std::string::npos);
  // The keyword table's own entries reference the routine address via a
  // plain "L9020"/"L9040" operand (not a label line) -- shouldn't be
  // confused with the back-link, and shouldn't itself carry one.
  CHECK(listing.find(".dw L9020  ; address") != std::string::npos);
  CHECK(listing.find(".dw L9040  ; address") != std::string::npos);
}

}  // namespace

int main() {
  testKeywordBackLink();

  if (g_failures == 0) {
    std::printf("All tests passed.\n");
    return 0;
  }
  std::printf("%d check(s) failed.\n", g_failures);
  return 1;
}
