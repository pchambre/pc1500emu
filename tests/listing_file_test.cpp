// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#include <cstdio>
#include <fstream>
#include <string>

#include "listing_file.h"

namespace {

using namespace pc1500::dap;

int g_failures = 0;

#define CHECK(cond)                                                \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      g_failures++;                                                 \
    }                                                                \
  } while (0)

// Matches the format sdcc-pc1500's own test_lh5801.py parses from
// sdaslh5801 -l output: <4-hex address> <hex bytes> <decimal source line>
// <TAB> <source text>. Line 1 and 5 below emit no bytes (a comment, a
// directive) and so must be skipped rather than matched.
const char* kFixture =
    "                     1 \t; a comment, no address\n"
    "E000 3E 05           3 \tld a,#5\n"
    "E002 C3 00 E0        4 \tjp 0xE000\n"
    "E005 90              4 \t; continuation byte, same source line\n"
    "                     6 \t.area CODE\n";

std::string writeFixture() {
  std::string path = "listing_file_test_fixture.lst";
  std::ofstream f(path, std::ios::trunc);
  f << kFixture;
  return path;
}

void testParsesAddressedLinesOnly() {
  std::string path = writeFixture();
  ListingFile lf;
  std::string error;
  CHECK(lf.load(path, &error));
  CHECK(error.empty());
  CHECK(lf.lines().size() == 3);  // lines 1 and 6 emit no bytes
}

void testAddressForLine() {
  std::string path = writeFixture();
  ListingFile lf;
  std::string error;
  CHECK(lf.load(path, &error));

  auto a3 = lf.addressForLine(3);
  CHECK(a3.has_value() && *a3 == 0xE000);

  // Two listing entries share source line 4 (a continuation byte) --
  // addressForLine should resolve to the first (lowest) one, since that's
  // the address a breakpoint on that source line should actually land on.
  auto a4 = lf.addressForLine(4);
  CHECK(a4.has_value() && *a4 == 0xE002);

  CHECK(!lf.addressForLine(1).has_value());  // comment-only line
  CHECK(!lf.addressForLine(999).has_value());
}

void testLineForAddress() {
  std::string path = writeFixture();
  ListingFile lf;
  std::string error;
  CHECK(lf.load(path, &error));

  auto l = lf.lineForAddress(0xE002);
  CHECK(l.has_value() && *l == 4);

  auto l2 = lf.lineForAddress(0xE005);
  CHECK(l2.has_value() && *l2 == 4);

  CHECK(!lf.lineForAddress(0xFFFF).has_value());
}

void testMissingFileFails() {
  ListingFile lf;
  std::string error;
  CHECK(!lf.load("this_file_does_not_exist.lst", &error));
  CHECK(!error.empty());
}

}  // namespace

int main() {
  testParsesAddressedLinesOnly();
  testAddressForLine();
  testLineForAddress();
  testMissingFileFails();

  if (g_failures == 0) {
    std::printf("All tests passed.\n");
    return 0;
  }
  std::printf("%d check(s) failed.\n", g_failures);
  return 1;
}
