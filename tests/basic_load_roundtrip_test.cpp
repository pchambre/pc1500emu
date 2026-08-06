// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
//
// Loads a real .bas file (containing lowercase text -- the case that
// exposed both the SML/PB3 hardware-gating bug fixed in bus.cpp and the
// typeBasicProgramText SML-toggle logic in text_loader.cpp) via the exact
// same keystroke-driven path the "Load Basic Text" menu item and
// `loadbasictext` FIFO command use, saves it back out through the ROM's
// own tokenizer, and checks the round-tripped text against the original --
// tolerant of the whitespace normalization the ROM's tokenizer/detokenizer
// pair applies (confirmed harmless and expected, not a bug: e.g. "CLEAR
// :WAIT" round-trips as "CLEAR:WAIT", "CHR$ (48+X)" as "CHR$(48+X)").
//
// Needs a real PC-1500 ROM dump and the target .bas file, neither of which
// ship in this repo (Sharp's ROM is copyrighted; the .bas file is
// Paul's own). Skips (prints a message, exits 0) if they're not present at
// their known location on this machine, rather than failing a build on
// any other one.
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "basic_text.h"
#include "bus.h"
#include "keyboard.h"
#include "lh5801.h"
#include "text_loader.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      g_failures++;                                                 \
    }                                                                \
  } while (0)

// Must match src/host/main.cpp's kCyclesPerFrame/kCyclesPerTimerTick --
// typeBasicProgramText takes these as parameters specifically so this test
// doesn't have to link the host executable to get them, but the *values*
// still have to agree with the real app's timing for the ROM's own
// keyboard-scan cadence assumptions to hold.
constexpr int kCyclesPerFrame = 1300000 / 60;
constexpr int kCyclesPerTimerTick = 8;

std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// Strips whitespace the ROM's own tokenize/detokenize pass doesn't
// preserve: spaces immediately before '(' or immediately after a
// keyword/identifier run and before an operand, and spaces around ':' --
// none of that spacing survives a real round trip (confirmed against this
// exact file), so comparing byte-for-byte would fail on cosmetic noise
// rather than real content differences. Also strips newlines, so trailing
// blank lines in the source .bas file (which the ROM's line editor simply
// never sees anything typed for, and so never appear in the saved-back
// text) don't register as a difference either. Collapsing all whitespace
// to nothing (not just runs to a single space) is deliberately aggressive:
// it means this check can't catch a *missing* space that changes meaning
// (e.g. "GOTO40" vs "GOTO 40") or a genuinely missing/extra blank line,
// only content/character differences -- an acceptable tradeoff here since
// the point of this test is the lowercase SML round-trip, not whitespace
// fidelity.
std::string stripSpaces(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    if (c != ' ' && c != '\n' && c != '\r' && c != '\t') out.push_back(c);
  }
  return out;
}

void testHexload1500RoundTrip() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kBasPath = "C:/Users/paulc/Documents/PC1500/hexload1500.bas";

  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> originalBytes = readFile(kBasPath);
  if (rom.empty() || originalBytes.empty()) {
    std::printf(
        "SKIP: testHexload1500RoundTrip -- ROM1.BIN and/or hexload1500.bas "
        "not found at their known location on this machine.\n");
    return;
  }
  std::string original(originalBytes.begin(), originalBytes.end());

  pc1500::Keyboard keyboard;
  pc1500::Bus bus(keyboard);
  lh5801::CPU cpu(bus);
  bus.loadME0(0xC000, rom.data(), rom.size());
  cpu.reset();

  // Run the real boot sequence synchronously (not paced to real wall-clock
  // frames -- irrelevant for a headless test) until the ROM settles into
  // its idle loop, the same state a human would see the machine reach a
  // couple of seconds after power-on. A hard cap avoids ever hanging the
  // test suite if a future regression breaks boot entirely.
  int cyclesSinceTimerTick = 0;
  long bootCycles = 0;
  constexpr long kMaxBootCycles = 20'000'000;
  auto stepOne = [&]() {
    int c = cpu.step();
    int used = (c > 0) ? c : 1;
    bootCycles += used;
    cyclesSinceTimerTick += used;
    bus.advanceCycles(used);
    while (cyclesSinceTimerTick >= kCyclesPerTimerTick) {
      cpu.tickTimer();
      cyclesSinceTimerTick -= kCyclesPerTimerTick;
    }
  };
  while (!cpu.halted() && bootCycles < kMaxBootCycles) stepOne();
  // First reaching halted() is a transient blip, not the machine's actual
  // settled state -- confirmed empirically: driving typeBasicProgramText
  // immediately from here has every single line rejected, even trivial
  // ones ("1 REM ..."), while waiting a couple more (real) seconds first
  // -- exactly what happens naturally in interactive use, where a human
  // never sends a command the instant the window appears -- works fine.
  // Give boot the same few extra seconds' worth of idle cycles here.
  constexpr long kPostBootSettleCycles = 4'000'000;
  for (long i = 0; i < kPostBootSettleCycles; i++) stepOne();
  CHECK(cpu.halted());

  std::string loadError;
  bool loaded = pc1500::basic::typeBasicProgramText(bus, cpu, original, kCyclesPerFrame,
                                                     kCyclesPerTimerTick, &loadError);
  CHECK(loaded);
  if (!loaded) {
    std::printf("  loadError: %s\n", loadError.c_str());
    return;
  }

  std::string saveError;
  std::vector<uint8_t> tokenized = pc1500::basic::readBasicProgramBytes(bus, &saveError);
  CHECK(!tokenized.empty());
  std::string roundTripped;
  CHECK(pc1500::basic::detokenizeBasicProgram(tokenized, &roundTripped, &saveError));

  // The specific regression this test exists for: the two lowercase-text
  // INPUT prompts (lines 20/30) must survive the round trip with their
  // case intact, not silently uppercased.
  CHECK(roundTripped.find("Anfangsadresse (dez.):") != std::string::npos);
  CHECK(roundTripped.find("Endadresse (dez.):") != std::string::npos);

  // Whole-file check, whitespace-insensitive (see stripSpaces's comment).
  CHECK(stripSpaces(roundTripped) == stripSpaces(original));
  if (stripSpaces(roundTripped) != stripSpaces(original)) {
    std::printf("---- original (spaces stripped) ----\n%s\n", stripSpaces(original).c_str());
    std::printf("---- round-tripped (spaces stripped) ----\n%s\n", stripSpaces(roundTripped).c_str());
  }
}

}  // namespace

int main() {
  testHexload1500RoundTrip();

  if (g_failures == 0) {
    std::printf("All tests passed.\n");
    return 0;
  }
  std::printf("%d check(s) failed.\n", g_failures);
  return 1;
}
