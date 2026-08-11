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
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
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
constexpr int kCyclesPerSecond = 1300000;
constexpr int kCyclesPerFrame = kCyclesPerSecond / 60;
constexpr int kCyclesPerTimerTick = 8;

std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// Same logic as main.cpp's "displaytext" FIFO command: the ROM's own 80-byte
// LCD text buffer (7BB0H-7BFFH), read as ASCII up to its 0DH terminator --
// ground truth for what's actually on screen, definitive proof a PRINT
// statement after WAIT/BEEP actually executed (as opposed to inferring
// completion from which ROM addresses got visited).
std::string readDisplayText(pc1500::Bus& bus) {
  constexpr uint16_t kDisplayTextBufBase = 0x7BB0;
  constexpr int kDisplayTextBufLen = 80;
  std::string text;
  for (int i = 0; i < kDisplayTextBufLen; i++) {
    uint8_t b = bus.readME0(static_cast<uint16_t>(kDisplayTextBufBase + i));
    if (b == 0x0D) break;
    text += (b >= 0x20 && b < 0x7F) ? static_cast<char>(b) : '?';
  }
  return text;
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

// Loads `rom` and boots+settles a fresh Bus/CPU pair, ready for
// typeBasicProgramText -- shared setup for every test below that needs a
// live ROM (not just the file-round-trip test above, which inlines its
// own copy since it predates this helper).
struct BootedMachine {
  pc1500::Keyboard keyboard;
  pc1500::Bus bus{keyboard};
  lh5801::CPU cpu{bus};
  int cyclesSinceTimerTick = 0;
};

// extRam4800Bytes: emulated module RAM to install at the 4800H window
// before boot (e.g. 0x2000 for an 8K module -- a real 1982-era hardware
// option, see README's "Extension RAM (4800H)" section) -- the ROM only
// detects installed extension RAM at reset/cold-start, not on the fly, so
// this has to be set before the boot loop below runs, not after.
std::unique_ptr<BootedMachine> bootAndSettle(const std::vector<uint8_t>& rom,
                                              size_t extRam4800Bytes = 0) {
  auto m = std::make_unique<BootedMachine>();
  // Freeze the RTC's clock to a fake, manually-advanced one (see
  // Upd1990ac::useManualClock()) so real elapsed wall-clock time
  // (including this test binary's own printf/instrumentation overhead)
  // never bleeds into TP timing -- without this, the same test could
  // observe different RTC state run to run, purely from host timing
  // jitter, since tp()/consumeRisingEdge() otherwise read the real clock.
  m->bus.ioPort().useManualRtcClock();
  m->bus.loadME0(0xC000, rom.data(), rom.size());
  if (extRam4800Bytes > 0) m->bus.setExtRam4800Size(extRam4800Bytes);
  m->cpu.reset();
  long bootCycles = 0;
  auto stepOne = [&]() {
    int c = m->cpu.step();
    int used = (c > 0) ? c : 1;
    bootCycles += used;
    m->cyclesSinceTimerTick += used;
    m->bus.advanceCycles(used);
    while (m->cyclesSinceTimerTick >= kCyclesPerTimerTick) {
      m->cpu.tickTimer();
      m->cyclesSinceTimerTick -= kCyclesPerTimerTick;
    }
  };
  while (!m->cpu.halted() && bootCycles < 20'000'000) stepOne();
  for (long i = 0; i < 4'000'000; i++) stepOne();
  return m;
}

// A ~100-char single BASIC line with several colon-separated statements
// (the real line 90 from the user's own Blackjack.bas, before it got
// word-wrapped in transcription) -- needs 3 passes: the ordinary 79-char
// fresh-line limit only fits the first few statements, and each
// subsequent LIST-and-append pass has progressively less room as the
// line's own stored (tokenized, compact) size grows. Exercises the
// specific real-world case this feature exists for.
void testLongLineMultiPass() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  std::vector<uint8_t> rom = readFile(kRomPath);
  if (rom.empty()) {
    std::printf("SKIP: testLongLineMultiPass -- ROM1.BIN not found.\n");
    return;
  }
  auto m = bootAndSettle(rom);
  std::string longLine =
      "90 \"1\":C=C+D:E=INT (C/256):POKE B,E:POKE B+1,C-(E*256):PRINT C:CURSOR 5:"
      "B=B+(PEEK (B+2))+3";
  CHECK(longLine.size() > 79);

  std::string loadError;
  bool loaded = pc1500::basic::typeBasicProgramText(m->bus, m->cpu, longLine, kCyclesPerFrame,
                                                     kCyclesPerTimerTick, &loadError);
  CHECK(loaded);
  if (!loaded) {
    std::printf("  loadError: %s\n", loadError.c_str());
    return;
  }

  std::string saveError;
  std::vector<uint8_t> tokenized = pc1500::basic::readBasicProgramBytes(m->bus, &saveError);
  std::string detok;
  CHECK(pc1500::basic::detokenizeBasicProgram(tokenized, &detok, &saveError));
  CHECK(stripSpaces(detok).find(stripSpaces(longLine)) != std::string::npos);
  if (stripSpaces(detok).find(stripSpaces(longLine)) == std::string::npos) {
    std::printf("  detok: %s\n", detok.c_str());
  }
}

// A colon *inside* a quoted string, in a line otherwise long enough to
// need a second pass -- the segment splitter must not treat that colon
// as a statement boundary (which would produce an unterminated string in
// one pass and a syntactically-nonsensical fragment in the next).
void testLongLineColonInsideQuotes() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  std::vector<uint8_t> rom = readFile(kRomPath);
  if (rom.empty()) {
    std::printf("SKIP: testLongLineColonInsideQuotes -- ROM1.BIN not found.\n");
    return;
  }
  auto m = bootAndSettle(rom);
  // "A:B" (colon inside quotes) followed by enough padding statements to
  // force a second pass. Uses PRINT (tokenizes well) rather than bare
  // assignments for the padding -- bare "Y0=1"-style statements barely
  // compress at all, so enough of them to push the *raw* length over 79
  // pushes the final *tokenized* size over the (also empirically
  // confirmed, see kContinuationPassBudget) ~78-byte ceiling on a single
  // line's total stored content, which is a real hardware limit no
  // amount of extra passes can work around -- not a bug, but also not
  // what this test is trying to exercise, so avoid it here.
  std::string longLine = "10 INPUT \"A:B\";X";
  for (int i = 0; i < 8; i++) longLine += ":PRINT \"Y" + std::to_string(i) + "\"";
  CHECK(longLine.size() > 79);

  std::string loadError;
  bool loaded = pc1500::basic::typeBasicProgramText(m->bus, m->cpu, longLine, kCyclesPerFrame,
                                                     kCyclesPerTimerTick, &loadError);
  CHECK(loaded);
  if (!loaded) {
    std::printf("  loadError: %s\n", loadError.c_str());
    return;
  }
  std::string saveError;
  std::vector<uint8_t> tokenized = pc1500::basic::readBasicProgramBytes(m->bus, &saveError);
  std::string detok;
  CHECK(pc1500::basic::detokenizeBasicProgram(tokenized, &detok, &saveError));
  CHECK(detok.find("A:B") != std::string::npos);
}

// A single statement (no colon anywhere) longer than any one pass could
// ever fit -- there's no valid split point, so this must fail cleanly
// with a clear error rather than silently truncating or corrupting the
// program area.
void testLongLineUnsplittableFails() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  std::vector<uint8_t> rom = readFile(kRomPath);
  if (rom.empty()) {
    std::printf("SKIP: testLongLineUnsplittableFails -- ROM1.BIN not found.\n");
    return;
  }
  auto m = bootAndSettle(rom);
  std::string longLine = "10 PRINT \"" + std::string(100, 'X') + "\"";
  CHECK(longLine.size() > 79);

  std::string loadError;
  bool loaded = pc1500::basic::typeBasicProgramText(m->bus, m->cpu, longLine, kCyclesPerFrame,
                                                     kCyclesPerTimerTick, &loadError);
  CHECK(!loaded);
  CHECK(!loadError.empty());
}

// The user's own real-world file (following this project's established
// "skip if not present at its known local path" convention, same as
// ROM1.BIN/hexload1500.bas/CE-150.ROM): 24 genuine long lines (80-108
// chars) from a real 1984 listing, none of them synthetic test data. The
// full program's tokenized size doesn't fit in the stock 2K of built-in
// RAM (confirmed: the ROM's own memory-full rejection fires partway
// through line 670) -- a real capacity limit, not a bug in the long-line
// typing feature -- so this boots with an emulated 8K expansion module at
// the 4800H window (a real 1982-era hardware option), matching what a
// real owner running a program this size would have needed.
void testBlackjackRoundTrip() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kBasPath = "C:/Users/paulc/Documents/PC1500/Blackjack.bas";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> originalBytes = readFile(kBasPath);
  if (rom.empty() || originalBytes.empty()) {
    std::printf("SKIP: testBlackjackRoundTrip -- ROM1.BIN and/or Blackjack.bas not found.\n");
    return;
  }
  std::string original(originalBytes.begin(), originalBytes.end());
  auto m = bootAndSettle(rom, 0x2000);

  std::string loadError;
  bool loaded = pc1500::basic::typeBasicProgramText(m->bus, m->cpu, original, kCyclesPerFrame,
                                                     kCyclesPerTimerTick, &loadError);
  CHECK(loaded);
  if (!loaded) {
    std::printf("  loadError: %s\n", loadError.c_str());
    return;
  }

  std::string saveError;
  std::vector<uint8_t> tokenized = pc1500::basic::readBasicProgramBytes(m->bus, &saveError);
  CHECK(!tokenized.empty());
  std::string roundTripped;
  CHECK(pc1500::basic::detokenizeBasicProgram(tokenized, &roundTripped, &saveError));
  CHECK(stripSpaces(roundTripped) == stripSpaces(original));
  if (stripSpaces(roundTripped) != stripSpaces(original)) {
    std::printf("---- original (spaces stripped) ----\n%s\n", stripSpaces(original).c_str());
    std::printf("---- round-tripped (spaces stripped) ----\n%s\n",
                stripSpaces(roundTripped).c_str());
  }
}

// Another real-world file (Paul's own, same "skip if not present"
// convention). The specific regression this test exists for: line 35's
// greedy 79-char first-pass packing lands mid-"A$(X,Y)" (right after the
// "("), which the ROM's tokenizer rejects outright rather than storing
// the partial expression -- typeLongLine has to notice the rejection and
// back the pass off (see its own comment) rather than failing the whole
// load. DIM A$(30,30)*1 alone is 900+ bytes, well past the stock 2K, so
// this boots with the same 8K expansion module as testBlackjackRoundTrip.
void testDungeonQuestRoundTrip() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kBasPath = "C:/Users/paulc/Documents/PC1500/DungeonQuest.bas";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> originalBytes = readFile(kBasPath);
  if (rom.empty() || originalBytes.empty()) {
    std::printf("SKIP: testDungeonQuestRoundTrip -- ROM1.BIN and/or DungeonQuest.bas not found.\n");
    return;
  }
  std::string original(originalBytes.begin(), originalBytes.end());
  auto m = bootAndSettle(rom, 0x2000);

  std::string loadError;
  bool loaded = pc1500::basic::typeBasicProgramText(m->bus, m->cpu, original, kCyclesPerFrame,
                                                     kCyclesPerTimerTick, &loadError);
  CHECK(loaded);
  if (!loaded) {
    std::printf("  loadError: %s\n", loadError.c_str());
    return;
  }

  std::string saveError;
  std::vector<uint8_t> tokenized = pc1500::basic::readBasicProgramBytes(m->bus, &saveError);
  CHECK(!tokenized.empty());
  std::string roundTripped;
  CHECK(pc1500::basic::detokenizeBasicProgram(tokenized, &roundTripped, &saveError));
  CHECK(stripSpaces(roundTripped) == stripSpaces(original));
  if (stripSpaces(roundTripped) != stripSpaces(original)) {
    std::printf("---- original (spaces stripped) ----\n%s\n", stripSpaces(original).c_str());
    std::printf("---- round-tripped (spaces stripped) ----\n%s\n",
                stripSpaces(roundTripped).c_str());
  }
}

// Actually RUNs a loaded program (not just loads/tokenizes it): types "RUN"
// + Enter at the ROM's command prompt using the same keystroke primitives
// typeBasicProgramText uses internally (charToTapActions), then steps the
// CPU with *simulated real-time frame pacing* -- calling
// Bus::advanceRealTime() once per synthetic ~60fps frame, the same way
// main.cpp's live loop does. This matters specifically because WAIT/BEEP's
// gap timer (see the Upd1990ac class comment in bus.h) depends on real
// wall-clock time advancing, not CPU cycles -- typeBasicProgramText's own
// stepCycles() helper never calls advanceRealTime() at all, since it only
// needs to drive the ROM's line editor, not run a program's own statements.
// This is exactly the code path this project's test suite had zero
// coverage of before this file (confirmed by grepping every existing test
// for a `RUN` keystroke -- none exists).
//
// Runs for `maxFrames` synthetic frames, sampling cpu.p() once per frame.
// Returns the set of unique PCs visited across the whole run: a small,
// bounded set despite a long run is the signature of a genuine infinite
// loop (confirmed live against the real freeze this test targets -- a
// register-level single-step trace showed the CPU cycling through fewer
// than 100 unique addresses indefinitely), whereas healthy progress through
// the BASIC interpreter/program visits many distinct addresses.
std::set<uint16_t> runProgramAndSamplePc(pc1500::Bus& bus, lh5801::CPU& cpu,
                                          int& cyclesSinceTimerTick, int maxFrames,
                                          const char* watchSubstring = nullptr,
                                          bool* watchFoundOut = nullptr) {
  // Fine-grained RTC advance -- one call per *instruction*, scaled by its
  // own cycle count, rather than one coarse once-per-frame jump. See
  // testWaitPollTrace's own identical fix for why: a once-per-frame
  // (1/60s) advance makes the RTC's state change all at once at frame
  // boundaries, which substantially inflates the odds of two
  // adjacent-but-separate register reads (OPB's live level, IF's latched
  // edge, a few CPU cycles apart -- see _bisect/rom1.asm's LE89C-LE8BC)
  // straddling a tick and seeing inconsistent state, compared to a
  // smoothly-advancing clock (real wall-clock time in production, or
  // this) where that same race is proportional to how close together the
  // two reads are relative to the RTC's own period, not to the test's own
  // frame granularity.
  // Rigorous, execution-only signal for watchSubstring: hooks DISP_N_CHARS
  // (LE00H)'s own call site and checks the bytes it's about to render,
  // rather than scanning memory for the substring anywhere. Scanning
  // memory turned out to be a false-positive trap: a PRINT statement's own
  // string *argument* is part of the tokenized program's stored text,
  // present in RAM from the moment the program is typed in -- long before
  // (or entirely without) that PRINT statement ever actually executing.
  // DISP_N_CHARS only ever gets called as part of genuinely rendering
  // characters, so catching it here can't be fooled the same way.
  bool watchFound = false;
  auto checkDispNChars = [&]() {
    if (watchSubstring && !watchFound && cpu.p() == 0xED00) {
      uint16_t start = cpu.u();
      uint8_t len = cpu.a();
      std::string text;
      for (int i = 0; i < len && i < 40; i++) {
        uint8_t b = bus.readME0(static_cast<uint16_t>(start + i));
        text += (b >= 0x20 && b < 0x7F) ? static_cast<char>(b) : '\x01';
      }
      if (text.find(watchSubstring) != std::string::npos) watchFound = true;
    }
  };
  auto stepOne = [&]() {
    checkDispNChars();
    int c = cpu.step();
    int used = (c > 0) ? c : 1;
    bus.ioPort().advanceManualRtcClock(static_cast<double>(used) / kCyclesPerSecond);
    cyclesSinceTimerTick += used;
    bus.advanceCycles(used);
    while (cyclesSinceTimerTick >= kCyclesPerTimerTick) {
      cpu.tickTimer();
      cyclesSinceTimerTick -= kCyclesPerTimerTick;
    }
  };
  auto stepCycles = [&](long cycles) {
    for (long i = 0; i < cycles;) {
      checkDispNChars();
      int c = cpu.step();
      int used = (c > 0) ? c : 1;
      bus.ioPort().advanceManualRtcClock(static_cast<double>(used) / kCyclesPerSecond);
      i += used;
      cyclesSinceTimerTick += used;
      bus.advanceCycles(used);
      while (cyclesSinceTimerTick >= kCyclesPerTimerTick) {
        cpu.tickTimer();
        cyclesSinceTimerTick -= kCyclesPerTimerTick;
      }
    }
  };
  auto runKeyAction = [&](pc1500::Key key, bool pressed, int framesToWait) {
    bus.setKeyState(key, pressed);
    stepCycles(static_cast<long>(framesToWait) * kCyclesPerFrame);
  };
  auto typeChar = [&](char c) {
    std::deque<pc1500::basic::QueuedKeyAction> actions;
    if (!pc1500::basic::charToTapActions(c, &actions)) return;
    for (const auto& a : actions) runKeyAction(a.key, a.pressed, a.framesToWait);
  };
  runKeyAction(pc1500::Key::Cl, true, pc1500::basic::kTapFrames);
  runKeyAction(pc1500::Key::Cl, false, pc1500::basic::kIdleFrames);
  // RUN only works from RUN mode, not PRO mode (the mode a machine is left
  // in right after loading/editing a program via typeBasicProgramText) --
  // typing RUN while still in PRO mode throws ERROR 26 instead of running.
  // Confirmed live: the MODE key toggles PRO/RUN.
  runKeyAction(pc1500::Key::Mode, true, pc1500::basic::kTapFrames);
  runKeyAction(pc1500::Key::Mode, false, pc1500::basic::kIdleFrames);
  for (char c : std::string("RUN")) typeChar(c);
  runKeyAction(pc1500::Key::Ent, true, pc1500::basic::kTapFrames);
  runKeyAction(pc1500::Key::Ent, false, pc1500::basic::kIdleFrames);
  // 4x the usual per-line settle time typeBasicProgramText uses elsewhere,
  // matching its own comment: tokenization/dispatch work isn't necessarily
  // done the instant Enter is processed.
  stepCycles(4L * pc1500::basic::kIdleFrames * kCyclesPerFrame);

  uint8_t ind2AfterRun = bus.readME0(0x764F);
  std::printf("  runProgramAndSamplePc: after RUN+Enter: PC=%04X ind2=%02X (run=%d pro=%d)\n",
              cpu.p(), ind2AfterRun, (ind2AfterRun & 0x40) != 0, (ind2AfterRun & 0x20) != 0);

  std::set<uint16_t> visited;
  constexpr double kFrameSeconds = 1.0 / 60.0;
  int tpTransitions = 0;
  int tpConfiguredAtFrame = -1;
  int ifBit1SetFrames = 0;
  // NOTE: F00BH/F00FH are ME1 (I/O-controller) addresses, not ME0 -- must
  // peek via bus.ioPort().read(reg), NOT bus.readME0()/readME1(addr)
  // directly with the full address (readME0() would silently return
  // unrelated RAM/garbage since it never routes through the I/O
  // controller at all; an earlier version of this diagnostic used
  // readME0() here and produced misleading "IF bit1 always set" data that
  // sent a chunk of this investigation down the wrong path).
  bool lastTp = (bus.ioPort().read(0x0F) & 0x20) != 0;
  for (int frame = 0; frame < maxFrames; frame++) {
    stepCycles(kCyclesPerFrame);
    if (tpConfiguredAtFrame < 0 && bus.ioPort().rtcDebugTpConfigured()) tpConfiguredAtFrame = frame;
    visited.insert(cpu.p());
    bool tp = (bus.ioPort().read(0x0F) & 0x20) != 0;
    if (tp != lastTp) tpTransitions++;
    lastTp = tp;
    if (bus.ioPort().read(0x0B) & 0x02) ifBit1SetFrames++;
  }
  if (watchFoundOut) *watchFoundOut = watchFound;
  std::printf("  runProgramAndSamplePc: IF bit1 (TP edge latch) seen SET on %d/%d frame-end samples\n",
              ifBit1SetFrames, maxFrames);
  std::printf("  runProgramAndSamplePc: TP (F00FH bit 5) transitioned %d time(s) over %d frames "
              "(tpConfigured first true at frame %d)\n",
              tpTransitions, maxFrames, tpConfiguredAtFrame);
  std::printf("  runProgramAndSamplePc: rtc tpLevel=%d totalToggleCount=%d\n",
              bus.ioPort().rtcDebugTpLevel(), bus.ioPort().rtcDebugTotalToggleCount());
  std::printf("  runProgramAndSamplePc: rtc tpConfigured=%d tpRateHz=%d\n",
              bus.ioPort().rtcDebugTpConfigured(), bus.ioPort().rtcDebugTpRateHz());
  std::printf("  runProgramAndSamplePc: OPC writes=%d lastValue=0x%02X\n",
              bus.ioPort().debugOpcWriteCount(), bus.ioPort().debugLastOpcValue());
  std::printf("  runProgramAndSamplePc: OPC write history: ");
  for (uint8_t v : bus.ioPort().debugOpcWriteHistory()) {
    std::printf("%02X(stb=%d,c0=%d,c1=%d,c2=%d) ", v, (v & 0x02) != 0, (v & 0x08) != 0,
                (v & 0x10) != 0, (v & 0x20) != 0);
  }
  std::printf("\n");
  return visited;
}

// The specific regression this test exists for: DungeonQuest.bas line 2
// (`WAIT 200:PRINT "..."`) hangs forever when actually RUN, confirmed live
// (user bisected the real program down to this exact line; on real
// hardware WAIT 200 holds the banner text on screen for ~4 seconds then
// continues -- in the emulator it flashes briefly then the CPU gets stuck
// cycling through a small ROM address range indefinitely, never reaching
// line 3). No extRAM needed -- this doesn't touch DIM/arrays at all,
// isolating WAIT itself from the earlier (wrong) extRAM/DIM hypothesis.
// Control test: does RUN work at all via this test's keystroke-driven RUN
// mechanism, for a program with no WAIT/BEEP/RTC involvement whatsoever?
// If this *also* gets stuck in the same address range as testWaitHangsOnRun,
// the bug isn't WAIT-specific -- it's something wrong with RUN dispatch
// itself (or this test's own RUN-typing mechanism).
// Comparison test: BEEP's repeat-gap timer uses the same underlying RTC
// TP-edge mechanism as WAIT (per Upd1990ac's own class comment in bus.h --
// "BASIC BEEP's repeat-gap wait... issues value 0x20... that's what
// resolved the mystery of what BEEP's gap timer actually depends on"),
// and is documented/confirmed already working. If BEEP completes cleanly
// here while WAIT hangs, that isolates the bug to WAIT's own code path
// specifically, not the shared RTC/TP machinery (which testWaitHangsOnRun
// independently confirmed *is* oscillating correctly at the C++ level).
void testBeepCompletesOnRun() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  std::vector<uint8_t> rom = readFile(kRomPath);
  if (rom.empty()) {
    std::printf("SKIP: testBeepCompletesOnRun -- ROM1.BIN not found.\n");
    return;
  }
  auto m = bootAndSettle(rom);
  // WAIT 0 first: per the user's correction, WAIT sets a *persistent*
  // post-PRINT pause duration -- with none ever set, PRINT's default is to
  // wait for ENTER indefinitely, which this headless test never sends.
  // Without this, a hang here would be indistinguishable from that (and
  // was: this is what invalidated the original "BEEP hangs identically to
  // WAIT" finding).
  std::string program = "1 WAIT 0:BEEP 5\n2 PRINT \"line 2 reached\"\n";

  std::string loadError;
  bool loaded = pc1500::basic::typeBasicProgramText(m->bus, m->cpu, program, kCyclesPerFrame,
                                                     kCyclesPerTimerTick, &loadError);
  CHECK(loaded);
  if (!loaded) {
    std::printf("  loadError: %s\n", loadError.c_str());
    return;
  }

  bool sawLine2 = false;
  std::set<uint16_t> visited =
      runProgramAndSamplePc(m->bus, m->cpu, m->cyclesSinceTimerTick, 600, "line 2 reached", &sawLine2);
  std::printf("  testBeepCompletesOnRun: visited %zu unique PC(s) over 600 frames; displayText=\"%s\"\n",
              visited.size(), readDisplayText(m->bus).c_str());
  CHECK(sawLine2);
  if (visited.size() < 150) {
    std::printf("  PCs visited: ");
    for (uint16_t pc : visited) std::printf("%04X ", pc);
    std::printf("\n");
  }
}

// User's question: does bare "BEEP 5" (single arg -- possibly just
// frequency/duration, no repeat count) even exercise the same repeat-gap
// mechanism as DungeonQuest.bas's own BEEP calls, which are all 3-arg
// (e.g. line 4010's "FOR I=66TO 33STEP -1:BEEP 1,I,20:NEXT I")? Tests
// the 3-arg form directly to check whether it hangs the same way.
void testBeep3ArgCompletesOnRun() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  std::vector<uint8_t> rom = readFile(kRomPath);
  if (rom.empty()) {
    std::printf("SKIP: testBeep3ArgCompletesOnRun -- ROM1.BIN not found.\n");
    return;
  }
  auto m = bootAndSettle(rom);
  std::string program = "1 WAIT 0:BEEP 1,50,20\n2 PRINT \"line 2 reached\"\n";

  std::string loadError;
  bool loaded = pc1500::basic::typeBasicProgramText(m->bus, m->cpu, program, kCyclesPerFrame,
                                                     kCyclesPerTimerTick, &loadError);
  CHECK(loaded);
  if (!loaded) {
    std::printf("  loadError: %s\n", loadError.c_str());
    return;
  }

  bool sawLine2 = false;
  std::set<uint16_t> visited =
      runProgramAndSamplePc(m->bus, m->cpu, m->cyclesSinceTimerTick, 600, "line 2 reached", &sawLine2);
  std::printf("  testBeep3ArgCompletesOnRun: visited %zu unique PC(s) over 600 frames; displayText=\"%s\"\n",
              visited.size(), readDisplayText(m->bus).c_str());
  CHECK(sawLine2);
  if (visited.size() < 150) {
    std::printf("  PCs visited: ");
    for (uint16_t pc : visited) std::printf("%04X ", pc);
    std::printf("\n");
  }
}

void testTrivialPrintRuns() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  std::vector<uint8_t> rom = readFile(kRomPath);
  if (rom.empty()) {
    std::printf("SKIP: testTrivialPrintRuns -- ROM1.BIN not found.\n");
    return;
  }
  auto m = bootAndSettle(rom);
  // WAIT 0 first -- see testBeepCompletesOnRun's comment: without it,
  // PRINT's default post-print pause (wait for ENTER) would hang this
  // headless test regardless of anything else.
  std::string program = "1 WAIT 0:PRINT \"HELLO\"\n2 PRINT \"line 2 reached\"\n";

  std::string loadError;
  bool loaded = pc1500::basic::typeBasicProgramText(m->bus, m->cpu, program, kCyclesPerFrame,
                                                     kCyclesPerTimerTick, &loadError);
  CHECK(loaded);
  if (!loaded) {
    std::printf("  loadError: %s\n", loadError.c_str());
    return;
  }

  std::set<uint16_t> visited =
      runProgramAndSamplePc(m->bus, m->cpu, m->cyclesSinceTimerTick, 60);
  std::printf("  testTrivialPrintRuns: visited %zu unique PC(s) over 60 frames\n", visited.size());
  if (visited.size() < 100) {
    std::printf("  PCs visited: ");
    for (uint16_t pc : visited) std::printf("%04X ", pc);
    std::printf("\n");
  }
}

void testWaitHangsOnRun() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  std::vector<uint8_t> rom = readFile(kRomPath);
  if (rom.empty()) {
    std::printf("SKIP: testWaitHangsOnRun -- ROM1.BIN not found.\n");
    return;
  }
  auto m = bootAndSettle(rom);
  std::string program = "1 WAIT 200:PRINT \"Dungeon Quest Ver1.3 by\"\n2 PRINT \"line 2 reached\"\n";

  std::string loadError;
  bool loaded = pc1500::basic::typeBasicProgramText(m->bus, m->cpu, program, kCyclesPerFrame,
                                                     kCyclesPerTimerTick, &loadError);
  CHECK(loaded);
  if (!loaded) {
    std::printf("  loadError: %s\n", loadError.c_str());
    return;
  }

  // ~10 simulated real seconds (600 frames @ 60fps) -- comfortably more than
  // the ~4 real seconds WAIT 200 should take on real hardware.
  bool sawLine2 = false;
  std::set<uint16_t> visited =
      runProgramAndSamplePc(m->bus, m->cpu, m->cyclesSinceTimerTick, 600, "line 2 reached", &sawLine2);
  std::printf("  testWaitHangsOnRun: visited %zu unique PC(s) over 600 frames; displayText=\"%s\"\n",
              visited.size(), readDisplayText(m->bus).c_str());
  CHECK(sawLine2);
  if (visited.size() < 150) {
    std::printf("  PCs visited: ");
    for (uint16_t pc : visited) std::printf("%04X ", pc);
    std::printf("\n");
  }
}

// Apples-to-apples comparison against real-hardware PEEK data the user
// gathered by hand: runs the *exact same* minimal diagnostic program
// (CLEAR then a single WAIT) and reads the same 7880H-788BH RAM range
// directly via bus.readME0, before and after the WAIT. Real hardware
// showed 7882H-7887H holding varied non-0xFF values (62,120,224,16,251,255)
// both before and after a WAIT 20 -- if the emulator shows 0xFF there
// (RAM's power-on-reset fill, i.e. never written), that's a genuine
// divergence in whatever writes that range during/before WAIT's own setup,
// not something caused by DIM/RANDOM (this program has neither).
void testWaitMemoryDump() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  std::vector<uint8_t> rom = readFile(kRomPath);
  if (rom.empty()) {
    std::printf("SKIP: testWaitMemoryDump -- ROM1.BIN not found.\n");
    return;
  }
  auto m = bootAndSettle(rom);
  std::printf("  testWaitMemoryDump: right after boot, before any typing: ");
  for (uint16_t addr = 0x7880; addr <= 0x788B; addr++) std::printf("%d ", m->bus.readME0(addr));
  std::printf("\n");
  std::string program = "1 CLEAR:WAIT 20\n";

  std::string loadError;
  bool loaded = pc1500::basic::typeBasicProgramText(m->bus, m->cpu, program, kCyclesPerFrame,
                                                     kCyclesPerTimerTick, &loadError);
  CHECK(loaded);
  if (!loaded) {
    std::printf("  loadError: %s\n", loadError.c_str());
    return;
  }

  auto dumpRange = [&](const char* label) {
    std::printf("  testWaitMemoryDump: %s: ", label);
    for (uint16_t addr = 0x7880; addr <= 0x788B; addr++) {
      std::printf("%d ", m->bus.readME0(addr));
    }
    std::printf("\n");
  };

  // "Before": as early into RUN as possible -- CLEAR:WAIT 20 is a single
  // line, so this lands right as WAIT's own setup is just starting.
  std::set<uint16_t> visitedEarly =
      runProgramAndSamplePc(m->bus, m->cpu, m->cyclesSinceTimerTick, 2);
  dumpRange("before (early)");

  // "After": 60 more frames (~1 real second) -- comfortably more than
  // WAIT 20 should need if working correctly, matching the real-hardware
  // test's single WAIT 20 checkpoint.
  for (int frame = 0; frame < 60; frame++) {
    m->bus.ioPort().advanceManualRtcClock(1.0 / 60.0); m->bus.pollRtc();
    for (int i = 0; i < kCyclesPerFrame; i++) {
      int c = m->cpu.step();
      int used = (c > 0) ? c : 1;
      m->cyclesSinceTimerTick += used;
      m->bus.advanceCycles(used);
      while (m->cyclesSinceTimerTick >= kCyclesPerTimerTick) {
        m->cpu.tickTimer();
        m->cyclesSinceTimerTick -= kCyclesPerTimerTick;
      }
    }
  }
  dumpRange("after 1s");
}

// Fine-grained write-watchpoint on 7882H (one of the confirmed-diverging
// bytes -- real hardware writes 62 there, the emulator leaves it at RAM's
// 0xFF power-on value, i.e. never written): single-steps one instruction
// at a time (not batched, unlike the rest of this file) so the exact PC
// responsible for the first write, if any, can be identified directly,
// rather than continuing to guess from static disassembly (which has
// undecoded raw-byte regions this write may fall inside).
void testFindWriterOf7882() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  std::vector<uint8_t> rom = readFile(kRomPath);
  if (rom.empty()) {
    std::printf("SKIP: testFindWriterOf7882 -- ROM1.BIN not found.\n");
    return;
  }
  auto m = bootAndSettle(rom);
  std::string program = "1 CLEAR:WAIT 20\n";

  std::string loadError;
  bool loaded = pc1500::basic::typeBasicProgramText(m->bus, m->cpu, program, kCyclesPerFrame,
                                                     kCyclesPerTimerTick, &loadError);
  CHECK(loaded);
  if (!loaded) {
    std::printf("  loadError: %s\n", loadError.c_str());
    return;
  }

  // Type "RUN" the same way runProgramAndSamplePc does (CL, MODE, RUN,
  // Enter, settle), then single-step from there.
  auto stepCycles = [&](long cycles) {
    for (long i = 0; i < cycles;) {
      int c = m->cpu.step();
      int used = (c > 0) ? c : 1;
      i += used;
      m->cyclesSinceTimerTick += used;
      m->bus.advanceCycles(used);
      while (m->cyclesSinceTimerTick >= kCyclesPerTimerTick) {
        m->cpu.tickTimer();
        m->cyclesSinceTimerTick -= kCyclesPerTimerTick;
      }
    }
  };
  auto runKeyAction = [&](pc1500::Key key, bool pressed, int framesToWait) {
    m->bus.setKeyState(key, pressed);
    stepCycles(static_cast<long>(framesToWait) * kCyclesPerFrame);
  };
  auto typeChar = [&](char c) {
    std::deque<pc1500::basic::QueuedKeyAction> actions;
    if (!pc1500::basic::charToTapActions(c, &actions)) return;
    for (const auto& a : actions) runKeyAction(a.key, a.pressed, a.framesToWait);
  };
  runKeyAction(pc1500::Key::Cl, true, pc1500::basic::kTapFrames);
  runKeyAction(pc1500::Key::Cl, false, pc1500::basic::kIdleFrames);
  runKeyAction(pc1500::Key::Mode, true, pc1500::basic::kTapFrames);
  runKeyAction(pc1500::Key::Mode, false, pc1500::basic::kIdleFrames);
  for (char c : std::string("RUN")) typeChar(c);
  runKeyAction(pc1500::Key::Ent, true, pc1500::basic::kTapFrames);
  runKeyAction(pc1500::Key::Ent, false, pc1500::basic::kIdleFrames);
  stepCycles(4L * pc1500::basic::kIdleFrames * kCyclesPerFrame);

  uint8_t before = m->bus.readME0(0x7882);
  bool found = false;
  // Budget: comfortably more instructions than a real WAIT 20 should ever
  // need (each instruction is only a handful of cycles; this is a large
  // multiple of one whole frame's worth of *instructions*, not cycles).
  for (long i = 0; i < 2'000'000 && !found; i++) {
    uint16_t pcBefore = m->cpu.p();
    int c = m->cpu.step();
    int used = (c > 0) ? c : 1;
    m->cyclesSinceTimerTick += used;
    m->bus.advanceCycles(used);
    while (m->cyclesSinceTimerTick >= kCyclesPerTimerTick) {
      m->cpu.tickTimer();
      m->cyclesSinceTimerTick -= kCyclesPerTimerTick;
    }
    uint8_t after = m->bus.readME0(0x7882);
    if (after != before) {
      std::printf("  testFindWriterOf7882: 7882H changed %d -> %d at instruction #%ld, PC before=%04X\n",
                  before, after, i, pcBefore);
      before = after;
      found = true;
    }
  }
  if (!found) {
    std::printf("  testFindWriterOf7882: 7882H never changed from %d across 2,000,000 instructions\n",
                before);
  }
}

// Watches U (UL/UH) at the exact moment PC reaches E89CH -- the TP-edge
// countdown loop found by reading the disassembly (LE89C: waits for a
// full TP rising+falling cycle, then "dec ul/bcr LE89C/dec uh/bcr LE89C").
// User confirmed on real hardware: WAIT N's delay is exactly N/64 seconds,
// linear (WAIT 64 = 1s, WAIT 3840 = 1 minute) -- matching this loop's
// design exactly, since it decrements once per full TP cycle and TP is
// independently confirmed to run at 64Hz in this emulator. If U holds the
// WAIT argument (77) here, this pins down the exact load site by
// comparing against a second run with a different argument (178) -- a
// free-running counter (like the false positive found via 7B0CH earlier)
// would show unrelated values in both runs, not the chosen argument.
// Needs an actual PRINT after WAIT to trigger the pause logic at all (see
// the user's correction: WAIT alone just sets a property; PRINT is what
// executes the pause afterward).
void testWaitCountdownRegisterValue() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  std::vector<uint8_t> rom = readFile(kRomPath);
  if (rom.empty()) {
    std::printf("SKIP: testWaitCountdownRegisterValue -- ROM1.BIN not found.\n");
    return;
  }
  for (int arg : {77, 178}) {
    auto m = bootAndSettle(rom);
    std::string program = "1 WAIT " + std::to_string(arg) + ":PRINT \"X\"\n";

    std::string loadError;
    bool loaded = pc1500::basic::typeBasicProgramText(m->bus, m->cpu, program, kCyclesPerFrame,
                                                       kCyclesPerTimerTick, &loadError);
    CHECK(loaded);
    if (!loaded) {
      std::printf("  loadError: %s\n", loadError.c_str());
      continue;
    }

    auto stepCycles = [&](long cycles) {
      for (long i = 0; i < cycles;) {
        int c = m->cpu.step();
        int used = (c > 0) ? c : 1;
        i += used;
        m->cyclesSinceTimerTick += used;
        m->bus.advanceCycles(used);
        while (m->cyclesSinceTimerTick >= kCyclesPerTimerTick) {
          m->cpu.tickTimer();
          m->cyclesSinceTimerTick -= kCyclesPerTimerTick;
        }
      }
    };
    auto runKeyAction = [&](pc1500::Key key, bool pressed, int framesToWait) {
      m->bus.setKeyState(key, pressed);
      stepCycles(static_cast<long>(framesToWait) * kCyclesPerFrame);
    };
    auto typeChar = [&](char c) {
      std::deque<pc1500::basic::QueuedKeyAction> actions;
      if (!pc1500::basic::charToTapActions(c, &actions)) return;
      for (const auto& a : actions) runKeyAction(a.key, a.pressed, a.framesToWait);
    };
    runKeyAction(pc1500::Key::Cl, true, pc1500::basic::kTapFrames);
    runKeyAction(pc1500::Key::Cl, false, pc1500::basic::kIdleFrames);
    runKeyAction(pc1500::Key::Mode, true, pc1500::basic::kTapFrames);
    runKeyAction(pc1500::Key::Mode, false, pc1500::basic::kIdleFrames);
    for (char c : std::string("RUN")) typeChar(c);
    runKeyAction(pc1500::Key::Ent, true, pc1500::basic::kTapFrames);
    runKeyAction(pc1500::Key::Ent, false, pc1500::basic::kIdleFrames);

    bool sawE89C = false;
    for (long i = 0; i < 500'000 && !sawE89C; i++) {
      uint16_t pcBefore = m->cpu.p();
      if (pcBefore == 0xE89C) {
        sawE89C = true;
        std::printf("  testWaitCountdownRegisterValue: arg=%d reached E89CH at instruction #%ld, "
                    "U=%04X (UL=%02X UH=%02X) A=%02X X=%04X Y=%04X\n",
                    arg, i, m->cpu.u(), m->cpu.u() & 0xFF, (m->cpu.u() >> 8) & 0xFF, m->cpu.a(),
                    m->cpu.x(), m->cpu.y());
      }
      int c = m->cpu.step();
      int used = (c > 0) ? c : 1;
      m->cyclesSinceTimerTick += used;
      m->bus.advanceCycles(used);
      while (m->cyclesSinceTimerTick >= kCyclesPerTimerTick) {
        m->cpu.tickTimer();
        m->cyclesSinceTimerTick -= kCyclesPerTimerTick;
      }
    }
    if (!sawE89C) {
      std::printf("  testWaitCountdownRegisterValue: arg=%d never reached E89CH in 500,000 instructions\n",
                  arg);
    }
  }
}

// The earlier big_trace.txt capture (40,000 cycles) only showed
// DISP_N_CHARS (LED00, a legitimate bounded per-character LCD font-render
// routine) still mid-render when the capture window ran out -- that trace
// was too short to distinguish "still doing legitimate one-time work" from
// "stuck forever." This test runs a much larger budget (3,000,000
// instructions -- far more than 26 characters of font rendering could ever
// need) and records the PC of the *last* 400 instructions executed, so the
// actual steady-state loop (whatever's still running after all legitimate
// one-time setup/render work should be long done) can be read directly.
void testSteadyStateLoop() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  std::vector<uint8_t> rom = readFile(kRomPath);
  if (rom.empty()) {
    std::printf("SKIP: testSteadyStateLoop -- ROM1.BIN not found.\n");
    return;
  }
  auto m = bootAndSettle(rom);
  std::string program = "1 WAIT 200:PRINT \"Dungeon Quest Ver1.3 by\"\n2 PRINT \"line 2 reached\"\n";

  std::string loadError;
  bool loaded = pc1500::basic::typeBasicProgramText(m->bus, m->cpu, program, kCyclesPerFrame,
                                                     kCyclesPerTimerTick, &loadError);
  CHECK(loaded);
  if (!loaded) {
    std::printf("  loadError: %s\n", loadError.c_str());
    return;
  }

  auto runKeyAction = [&](pc1500::Key key, bool pressed, int framesToWait) {
    m->bus.setKeyState(key, pressed);
    for (int f = 0; f < framesToWait; f++) {
      for (int i = 0; i < kCyclesPerFrame; i++) {
        int c = m->cpu.step();
        int used = (c > 0) ? c : 1;
        m->cyclesSinceTimerTick += used;
        m->bus.advanceCycles(used);
        while (m->cyclesSinceTimerTick >= kCyclesPerTimerTick) {
          m->cpu.tickTimer();
          m->cyclesSinceTimerTick -= kCyclesPerTimerTick;
        }
      }
    }
  };
  runKeyAction(pc1500::Key::Cl, true, pc1500::basic::kTapFrames);
  runKeyAction(pc1500::Key::Cl, false, pc1500::basic::kIdleFrames);
  runKeyAction(pc1500::Key::Mode, true, pc1500::basic::kTapFrames);
  runKeyAction(pc1500::Key::Mode, false, pc1500::basic::kIdleFrames);
  for (char c : std::string("RUN")) {
    std::deque<pc1500::basic::QueuedKeyAction> actions;
    if (!pc1500::basic::charToTapActions(c, &actions)) continue;
    for (const auto& a : actions) runKeyAction(a.key, a.pressed, a.framesToWait);
  }
  runKeyAction(pc1500::Key::Ent, true, pc1500::basic::kTapFrames);
  runKeyAction(pc1500::Key::Ent, false, pc1500::basic::kIdleFrames);

  // Frame-based, exactly like runProgramAndSamplePc -- advanceRealTime(1/60)
  // once per frame is what actually advances the RTC's TP level (see
  // Upd1990ac::advanceRealTime). A prior version of this test stepped the
  // CPU for 3,000,000 raw instructions with NO advanceRealTime() calls at
  // all, which freezes tpLevel_ for the whole run -- any busy-wait on the
  // live TP bit would spin forever in that harness regardless of whether
  // the real emulator (which does call advanceRealTime once per real frame)
  // has the same problem. This version runs 3000 real frames (~50 simulated
  // seconds -- WAIT 200 should take ~4s on real hardware) and builds a
  // histogram of every PC visited, so the address(es) actually dominating
  // CPU time in a *properly* time-respecting run can be identified.
  constexpr int kFrames = 3000;
  std::unordered_map<uint16_t, uint64_t> pcHistogram;
  uint64_t totalInstructions = 0;
  std::vector<uint16_t> tailPcs;
  constexpr size_t kTailLen = 60;
  for (int frame = 0; frame < kFrames; frame++) {
    m->bus.ioPort().advanceManualRtcClock(1.0 / 60.0); m->bus.pollRtc();
    for (int i = 0; i < kCyclesPerFrame; i++) {
      uint16_t pc = m->cpu.p();
      pcHistogram[pc]++;
      totalInstructions++;
      if (tailPcs.size() == kTailLen) tailPcs.erase(tailPcs.begin());
      tailPcs.push_back(pc);
      int c = m->cpu.step();
      int used = (c > 0) ? c : 1;
      m->cyclesSinceTimerTick += used;
      m->bus.advanceCycles(used);
      while (m->cyclesSinceTimerTick >= kCyclesPerTimerTick) {
        m->cpu.tickTimer();
        m->cyclesSinceTimerTick -= kCyclesPerTimerTick;
      }
    }
  }
  std::printf("  testSteadyStateLoop: after %d frames (%llu instructions), displayText=\"%s\"\n",
              kFrames, static_cast<unsigned long long>(totalInstructions),
              readDisplayText(m->bus).c_str());

  std::vector<std::pair<uint16_t, uint64_t>> sorted(pcHistogram.begin(), pcHistogram.end());
  std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
  std::printf("  testSteadyStateLoop: top PCs by visit count (of %zu unique):\n", sorted.size());
  for (size_t i = 0; i < sorted.size() && i < 25; i++) {
    std::printf("    %04X: %llu (%.1f%%)\n", sorted[i].first,
                static_cast<unsigned long long>(sorted[i].second),
                100.0 * sorted[i].second / static_cast<double>(totalInstructions));
  }
  std::printf("  testSteadyStateLoop: last %zu PCs: ", tailPcs.size());
  for (uint16_t pc : tailPcs) std::printf("%04X ", pc);
  std::printf("\n");
}

// Focused, register/bit-level trace of WAIT's own polling state machine
// (LE89C-LE8BC, per _bisect/rom1.asm) -- logs every visit to the key
// decision points (E89C, E8A1 [right after the first bii], E8A9, E8AE,
// E8B0, E8B2, E8BC) along with the live OPB bit 0x20 (RTC TP level) and
// IF bit 0x02 (TP edge latch / shared BREAK flag) values, in a properly
// frame-paced (advanceRealTime-respecting) run, to see exactly which
// branch gets taken on each pass and why the E8BC decrement (the only
// path that actually counts WAIT's argument down) is never reached.
void testWaitPollTrace() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  std::vector<uint8_t> rom = readFile(kRomPath);
  if (rom.empty()) {
    std::printf("SKIP: testWaitPollTrace -- ROM1.BIN not found.\n");
    return;
  }
  auto m = bootAndSettle(rom);
  std::string program = "1 WAIT 32:PRINT \"HELLO\"\n2 PRINT \"line 2 reached\"\n";

  std::string loadError;
  bool loaded = pc1500::basic::typeBasicProgramText(m->bus, m->cpu, program, kCyclesPerFrame,
                                                     kCyclesPerTimerTick, &loadError);
  CHECK(loaded);
  if (!loaded) {
    std::printf("  loadError: %s\n", loadError.c_str());
    return;
  }

  // Exact-ground-truth mode: once PC first reaches E886 (WAIT's own dispatch
  // entry, right after its argument's been evaluated into U), trace every
  // single instruction (PC, opcode byte, A/U/X) for a fixed window, so each
  // bzr/bzs/bii decision can be read unambiguously by watching consecutive
  // PCs against the disassembly. This has to wrap the *entire* run
  // (including the CL/MODE/RUN/Enter keypress-driving frames, each of which
  // does its own real-time advancement, same as the live 60fps loop) --
  // an earlier version of this test only started tracing after the Enter
  // keypress's own settle frames, which turned out to already be *after*
  // E886 had come and gone (0 events logged), proving the entire WAIT
  // dispatch+poll completes within Enter's own idle-frame budget, i.e. in
  // a fraction of a second, not anywhere near the ~3.1s WAIT 200 should take.
  bool armed = false;
  int logged = 0;
  constexpr int kMaxLog = 4000;
  int frameCounter = -1;
  std::set<uint16_t> seenThisFrame;
  // Full unconditional trace from E6E0 (WAIT's own return target, per the
  // prior post-E8BB-rtn finding) through to E243 (KEYSCAN_WAIT's entry) --
  // every single instruction, no dedup -- to see exactly which branches
  // get taken instead of guessing from static disassembly.
  uint64_t seq = 0;
  int dispCallCount = 0;
  bool postE89cArmed = false;
  int postE89cCount = 0;
  uint16_t lastU = 0xFFFF;
  // Tracks the *actual* branch outcome of each of these three conditional
  // branches by landing-PC, instead of a separate diagnostic register peek
  // (which can disagree with what the branch itself saw, since the
  // continuously-advancing fine-grained clock can shift state between the
  // branch's own execution and any later peek -- confirmed to be
  // misleading earlier in this investigation).
  uint16_t traceAfterPc = 0;
  uint16_t traceAfterTakenTarget = 0;
  int branchTraceCount = 0;
  constexpr int kMaxBranchTrace = 2000;
  auto tracedStep = [&]() {
    uint16_t pc = m->cpu.p();
    if (!armed && pc == 0xE886) armed = true;
    if (armed) {
      seq++;
      if (traceAfterPc != 0) {
        std::printf("  seq=%llu post-%04X landed at PC=%04X (branch %s)\n",
                    static_cast<unsigned long long>(seq), traceAfterPc, pc,
                    pc == traceAfterTakenTarget ? "TAKEN" : "NOT taken");
        traceAfterPc = 0;
      }
      if (pc == 0xE8AE && branchTraceCount < kMaxBranchTrace) {
        branchTraceCount++;
        traceAfterPc = 0xE8AE;
        traceAfterTakenTarget = 0xE8BC;  // bzr LE8BC
      } else if (pc == 0xE8B2 && branchTraceCount < kMaxBranchTrace) {
        branchTraceCount++;
        traceAfterPc = 0xE8B2;
        traceAfterTakenTarget = 0xE8A9;  // bzs LE8A9
      } else if (pc == 0xE8BD) {
        traceAfterPc = 0xE8BD;
        traceAfterTakenTarget = 0xE89C;  // bcs LE89C
      }
      if (postE89cArmed) {
        uint16_t curU = m->cpu.u();
        if (curU != lastU) {
          std::printf("  seq=%llu U changed %04X -> %04X at PC=%04X\n",
                      static_cast<unsigned long long>(seq), lastU, curU, pc);
          lastU = curU;
        }
      }
      if (pc == 0xE886 || pc == 0xE6CC || pc == 0xE6D2 || pc == 0xE6E0 || pc == 0xE88C ||
          pc == 0xE89C || pc == 0xE8A9 || pc == 0xE8B4 || pc == 0xE8BB || pc == 0xE243) {
        std::printf("  seq=%llu PC=%04X A=%02X U=%04X 7871H=%02X OPB.20=%d IF.02=%d\n",
                    static_cast<unsigned long long>(seq), pc, m->cpu.a(), m->cpu.u(),
                    m->bus.readME0(0x7871), (m->bus.ioPort().read(0x0F) & 0x20) ? 1 : 0,
                    (m->bus.ioPort().read(0x0B) & 0x02) ? 1 : 0);
        if (pc == 0xE89C) postE89cArmed = true;
      }
      if (postE89cArmed && postE89cCount < 300) {
        std::printf("    +%d PC=%04X A=%02X U=%04X\n", postE89cCount, pc, m->cpu.a(), m->cpu.u());
        postE89cCount++;
      }
      if (pc == 0xED00 && dispCallCount < 5) {
        dispCallCount++;
        uint16_t start = m->cpu.u();
        uint8_t len = m->cpu.a();
        std::printf("  seq=%llu DISP_N_CHARS: U=%04X A(len)=%d bytes:",
                    static_cast<unsigned long long>(seq), start, len);
        std::string ascii;
        for (int i = 0; i < len && i < 40; i++) {
          uint8_t b = m->bus.readME0(static_cast<uint16_t>(start + i));
          std::printf(" %02X", b);
          ascii += (b >= 0x20 && b < 0x7F) ? static_cast<char>(b) : '.';
        }
        std::printf("  ascii=\"%s\"\n", ascii.c_str());
      }
      if (logged < kMaxLog) logged++;
    }
    int c = m->cpu.step();
    int used = (c > 0) ? c : 1;
    // Fine-grained RTC advance -- one call per *instruction*, scaled by its
    // own cycle count, rather than one coarse once-per-frame jump. A
    // once-per-frame (1/60s) advance makes the RTC's state change all at
    // once at frame boundaries, which turned out to substantially inflate
    // the odds of two adjacent-but-separate register reads (OPB's live
    // level at E8A9, IF's latched edge at E8B0, a few CPU cycles apart --
    // see _bisect/rom1.asm) straddling a tick and seeing inconsistent
    // state, compared to a smoothly-advancing clock (real wall-clock time
    // in production, or this) where that same race is proportional to how
    // close together the two reads are relative to the RTC's own period,
    // not to the test's own frame granularity.
    m->bus.ioPort().advanceManualRtcClock(static_cast<double>(used) / kCyclesPerSecond);
    m->cyclesSinceTimerTick += used;
    m->bus.advanceCycles(used);
    while (m->cyclesSinceTimerTick >= kCyclesPerTimerTick) {
      m->cpu.tickTimer();
      m->cyclesSinceTimerTick -= kCyclesPerTimerTick;
    }
  };
  auto runKeyAction = [&](pc1500::Key key, bool pressed, int framesToWait) {
    m->bus.setKeyState(key, pressed);
    for (int f = 0; f < framesToWait; f++) {
      frameCounter++;
      seenThisFrame.clear();
      for (int i = 0; i < kCyclesPerFrame; i++) tracedStep();
    }
  };
  runKeyAction(pc1500::Key::Cl, true, pc1500::basic::kTapFrames);
  runKeyAction(pc1500::Key::Cl, false, pc1500::basic::kIdleFrames);
  runKeyAction(pc1500::Key::Mode, true, pc1500::basic::kTapFrames);
  runKeyAction(pc1500::Key::Mode, false, pc1500::basic::kIdleFrames);
  for (char c : std::string("RUN")) {
    std::deque<pc1500::basic::QueuedKeyAction> actions;
    if (!pc1500::basic::charToTapActions(c, &actions)) continue;
    for (const auto& a : actions) runKeyAction(a.key, a.pressed, a.framesToWait);
  }
  runKeyAction(pc1500::Key::Ent, true, pc1500::basic::kTapFrames);
  runKeyAction(pc1500::Key::Ent, false, pc1500::basic::kIdleFrames);

  constexpr int kFrames = 900;  // ~15 simulated seconds -- WAIT 200 should need ~3.1s
  for (int frame = 0; frame < kFrames; frame++) {
    if (postE89cArmed && (frame % 5 == 0)) {
      std::printf("  frame=%d tpLevel=%d tpConfigured=%d U=%04X\n", frame,
                  m->bus.ioPort().rtcDebugTpLevel(), m->bus.ioPort().rtcDebugTpConfigured(),
                  m->cpu.u());
    }
    frameCounter++;
    seenThisFrame.clear();
    for (int i = 0; i < kCyclesPerFrame; i++) {
      tracedStep();
    }
  }
  std::printf("  testWaitPollTrace: logged %d events, displayText=\"%s\"\n", logged,
              readDisplayText(m->bus).c_str());
}

// Precisely measures how much simulated real time elapses between RUN
// starting and a target PRINT statement's text appearing in memory, for a
// range of WAIT durations -- confirms the fix (IoPortController::read()'s
// IF-bit-1 read-consumes-the-latch behavior) doesn't just make WAIT n
// *complete* but complete in the *correct* amount of time, not too fast.
// User's own real-hardware measurement: WAIT 64 = 1 real second, linear
// (WAIT n = n/64 seconds).
void testWaitTimingIsAccurate() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  std::vector<uint8_t> rom = readFile(kRomPath);
  if (rom.empty()) {
    std::printf("SKIP: testWaitTimingIsAccurate -- ROM1.BIN not found.\n");
    return;
  }
  for (int waitArg : {32, 64, 200}) {
    auto m = bootAndSettle(rom);
    std::string program =
        "1 WAIT " + std::to_string(waitArg) + ":PRINT \"X\"\n2 PRINT \"DONE\"\n";

    std::string loadError;
    bool loaded = pc1500::basic::typeBasicProgramText(m->bus, m->cpu, program, kCyclesPerFrame,
                                                       kCyclesPerTimerTick, &loadError);
    CHECK(loaded);
    if (!loaded) {
      std::printf("  loadError: %s\n", loadError.c_str());
      continue;
    }
    // Rigorous, execution-only signal: hooks DISP_N_CHARS (LE00H)'s own
    // call site and checks the bytes it's about to render, rather than
    // scanning memory for the substring anywhere. Scanning memory is a
    // false-positive trap confirmed live: "DONE" (this program's own
    // line-2 PRINT argument) is present in RAM the instant the program is
    // typed in -- long before, or entirely without, that PRINT ever
    // actually executing -- so a plain memory scan can't tell "stored
    // source text" apart from "actually rendered."
    bool found = false;
    double foundAtSeconds = -1.0;
    double simulatedSeconds = 0.0;
    // WAIT's own dispatch (LE86AH) -- and the text rendering for whatever
    // PRINT statement follows it on the same line -- starts executing
    // *during* the Enter keystroke's own settle-frame budget below, not
    // strictly after it (confirmed live: DISP_N_CHARS for line 1's own
    // PRINT fires well before the WAIT poll loop itself even starts). So
    // the timing baseline has to be captured at this precise, execution-
    // based marker, not at an arbitrary point after the keystroke sequence
    // finishes (which showed up as a bogus *negative* elapsed time --
    // "found" before the naive baseline was even captured).
    bool waitStarted = false;
    double waitStartSeconds = -1.0;
    int e8bcCount = 0;
    int e8b4Count = 0;
    int e2aaCount = 0;
    int e269Count = 0;
    int c4c6Count = 0;
    auto tracedStep = [&]() {
      if (!waitStarted && m->cpu.p() == 0xE86A) {
        waitStarted = true;
        waitStartSeconds = simulatedSeconds;
      }
      if (waitStarted && !found) {
        if (m->cpu.p() == 0xE8BC) e8bcCount++;
        if (m->cpu.p() == 0xE8B4) e8b4Count++;
        if (m->cpu.p() == 0xE2AA) e2aaCount++;
        if (m->cpu.p() == 0xE269) e269Count++;
        if (m->cpu.p() == 0xC4C6) c4c6Count++;
      }
      if (m->cpu.p() == 0xED00) {
        uint16_t start = m->cpu.u();
        uint8_t len = m->cpu.a();
        std::string text;
        for (int i = 0; i < len && i < 40; i++) {
          uint8_t b = m->bus.readME0(static_cast<uint16_t>(start + i));
          text += (b >= 0x20 && b < 0x7F) ? static_cast<char>(b) : '\x01';
        }
        if (!found && text.find("DONE") != std::string::npos) {
          found = true;
          foundAtSeconds = simulatedSeconds;
        }
      }
      int c = m->cpu.step();
      int used = (c > 0) ? c : 1;
      double dt = static_cast<double>(used) / kCyclesPerSecond;
      simulatedSeconds += dt;
      m->bus.ioPort().advanceManualRtcClock(dt);
      m->cyclesSinceTimerTick += used;
      m->bus.advanceCycles(used);
      while (m->cyclesSinceTimerTick >= kCyclesPerTimerTick) {
        m->cpu.tickTimer();
        m->cyclesSinceTimerTick -= kCyclesPerTimerTick;
      }
    };
    auto runKeyAction = [&](pc1500::Key key, bool pressed, int framesToWait) {
      m->bus.setKeyState(key, pressed);
      for (int f = 0; f < framesToWait; f++)
        for (int i = 0; i < kCyclesPerFrame; i++) tracedStep();
    };
    runKeyAction(pc1500::Key::Cl, true, pc1500::basic::kTapFrames);
    runKeyAction(pc1500::Key::Cl, false, pc1500::basic::kIdleFrames);
    runKeyAction(pc1500::Key::Mode, true, pc1500::basic::kTapFrames);
    runKeyAction(pc1500::Key::Mode, false, pc1500::basic::kIdleFrames);
    for (char c : std::string("RUN")) {
      std::deque<pc1500::basic::QueuedKeyAction> actions;
      if (!pc1500::basic::charToTapActions(c, &actions)) continue;
      for (const auto& a : actions) runKeyAction(a.key, a.pressed, a.framesToWait);
    }
    runKeyAction(pc1500::Key::Ent, true, pc1500::basic::kTapFrames);
    runKeyAction(pc1500::Key::Ent, false, pc1500::basic::kIdleFrames);

    constexpr long kMaxInstructions = 20'000'000;
    for (long i = 0; i < kMaxInstructions && !found; i++) tracedStep();
    double expectedSeconds = static_cast<double>(waitArg) / 64.0;
    double elapsedSeconds = foundAtSeconds - waitStartSeconds;
    std::printf(
        "  testWaitTimingIsAccurate: WAIT %d -- expected ~%.4fs, measured %.4fs (found=%d) "
        "e8bcCount=%d e8b4Count=%d e2aaCount=%d e269Count=%d c4c6Count=%d\n",
        waitArg, expectedSeconds, elapsedSeconds, found, e8bcCount, e8b4Count, e2aaCount, e269Count,
        c4c6Count);
    CHECK(waitStarted);
    CHECK(found);
    if (waitStarted && found) {
      // Generous tolerance (+/-30%) -- this is checking for a gross timing
      // bug (e.g. off by a factor of 2), not calibrating exact real-hardware
      // accuracy.
      CHECK(elapsedSeconds > expectedSeconds * 0.7);
      CHECK(elapsedSeconds < expectedSeconds * 1.3);
    }
  }
}

// Regression test for a real bug: the MI interrupt handler (LE171 in
// _bisect/rom1.asm) reads IF (F00BH) to test a completely unrelated bit
// (bit 0, LE17E's "bii #(0xF00B),0x01") as part of its own dispatch logic
// -- and an earlier version of IoPortController::read()'s IF case cleared
// kTpFlagBit (bit 1, BREAK's own flag) unconditionally on *any* read of
// the register, regardless of which bit the caller's own bii instruction
// actually cared about. That meant every BREAK-triggered MI dispatch
// silently ate BREAK's own flag as a side effect, before the interpreter's
// statement-boundary break-check (LC42A) ever got a chance to see it --
// confirmed live: a BREAK held for over a real second had zero effect on
// a running FOR/NEXT loop. Covers both a poll loop that reads IF itself
// (WAIT, which has its own -- narrower -- history of IF-related bugs) and
// plain interpreter code that doesn't touch IF at all until the next
// statement boundary (a FOR/NEXT loop), since the fix needed to hold for
// both without reverting WAIT's own timing fix.
void testBreakStopsRunningProgram() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  std::vector<uint8_t> rom = readFile(kRomPath);
  if (rom.empty()) {
    std::printf("SKIP: testBreakStopsRunningProgram -- ROM1.BIN not found.\n");
    return;
  }
  constexpr uint16_t kIdleAddr = 0xE2AA;
  constexpr uint16_t kWaitPollAddr = 0xE8A9;  // inside WAIT's own poll loop (LE89C-LE8BC)

  // triggerBreakAt: runs the program, presses+releases BREAK (matching a
  // quick real F12 tap -- see IoPortController::setOnKeyLine()'s own
  // comment) at the first instant `stopCondition` is true, then keeps
  // stepping and returns whether the CPU reached the idle prompt address
  // within a generous bounded window.
  auto testCase = [&](const char* label, const std::string& program,
                       const std::function<bool(BootedMachine&)>& stopCondition) {
    auto m = bootAndSettle(rom);
    std::string loadError;
    bool loaded = pc1500::basic::typeBasicProgramText(m->bus, m->cpu, program, kCyclesPerFrame,
                                                       kCyclesPerTimerTick, &loadError);
    CHECK(loaded);
    if (!loaded) {
      std::printf("  %s: loadError: %s\n", label, loadError.c_str());
      return;
    }
    auto tracedStep = [&]() {
      int c = m->cpu.step();
      int used = (c > 0) ? c : 1;
      m->bus.ioPort().advanceManualRtcClock(static_cast<double>(used) / kCyclesPerSecond);
      m->cyclesSinceTimerTick += used;
      m->bus.advanceCycles(used);
      while (m->cyclesSinceTimerTick >= kCyclesPerTimerTick) {
        m->cpu.tickTimer();
        m->cyclesSinceTimerTick -= kCyclesPerTimerTick;
      }
    };
    auto runKeyAction = [&](pc1500::Key key, bool pressed, int framesToWait) {
      m->bus.setKeyState(key, pressed);
      for (int f = 0; f < framesToWait; f++)
        for (int i = 0; i < kCyclesPerFrame; i++) tracedStep();
    };
    runKeyAction(pc1500::Key::Mode, true, pc1500::basic::kTapFrames);
    runKeyAction(pc1500::Key::Mode, false, pc1500::basic::kIdleFrames);
    for (char c : std::string("RUN")) {
      std::deque<pc1500::basic::QueuedKeyAction> actions;
      if (!pc1500::basic::charToTapActions(c, &actions)) continue;
      for (const auto& a : actions) runKeyAction(a.key, a.pressed, a.framesToWait);
    }
    runKeyAction(pc1500::Key::Ent, true, pc1500::basic::kTapFrames);
    runKeyAction(pc1500::Key::Ent, false, pc1500::basic::kIdleFrames);

    constexpr long kMaxInstructionsToTrigger = 20'000'000;
    bool reachedCondition = false;
    for (long i = 0; i < kMaxInstructionsToTrigger; i++) {
      if (stopCondition(*m)) {
        reachedCondition = true;
        break;
      }
      tracedStep();
    }
    CHECK(reachedCondition);
    if (!reachedCondition) {
      std::printf("  %s: never reached the BREAK trigger condition\n", label);
      return;
    }

    // A plain, fire-and-forget press+release -- same as tools/send-command.ps1's
    // "break" FIFO command with no cycle count, which is the realistic case
    // (even a fast real F12 tap holds far longer than a single instruction).
    m->cpu.pressOnKey();
    m->bus.ioPort().setOnKeyLine(true);
    m->cpu.requestMI();
    m->bus.ioPort().setOnKeyLine(false);

    constexpr long kMaxInstructionsAfterBreak = 5'000'000;
    bool reachedIdle = false;
    for (long i = 0; i < kMaxInstructionsAfterBreak; i++) {
      if (m->cpu.p() == kIdleAddr && m->cpu.halted()) {
        reachedIdle = true;
        break;
      }
      tracedStep();
    }
    std::printf("  %s: reachedIdle=%d final P=%04X halted=%d\n", label, reachedIdle, m->cpu.p(),
                m->cpu.halted());
    CHECK(reachedIdle);
  };

  testCase(
      "BREAK during WAIT's own poll loop", "1 WAIT 2000:PRINT \"DONE\"\n",
      [](BootedMachine& m) { return m.cpu.p() == kWaitPollAddr; });

  // Matches the user's own real-world reproduction: WAIT 0 (negligible
  // delay, but still configures then disables TP -- see
  // Upd1990ac::latchCommand()'s Group 0 reset) followed by a FOR/NEXT loop
  // that never touches IF/OPB itself at all, so BREAK can only ever be
  // caught by the interpreter's own statement-boundary check, not by any
  // poll loop's own IF read.
  testCase("BREAK during FOR/NEXT loop after a prior WAIT",
           "1 WAIT 0\n2 FOR I=1 TO 5000\n3 PRINT \"L\"+STR$(I)\n4 NEXT I\n",
           [](BootedMachine& m) { return m.cpu.p() == 0xC42A; });
}

void testFindWaitArgumentStorage() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  std::vector<uint8_t> rom = readFile(kRomPath);
  if (rom.empty()) {
    std::printf("SKIP: testFindWaitArgumentStorage -- ROM1.BIN not found.\n");
    return;
  }
  auto m = bootAndSettle(rom);
  std::string program = "1 WAIT 77\n";

  std::string loadError;
  bool loaded = pc1500::basic::typeBasicProgramText(m->bus, m->cpu, program, kCyclesPerFrame,
                                                     kCyclesPerTimerTick, &loadError);
  CHECK(loaded);
  if (!loaded) {
    std::printf("  loadError: %s\n", loadError.c_str());
    return;
  }

  auto stepCycles = [&](long cycles) {
    for (long i = 0; i < cycles;) {
      int c = m->cpu.step();
      int used = (c > 0) ? c : 1;
      i += used;
      m->cyclesSinceTimerTick += used;
      m->bus.advanceCycles(used);
      while (m->cyclesSinceTimerTick >= kCyclesPerTimerTick) {
        m->cpu.tickTimer();
        m->cyclesSinceTimerTick -= kCyclesPerTimerTick;
      }
    }
  };
  auto runKeyAction = [&](pc1500::Key key, bool pressed, int framesToWait) {
    m->bus.setKeyState(key, pressed);
    stepCycles(static_cast<long>(framesToWait) * kCyclesPerFrame);
  };
  auto typeChar = [&](char c) {
    std::deque<pc1500::basic::QueuedKeyAction> actions;
    if (!pc1500::basic::charToTapActions(c, &actions)) return;
    for (const auto& a : actions) runKeyAction(a.key, a.pressed, a.framesToWait);
  };
  runKeyAction(pc1500::Key::Cl, true, pc1500::basic::kTapFrames);
  runKeyAction(pc1500::Key::Cl, false, pc1500::basic::kIdleFrames);
  runKeyAction(pc1500::Key::Mode, true, pc1500::basic::kTapFrames);
  runKeyAction(pc1500::Key::Mode, false, pc1500::basic::kIdleFrames);
  for (char c : std::string("RUN")) typeChar(c);
  runKeyAction(pc1500::Key::Ent, true, pc1500::basic::kTapFrames);
  runKeyAction(pc1500::Key::Ent, false, pc1500::basic::kIdleFrames);

  std::vector<uint8_t> before(0x400);  // 7800H-7BFFH
  for (uint16_t a = 0; a < 0x400; a++) before[a] = m->bus.readME0(static_cast<uint16_t>(0x7800 + a));

  bool foundAny = false;
  bool sawWaitEntry = false;
  for (long i = 0; i < 60000; i++) {
    uint16_t pcBefore = m->cpu.p();
    if (pcBefore == 0xE86A && !sawWaitEntry) {
      sawWaitEntry = true;
      std::printf("  testFindWaitArgumentStorage: reached WAIT entry E86AH at instruction #%ld\n", i);
    }
    int c = m->cpu.step();
    int used = (c > 0) ? c : 1;
    m->cyclesSinceTimerTick += used;
    m->bus.advanceCycles(used);
    while (m->cyclesSinceTimerTick >= kCyclesPerTimerTick) {
      m->cpu.tickTimer();
      m->cyclesSinceTimerTick -= kCyclesPerTimerTick;
    }
    if (pcBefore == 0xE86A) {
      // Dump ARITH_X (7A00H-7A07H) and the CPU's own registers right as
      // WAIT's argument-evaluation (vej 0xC8) is about to run, then again
      // 30 instructions later once it should have completed.
      auto dumpArithX = [&](const char* label) {
        std::printf("  testFindWaitArgumentStorage: %s ARITH_X=", label);
        for (uint16_t a = 0x7A00; a <= 0x7A07; a++) std::printf("%02X ", m->bus.readME0(a));
        std::printf(" A=%02X X=%04X Y=%04X U=%04X\n", m->cpu.a(), m->cpu.x(), m->cpu.y(), m->cpu.u());
      };
      dumpArithX("at E86A entry");
      for (int j = 0; j < 30; j++) {
        int cc = m->cpu.step();
        int uu = (cc > 0) ? cc : 1;
        m->cyclesSinceTimerTick += uu;
        m->bus.advanceCycles(uu);
        while (m->cyclesSinceTimerTick >= kCyclesPerTimerTick) {
          m->cpu.tickTimer();
          m->cyclesSinceTimerTick -= kCyclesPerTimerTick;
        }
      }
      dumpArithX("30 instructions later");
    }
    for (uint16_t a = 0; a < 0x400; a++) {
      if (0x7800 + a == 0x7B0C) continue;  // known free-running tick counter, not WAIT-argument-related
      uint8_t v = m->bus.readME0(static_cast<uint16_t>(0x7800 + a));
      if (v != before[a]) {
        if (v == 77 || v == 0x77) {
          std::printf("  testFindWaitArgumentStorage: %04XH %d -> %d (0x%02X) at instruction #%ld, PC before=%04X\n",
                      0x7800 + a, before[a], v, v, i, pcBefore);
          foundAny = true;
        }
        before[a] = v;
      }
    }
  }
  if (!foundAny) {
    std::printf("  testFindWaitArgumentStorage: value 77 never appeared (excl. 7B0CH) in 60000 instructions\n");
  }
  std::printf("  testFindWaitArgumentStorage: sawWaitEntry=%d final PC=%04X displayText=\"%s\"\n",
              sawWaitEntry, m->cpu.p(), readDisplayText(m->bus).c_str());
}

}  // namespace

int main() {
  testHexload1500RoundTrip();
  testLongLineMultiPass();
  testLongLineColonInsideQuotes();
  testLongLineUnsplittableFails();
  testBlackjackRoundTrip();
  testDungeonQuestRoundTrip();
  testTrivialPrintRuns();
  testBeepCompletesOnRun();
  testBeep3ArgCompletesOnRun();
  testWaitHangsOnRun();
  testWaitMemoryDump();
  testFindWriterOf7882();
  testWaitCountdownRegisterValue();
  testFindWaitArgumentStorage();
  testSteadyStateLoop();
  testWaitPollTrace();
  testWaitTimingIsAccurate();
  testBreakStopsRunningProgram();

  if (g_failures == 0) {
    std::printf("All tests passed.\n");
    return 0;
  }
  std::printf("%d check(s) failed.\n", g_failures);
  return 1;
}
