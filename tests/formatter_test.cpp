// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#include <cstdio>
#include <string>
#include <vector>

#include "analyzer.h"
#include "formatter.h"

namespace {

using pc1500::disasm::analyzeModuleRom;
using pc1500::disasm::analyzeProgram;
using pc1500::disasm::AsmDialect;
using pc1500::disasm::formatListing;
using pc1500::disasm::FormatOptions;

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

// AsmDialect::Tasm renders uppercase mnemonics/registers and "$"-prefixed
// hex, with no ".area" wrapper (see AsmDialect's own comment in
// formatter.h) -- the reverse direction from tasm_convert.h's TASM->sdas
// converter, for producing output a TASM user can read, not for
// reassembly by this project's own sdaslh5801-based toolchain.
void testTasmDialectEmit() {
  constexpr uint16_t kBase = 0x8000;
  std::vector<uint8_t> image(0x10, 0xFF);
  // ldi a,0x11 ; sta (0x8010) ; rtn
  image[0] = 0xB5;
  image[1] = 0x11;
  image[2] = 0xAE;
  image[3] = 0x80;
  image[4] = 0x10;
  image[5] = 0x9A;
  image.resize(6);

  auto r = analyzeProgram(image, kBase);
  FormatOptions opts;
  opts.dialect = AsmDialect::Tasm;
  std::string listing = formatListing(image, r, opts);

  CHECK(listing.find("\t.ORG $8000\n") != std::string::npos);
  CHECK(listing.find(".area") == std::string::npos);
  CHECK(listing.find("\tLDI A,$11\n") != std::string::npos);
  CHECK(listing.find("\tSTA (") != std::string::npos);
  CHECK(listing.find("\tRTN\n") != std::string::npos);
  // Sdas dialect (the default) is untouched by this -- still lowercase,
  // still "0x"-prefixed, still wrapped in ".area".
  std::string sdasListing = formatListing(image, r);
  CHECK(sdasListing.find("\t.area CODE (ABS)\n") != std::string::npos);
  CHECK(sdasListing.find("\tldi a,0x11\n") != std::string::npos);
}

}  // namespace

int main() {
  testKeywordBackLink();
  testTasmDialectEmit();

  if (g_failures == 0) {
    std::printf("All tests passed.\n");
    return 0;
  }
  std::printf("%d check(s) failed.\n", g_failures);
  return 1;
}
