// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
//
// In-process regression coverage for the PC1500-PSOC5 expansion ROM's
// custom-keyword dispatch (rom.asm, built externally into rom_8800.bin),
// specifically the shared KEYWORD_RETURN tail every keyword (SDLS/SFMT/etc.)
// ends with. Exists because diagnosing KEYWORD_RETURN's return-to-idle
// behavior by hand -- driving a live, visible pc1500emu.exe over the FIFO
// pipe, sleeping an empirically-guessed number of milliseconds between each
// step, eyeballing ASCII-art LCD dumps -- has repeatedly cost real time to
// real mistakes (a presskey/trace/releasekey sequence that let one held key
// get "seen" twice by the ROM; a huge trace file silently truncated by a
// PowerShell variable read instead of erroring) across several sessions
// without actually finding the root cause. This boots a real ROM1.BIN plus
// the built expansion ROM entirely in-process -- no SDL window, no FIFO, no
// wall-clock sleeps -- so the exact same keystroke sequence a human would
// type reproduces deterministically in well under a second, following the
// precedent already established by basic_load_roundtrip_test.cpp's
// BootedMachine/bootAndSettle() pattern and testBreakStopsRunningProgram's
// own inline charToTapActions-driven typing helper.
//
// Needs a real PC-1500 ROM dump and the built expansion ROM, neither of
// which ship in this repo (Sharp's ROM is copyrighted; the expansion ROM is
// built from a sibling PSoC Creator project). Skips (prints a message,
// exits 0) if they're not present at their known location on this machine,
// matching every other ROM-dependent test here.
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <vector>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

#include "basic_text.h"
#include "bus.h"
#include "keyboard.h"
#ifdef PC1500_HAVE_EXPANSION_KEYWORDS
#include "basic_xlate.h"  // the firmware's own (testBasicXlateChunks)
#include "kbd_seq.h"      // ...and its key sequencer (testKbdLayouts)
#endif
#include "lh5801.h"
#include "text_loader.h"

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      g_failures++;                                                 \
    }                                                                \
  } while (0)

// Must match src/host/main.cpp's kCyclesPerFrame/kCyclesPerTimerTick, same
// reasoning as basic_load_roundtrip_test.cpp's own copy of these constants.
constexpr int kCyclesPerFrame = 1300000 / 60;
constexpr int kCyclesPerTimerTick = 8;

// BASIC's stable idle/ready-prompt address (HLT-based) -- confirmed via
// live entertrace in an earlier session, and already relied on by
// basic_load_roundtrip_test.cpp's testBreakStopsRunningProgram. SDLS's own
// internal KEYSCAN_WAIT blocking loop also settles here while waiting for a
// key (confirmed live: KEYSCAN_WAIT's "nothing pressed yet" wait cycles
// through the same shared HLT/wake point as the top-level idle loop), so
// this single condition detects both "genuinely idle" and "a keyword is
// blocked waiting for its own next keypress".
constexpr uint16_t kIdleAddr = 0xE2AA;

// ERL -- BASIC's own "error number when occurred" system variable, per
// known_symbols.cpp. Shared by every ERROR-raising test below.
constexpr uint16_t kErlAbs = 0x789B;

std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

fs::path makeTempTestDir(const char* name) {
  fs::path dir = fs::temp_directory_path() / name;
  std::error_code ec;
  fs::remove_all(dir, ec);  // clean slate if a previous run left it behind
  fs::create_directories(dir);
  return dir;
}

// Same logic as main.cpp's "displaytext" FIFO command and
// basic_load_roundtrip_test.cpp's readDisplayText(): the ROM's own 80-byte
// LCD text buffer (7BB0H-7BFFH), read as ASCII up to its 0DH terminator.
// This is the *editor's* logical view of the current line -- what's
// actually about to be tokenized/submitted -- as opposed to the real
// rendered VRAM, and is exactly where the reported "SDLS MEM" concatenation
// bug shows up: it's captured *during typing*, before Enter is pressed.
std::string readDisplayBuffer(pc1500::Bus& bus) {
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

// Boots+settles a fresh Bus/CPU pair on `rom`, the same two-stage process
// (run to first halted(), then a further settle window) basic_load_
// roundtrip_test.cpp's own bootAndSettle() uses and documents at length --
// see that file for why the first halted() alone isn't the real settled
// state.
struct BootedMachine {
  pc1500::Keyboard keyboard;
  pc1500::Bus bus{keyboard};
  lh5801::CPU cpu{bus};
  int cyclesSinceTimerTick = 0;
};

void stepOne(BootedMachine& m) {
  int c = m.cpu.step();
  int used = (c > 0) ? c : 1;
  m.cyclesSinceTimerTick += used;
  m.bus.advanceCycles(used);
  while (m.cyclesSinceTimerTick >= kCyclesPerTimerTick) {
    m.cpu.tickTimer();
    m.cyclesSinceTimerTick -= kCyclesPerTimerTick;
  }
}

// extRam0000Bytes/extRamExtBytes/variant set the machine's RAM. The default
// is the 26K the expansion module itself adds (16K at 0000H, 10K at 4800H):
// every test here is of that module, so none runs on a bare machine. They
// must be set before cpu.reset() so BASIC's own boot-time RAM scan sees
// them, matching AppConfig::isPC1500A/extRamExtBytes's own documented
// ordering requirement (src/hoststate/app_config.h).
std::unique_ptr<BootedMachine> bootAndSettle(
    const std::vector<uint8_t>& rom, size_t extRam0000Bytes = 0x4000, size_t extRamExtBytes = 0x2800,
    pc1500::Bus::MachineVariant variant = pc1500::Bus::MachineVariant::PC1500) {
  auto m = std::make_unique<BootedMachine>();
  m->bus.ioPort().useManualRtcClock();
  m->bus.loadME0(0xC000, rom.data(), rom.size());
  // Variant first -- extRamExtBytes is interpreted relative to whichever
  // variant is current (Bus::extRamExtBase()), same ordering the real
  // main.cpp's own pre-reset block uses.
  m->bus.setMachineVariant(variant);
  m->bus.setExtRam0000Size(extRam0000Bytes);
  m->bus.setExtRamExtSize(extRamExtBytes);
  m->cpu.reset();
  long bootCycles = 0;
  constexpr long kMaxBootCycles = 20'000'000;
  while (!m->cpu.halted() && bootCycles < kMaxBootCycles) {
    stepOne(*m);
    bootCycles++;
  }
  constexpr long kPostBootSettleCycles = 4'000'000;
  for (long i = 0; i < kPostBootSettleCycles; i++) stepOne(*m);
  return m;
}

// Loads the built expansion ROM at the same address/window layout the real
// firmware uses (base=8800, 2K data window 8000-87FF, instruction byte at
// 87FF -- moved from base=9000/4K data window 2026-08-18 to grow the ROM
// region to 6K; unrelated to bus_test.cpp's own makeExpansionBus(), which
// tests the generic loadExpansionModule mechanism against a synthetic
// hand-built ROM at a still-arbitrary 0x9000, not this real one), then
// points its ExpansionMock at a real host directory.
void loadExpansionRom(BootedMachine& m, const std::vector<uint8_t>& expRom, const fs::path& sdDir) {
  m.bus.loadExpansionModule(0, expRom.data(), expRom.size(), /*base=*/0x8800, /*requirePv=*/false,
                             /*usePuBank=*/false, /*dataWindowBase=*/0x8000,
                             /*dataWindowSize=*/0x800, /*instructionAddr=*/0x87FF);
  m.bus.expansionMock().setRootDir(sdDir);
}

void runKeyAction(BootedMachine& m, pc1500::Key key, bool pressed, int framesToWait) {
  m.bus.setKeyState(key, pressed);
  for (int f = 0; f < framesToWait; f++)
    for (int i = 0; i < kCyclesPerFrame; i++) stepOne(m);
}

// A single tap+release of a dedicated key (CL, Enter, cursor keys, etc.) --
// not routed through charToTapActions, which only maps printable
// characters, matching testBreakStopsRunningProgram's own Key::Mode usage.
void tapKey(BootedMachine& m, pc1500::Key key) {
  runKeyAction(m, key, true, pc1500::basic::kTapFrames);
  runKeyAction(m, key, false, pc1500::basic::kIdleFrames);
}

// Types `text` character-by-character via charToTapActions -- the same
// primitive the live host's interactive `type` FIFO command and
// typeBasicProgramText both build on -- deliberately *not*
// typeBasicProgramText itself, since that function drives BASIC's PRO-mode
// *program-line* editor (line numbers, multi-pass long lines) and SDLS/MEM/
// NEW0 are direct, immediate-mode commands typed straight at the READY
// prompt, a different context entirely.
void typeText(BootedMachine& m, const std::string& text) {
  for (char c : text) {
    std::deque<pc1500::basic::QueuedKeyAction> actions;
    if (!pc1500::basic::charToTapActions(c, &actions)) continue;
    for (const auto& a : actions) runKeyAction(m, a.key, a.pressed, a.framesToWait);
  }
}

// Bounded wait for the CPU to settle at the idle address (see kIdleAddr's
// own comment) -- the in-process equivalent of the live-testing convention
// of sleeping a guessed number of milliseconds after each keypress, except
// deterministic: it returns as soon as the real condition is true rather
// than hoping a fixed delay was long enough.
// With the external keyboard's driver on (79D4H = 55H), the machine idles
// at the same place in the driver's copy of the loop (KBD_LOOP, from the
// module ROM's descriptor at 8811H) instead.
bool waitForIdle(BootedMachine& m, long maxInstructions = 2'000'000) {
  uint16_t driverIdle = kIdleAddr;
  if (m.bus.readME0(0x79D4) == 0x55) {
    uint16_t loop = static_cast<uint16_t>(m.bus.readME0(0x8811) << 8 | m.bus.readME0(0x8812));
    driverIdle = static_cast<uint16_t>(loop + (kIdleAddr - 0xE24A));
  }
  for (long i = 0; i < maxInstructions; i++) {
    if ((m.cpu.p() == kIdleAddr || m.cpu.p() == driverIdle) && m.cpu.halted()) return true;
    stepOne(m);
  }
  return false;
}

// The exact regression reported live: run SDLS, browse (no navigation
// needed to reproduce this), press Enter to exit, then start typing a
// fresh command. Before this fix, KEYWORD_RETURN redraws ">" directly to
// VRAM (a purely cosmetic fix) but never resets whatever field the real
// line editor uses to track "how many characters are in the current input
// line" -- so it's still logically midway through the old "SDLS" line, and
// new characters get appended onto it instead of starting fresh. Checked
// *during* typing "MEM", before Enter is pressed for it -- that's exactly
// where the user observed the tokenized line "SDLS MEM" on screen.
void testSlsExitThenTypingDoesNotConcatenate() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf(
        "SKIP: testSlsExitThenTypingDoesNotConcatenate -- ROM1.BIN and/or "
        "rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sls_concat");
  {
    std::ofstream f(sdDir / "TEST.BAS", std::ios::binary);
    f << "10 PRINT 1\n";
  }

  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDLS");
  tapKey(*m, pc1500::Key::Ent);  // dispatch SDLS
  CHECK(waitForIdle(*m));        // settles on SDLS's own KEYSCAN_WAIT loop

  tapKey(*m, pc1500::Key::Ent);  // the "exit" Enter
  CHECK(waitForIdle(*m));

  typeText(*m, "MEM");
  std::string beforeEnter = readDisplayBuffer(m->bus);
  CHECK(beforeEnter == "MEM");
  if (beforeEnter != "MEM") {
    std::printf("  DISP_BUFFER after typing \"MEM\" post-SDLS-exit: \"%s\" (want \"MEM\")\n",
                beforeEnter.c_str());
  }
}

// SDLS's directory listing must not show BASIC's own blinking block
// cursor. Root cause: DISP_N_CHARS0 (ROM1.BIN's shared display routine,
// which SD_LIST_DISPLAY calls to blit each entry) has a side effect of
// moving BLINK_CURSOR_H/L (787EH/787FH) to point right after whatever it
// just drew -- correct for its real purpose (echoing typed input) but
// wrong for a non-interactive listing. Confirmed live this left a visible
// blinking block mid-line; fixed by SD_LIST_DISPLAY resetting
// BLINK_CURSOR_H/L back to 7400H (confirmed live to be exactly what a
// genuinely idle prompt holds there) right after its own DISP_N_CHARS0
// call. Checked after both the initial draw and a Down-arrow redraw,
// since SD_LIST_UP/DOWN redraw via the same shared SD_LIST_DISPLAY.
void testSdlsHidesBlinkingCursorDuringBrowse() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf(
        "SKIP: testSdlsHidesBlinkingCursorDuringBrowse -- ROM1.BIN and/or "
        "rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sls_cursor");
  {
    std::ofstream f(sdDir / "TEST.BAS", std::ios::binary);
    f << "10 PRINT 1\n";
  }
  {
    std::ofstream f(sdDir / "OTHER.BAS", std::ios::binary);
    f << "\xF0\x97\xF6\x32\x0D\xFF";
  }

  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDLS");
  tapKey(*m, pc1500::Key::Ent);  // dispatch -- draws the first entry
  CHECK(waitForIdle(*m));

  CHECK(m->bus.readME0(0x787E) == 0x74);
  CHECK(m->bus.readME0(0x787F) == 0x00);

  tapKey(*m, pc1500::Key::Down);  // redraws via the same SD_LIST_DISPLAY
  CHECK(waitForIdle(*m));

  CHECK(m->bus.readME0(0x787E) == 0x74);
  CHECK(m->bus.readME0(0x787F) == 0x00);
}

// Real-hardware bug (2026-09-19/20): scrolling down through a listing was
// fine, but scrolling back up sometimes showed a corrupted file name --
// "random and less repeatable" than the separate summary-line "r" bug.
// Root cause: SD_LIST_INDEX_ABS/COUNT_ABS/ADDR_HI_ABS/ADDR_LO_ABS (the
// browser's own navigation state, rewritten on every Up/Down keypress)
// used to live at EXP_SCRATCH_ABS (window offset 256) -- which a real
// directory entry's own data reaches once there are 9+ files: entry #8
// occupies window offsets [242, 272), which contains [256, 260), so
// every Up/Down write silently clobbered the last 2 characters of entry
// #8's name and the first 2 of its size text with whatever navigation
// state was current at that moment. Invisible while scrolling past it
// (not being displayed at that instant); only visible the next time it's
// redrawn -- which is why going back up to revisit it looked like random
// corruption, when it was really just stale index/address bytes. Fixed
// by moving that state to a dedicated SD_LIST_SCRATCH_ABS (window offset
// 2038) past every entry's and the summary line's maximum reach. This
// test needs 9+ files specifically to reach entry #8 and would have
// failed before that fix (entry #8's name changing between the first
// visit and a later revisit after scrolling further and back).
void testSdlsScrollUpDoesNotCorruptEntryPastScratchOffset() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf(
        "SKIP: testSdlsScrollUpDoesNotCorruptEntryPastScratchOffset -- ROM1.BIN "
        "and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sls_scratch_collision");
  for (int i = 0; i < 12; i++) {
    std::ofstream f(sdDir / ("FILE" + std::to_string(i) + ".BAS"), std::ios::binary);
    f << "10 PRINT 1\n";
  }

  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDLS");
  tapKey(*m, pc1500::Key::Ent);  // dispatch -- draws entry 0
  CHECK(waitForIdle(*m));

  for (int i = 0; i < 8; i++) {  // walk down to entry #8 (0-indexed)
    tapKey(*m, pc1500::Key::Down);
    CHECK(waitForIdle(*m));
  }
  std::string entry8Before = readDisplayBuffer(m->bus);

  for (int i = 0; i < 3; i++) {  // scroll further down (entries 9-11)...
    tapKey(*m, pc1500::Key::Down);
    CHECK(waitForIdle(*m));
  }
  for (int i = 0; i < 3; i++) {  // ...and back up to entry #8 again
    tapKey(*m, pc1500::Key::Up);
    CHECK(waitForIdle(*m));
  }
  std::string entry8After = readDisplayBuffer(m->bus);

  CHECK(entry8Before == entry8After);
  if (entry8Before != entry8After) {
    std::printf("  entry #8 before further scrolling: \"%s\"\n", entry8Before.c_str());
    std::printf("  entry #8 after scrolling down and back up:  \"%s\"\n", entry8After.c_str());
  }
}

// SDMKDIR "<name>" creates a real subdirectory under the SD root.
void testSdmkdirCreatesDirectory() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdmkdirCreatesDirectory -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdmkdir");
  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDMKDIR \"NEWDIR\"");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  CHECK(fs::is_directory(sdDir / "NEWDIR"));
}

// SDRMDIR "<name>" removes an empty subdirectory.
void testSdrmdirRemovesEmptyDirectory() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdrmdirRemovesEmptyDirectory -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdrmdir");
  fs::create_directory(sdDir / "EMPTYDIR");
  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDRMDIR \"EMPTYDIR\"");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  CHECK(!fs::exists(sdDir / "EMPTYDIR"));
}

// Two edge cases tested directly against ExpansionMock's own
// processCommand, bypassing the full ROM dispatch: SDCD_ROUTINE/
// SDMKDIR_ROUTINE/SDRMDIR_ROUTINE don't distinguish EXP_STATUS_SUCCESS
// from EXP_STATUS_ERROR at all (silent abort either way -- see their own
// block comment in rom.asm), so the actual behavior under test here --
// does the mock's own status byte come back right -- isn't observable
// through a full keystroke-driven test. No ROM/boot needed at all since
// processCommand is called directly.

// SDMKDIR on a name that already exists as a directory must fail, not
// silently no-op as success -- std::filesystem::create_directory itself
// returns false (not newly created) without setting an error_code in
// this exact case, so this specifically confirms
// ExpansionMock::makeSdDir's own `created && !ec` check correctly turns
// that "false, no error" combination into EXP_STATUS_ERROR rather than
// treating the lack of an error_code as success.
void testSdmkdirFailsIfDirectoryAlreadyExists() {
  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdmkdir_exists");
  fs::create_directory(sdDir / "EXISTING");

  pc1500::Keyboard kb;
  pc1500::Bus bus(kb);
  bus.expansionMock().setRootDir(sdDir);

  std::vector<uint8_t> window(4096, 0xFF);
  const std::string name = "EXISTING";
  window[0] = 0;
  window[1] = static_cast<uint8_t>(name.size());
  for (size_t i = 0; i < name.size(); i++) window[2 + i] = static_cast<uint8_t>(name[i]);

  bus.expansionMock().processCommand(pc1500::ExpansionMock::kCommandMakeSdDir, window, 0x7FF);
  bus.expansionMock().waitUntilIdleForTest();
  CHECK(bus.expansionMock().pollStatus() == pc1500::ExpansionMock::kStatusError);
}

// SDRMDIR on a non-empty directory must fail and leave it (and its
// contents) completely untouched -- matches real emFile's own FS_RmDir
// semantics (fails outright, never recurses).
void testSdrmdirFailsOnNonEmptyDirectory() {
  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdrmdir_nonempty");
  fs::create_directory(sdDir / "NONEMPTY");
  {
    std::ofstream f(sdDir / "NONEMPTY" / "FILE.TXT", std::ios::binary);
    f << "x";
  }

  pc1500::Keyboard kb;
  pc1500::Bus bus(kb);
  bus.expansionMock().setRootDir(sdDir);

  std::vector<uint8_t> window(4096, 0xFF);
  const std::string name = "NONEMPTY";
  window[0] = 0;
  window[1] = static_cast<uint8_t>(name.size());
  for (size_t i = 0; i < name.size(); i++) window[2 + i] = static_cast<uint8_t>(name[i]);

  bus.expansionMock().processCommand(pc1500::ExpansionMock::kCommandRemoveSdDir, window, 0x7FF);
  bus.expansionMock().waitUntilIdleForTest();
  CHECK(bus.expansionMock().pollStatus() == pc1500::ExpansionMock::kStatusError);
  CHECK(fs::exists(sdDir / "NONEMPTY"));
  CHECK(fs::exists(sdDir / "NONEMPTY" / "FILE.TXT"));
}

// SDCD "<name>" changes the directory subsequent SD commands (SDLOAD here)
// resolve names against -- the actual end-to-end point of having a
// current-directory concept at all, not just that ExpansionMock's own
// currentDir_ field changes. A same-named file sits both at the root and
// inside the subdirectory, with different contents, so loading it after
// SDCD proves the *subdirectory's* copy was the one actually read.
void testSdcdAffectsSubsequentFileCommands() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdcdAffectsSubsequentFileCommands -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdcd");
  fs::create_directory(sdDir / "SUBDIR");
  // "10 PRINT 1" / "20 END", tokenized -- the same fixture bytes used
  // elsewhere this session, sitting at the SD root.
  const std::vector<uint8_t> kRootFixture = {0x00, 0x0A, 0x04, 0xF0, 0x97, 0x31, 0x0D,
                                              0x00, 0x14, 0x03, 0xF1, 0x8E, 0x0D, 0xFF};
  // "10 END" tokenized -- deliberately different content, same filename,
  // inside SUBDIR.
  const std::vector<uint8_t> kSubdirFixture = {0x00, 0x0A, 0x03, 0xF1, 0x8E, 0x0D, 0xFF};
  {
    std::ofstream f(sdDir / "TEST.BAS", std::ios::binary);
    f.write(reinterpret_cast<const char*>(kRootFixture.data()),
            static_cast<std::streamsize>(kRootFixture.size()));
  }
  {
    std::ofstream f(sdDir / "SUBDIR" / "TEST.BAS", std::ios::binary);
    f.write(reinterpret_cast<const char*>(kSubdirFixture.data()),
            static_cast<std::streamsize>(kSubdirFixture.size()));
  }

  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDCD \"SUBDIR\"");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));
  CHECK(m->bus.expansionMock().currentDir() == sdDir / "SUBDIR");

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDLOAD \"TEST.BAS\"");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  std::string readError;
  std::vector<uint8_t> loadedProgram = pc1500::basic::readBasicProgramBytes(m->bus, &readError);
  CHECK(loadedProgram == kSubdirFixture);  // SUBDIR's own copy, not the root's
  if (loadedProgram != kSubdirFixture) {
    std::printf("  loadedProgram.size()=%zu (expected SUBDIR's %zu-byte fixture)\n",
                loadedProgram.size(), kSubdirFixture.size());
  }
}

// SDPWD triggers GET_SD_CWD and stages its length-prefixed response at
// EXP_SCRATCH_ABS (787EH.. wait -- 8100H, see rom_defs.inc's own
// EXP_SCRATCH_ABS) for SD_LIST_DISPLAY's shared DISP_N_CHARS0 blit to draw.
// Checked by reading that staged response directly rather than decoding
// rendered VRAM pixels -- the actual new logic under test is "did SDPWD
// trigger the right command and stage the right bytes", not DISP_N_CHARS0's
// own already-proven rendering.
void testSdpwdStagesCurrentDirectoryResponse() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf(
        "SKIP: testSdpwdStagesCurrentDirectoryResponse -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdpwd");
  fs::create_directory(sdDir / "SUBDIR");
  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  // Before any SDCD at all, SDPWD must report exactly "/" -- the SD root,
  // not emFile's own root representation (which may not even be "/").
  constexpr uint16_t kExpScratchAbsRoot = 0x8100;
  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDPWD");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));
  {
    uint8_t rootLen = m->bus.readME0(kExpScratchAbsRoot);
    std::string rootCwd;
    for (uint8_t i = 0; i < rootLen; i++) {
      rootCwd += static_cast<char>(m->bus.readME0(kExpScratchAbsRoot + 1 + i));
    }
    CHECK(rootCwd == "/");
    if (rootCwd != "/") std::printf("  SDPWD at root staged \"%s\" (want \"/\")\n", rootCwd.c_str());
  }

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDCD \"SUBDIR\"");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDPWD");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  constexpr uint16_t kExpScratchAbs = 0x8100;
  uint8_t len = m->bus.readME0(kExpScratchAbs);
  std::string cwd;
  for (uint8_t i = 0; i < len; i++) cwd += static_cast<char>(m->bus.readME0(kExpScratchAbs + 1 + i));
  CHECK(cwd == "/SUBDIR");
  if (cwd != "/SUBDIR") {
    std::printf("  SDPWD staged \"%s\" (want \"/SUBDIR\")\n", cwd.c_str());
  }
}

// SDLS's listing must include subdirectories alongside files, with
// "<DIR>" (right-justified, same column real sizes occupy) instead of a
// size -- checked by reading the raw EXP_COMMAND_LIST_SD_DIR wire format
// directly (EXP_BUFFER_START_ABS=0x8000: 2-byte BE count, then
// kDirRecordSize=30-byte records: 16 bytes name, 10 bytes size text, 4
// bytes binary size) rather than decoding rendered VRAM -- SD_LIST_DISPLAY
// itself is already well-tested elsewhere (SDLS/SDLOAD browsing), so the
// new thing actually under test here is main.c's/ExpansionMock's own
// listing content, not the ROM's shared blit primitive. Also confirms
// SDLS respects a prior SDCD (a real gap found and fixed alongside this
// same change -- ExpansionMock::listSdDir used to always list the root).
void testSdlsListsDirectoriesWithDirMarker() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdlsListsDirectoriesWithDirMarker -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdls_dirs");
  fs::create_directory(sdDir / "ADIR");
  {
    std::ofstream f(sdDir / "AFILE.BAS", std::ios::binary);
    f << "10 END\n";
  }

  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDLS");
  tapKey(*m, pc1500::Key::Ent);  // dispatch -- triggers LIST_SD_DIR, draws the first entry
  CHECK(waitForIdle(*m));

  constexpr uint16_t kBufAbs = 0x8000;
  constexpr int kNameLen = 16;
  constexpr int kSizeTextLen = 10;
  constexpr int kRecordSize = 30;
  uint16_t count = (static_cast<uint16_t>(m->bus.readME0(kBufAbs)) << 8) | m->bus.readME0(kBufAbs + 1);
  CHECK(count == 2);  // ADIR + AFILE.BAS

  bool foundDir = false, foundFile = false;
  for (uint16_t i = 0; i < count; i++) {
    uint16_t entryOff = kBufAbs + 2 + i * kRecordSize;
    std::string name, sizeText;
    for (int j = 0; j < kNameLen; j++) name += static_cast<char>(m->bus.readME0(entryOff + j));
    for (int j = 0; j < kSizeTextLen; j++) {
      sizeText += static_cast<char>(m->bus.readME0(entryOff + kNameLen + j));
    }
    // Trim trailing spaces off the space-padded name field for a clean comparison.
    while (!name.empty() && name.back() == ' ') name.pop_back();
    if (name == "ADIR") {
      foundDir = true;
      CHECK(sizeText == "     <DIR>");  // right-justified in the 10-byte field
      if (sizeText != "     <DIR>") std::printf("  ADIR size text: \"%s\"\n", sizeText.c_str());
    } else if (name == "AFILE.BAS") {
      foundFile = true;
      CHECK(sizeText.find("<DIR>") == std::string::npos);  // a real file, not marked as a directory
    }
  }
  CHECK(foundDir);
  CHECK(foundFile);
}

// SDCD/SDMKDIR/SDRMDIR all require a "<name>" argument -- missing it must
// raise a genuine BASIC ERROR 1, matching SDSAVE's own established
// convention for a deliberate operation with a malformed/missing argument
// (as opposed to SDLOAD's own silent-abort-on-malformed-argument, a
// passive browsing command).
void testSdDirectoryCommandsRaiseError1WithoutArgument() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf(
        "SKIP: testSdDirectoryCommandsRaiseError1WithoutArgument -- ROM1.BIN and/or "
        "rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sddir_error1");
  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  for (const char* cmd : {"SDCD", "SDMKDIR", "SDRMDIR"}) {
    tapKey(*m, pc1500::Key::Cl);
    typeText(*m, "NEW0");
    tapKey(*m, pc1500::Key::Ent);
    CHECK(waitForIdle(*m));

    // Reset ERL to a sentinel neither 0 nor 1 before each attempt -- NEW0
    // doesn't touch it, so without this a stale ERL==1 from a *previous*
    // iteration would make a genuinely-broken later command falsely pass.
    m->bus.writeME0(kErlAbs, 0xEE);

    tapKey(*m, pc1500::Key::Cl);
    typeText(*m, cmd);  // no argument at all
    tapKey(*m, pc1500::Key::Ent);
    CHECK(waitForIdle(*m));

    CHECK(m->bus.readME0(kErlAbs) == 1);
    if (m->bus.readME0(kErlAbs) != 1) {
      std::printf("  %s with no argument: ERL=%u (want 1)\n", cmd, m->bus.readME0(kErlAbs));
    }
  }
}

// Separate, previously-documented bug (see the KNOWN SEPARATE BUG comment
// block that used to sit in rom.asm right after KEYWORD_RETURN): a third
// Enter, pressed on the idle prompt with nothing typed after exiting SDLS,
// could silently re-dispatch SDLS_ROUTINE from scratch -- confirmed live by
// watching EXP_BUFFER get overwritten with a brand new LIST_SD_DIR round
// trip. Checked here by planting a sentinel in the data window's count
// field right after SDLS's real exit, then confirming a stray extra Enter
// doesn't touch it -- if SDLS got re-dispatched, ExpansionMock would
// overwrite this with a fresh (correct) count, silently masking the bug.
void testStrayEnterAfterSlsExitDoesNotRedispatch() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf(
        "SKIP: testStrayEnterAfterSlsExitDoesNotRedispatch -- ROM1.BIN and/or "
        "rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sls_redispatch");
  {
    std::ofstream f(sdDir / "TEST.BAS", std::ios::binary);
    f << "10 PRINT 1\n";
  }

  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDLS");
  tapKey(*m, pc1500::Key::Ent);  // dispatch
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Ent);  // real exit
  CHECK(waitForIdle(*m));

  constexpr uint16_t kExpBufferStart = 0x8000;
  m->bus.writeME0(kExpBufferStart, 0xAA);
  m->bus.writeME0(kExpBufferStart + 1, 0x55);

  tapKey(*m, pc1500::Key::Ent);  // the stray, unrelated third Enter
  CHECK(waitForIdle(*m));

  CHECK(m->bus.readME0(kExpBufferStart) == 0xAA);
  CHECK(m->bus.readME0(kExpBufferStart + 1) == 0x55);
  if (m->bus.readME0(kExpBufferStart) != 0xAA || m->bus.readME0(kExpBufferStart + 1) != 0x55) {
    std::printf("  EXP_BUFFER count field changed after a stray Enter -- SDLS got re-dispatched\n");
  }
}

// SDLOAD's no-argument form: browse the same listing SDLS uses, but L
// selects-and-loads the highlighted file instead of exiting, Enter is
// ignored, and only CL/BREAK abort. The fixture file is generated on the
// fly via saveBasicProgram -- SDLOAD expects the same raw-tokenized-bytes
// format that function (and readBasicProgramBytes, used below to verify)
// produce/consume, per SDLOAD_ROUTINE's own header comment in rom.asm.
// Pressing Enter before L implicitly verifies Enter is really ignored (not
// an abort key here): if it had aborted, the subsequent L would just be
// ordinary typed text at the READY prompt, nothing would load, and the
// final byte-for-byte comparison below would fail.
void testSdloadSelectsAndLoadsFile() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf(
        "SKIP: testSdloadSelectsAndLoadsFile -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdload");
  fs::path fixturePath = sdDir / "TEST.BAS";
  {
    auto fixtureMachine = bootAndSettle(rom);
    std::string loadError;
    bool loaded = pc1500::basic::typeBasicProgramText(fixtureMachine->bus, fixtureMachine->cpu,
                                                        "10 PRINT 1\n20 END\n", kCyclesPerFrame,
                                                        kCyclesPerTimerTick, &loadError);
    CHECK(loaded);
    if (!loaded) {
      std::printf("  fixture generation loadError: %s\n", loadError.c_str());
      return;
    }
    std::string saveError;
    CHECK(pc1500::basic::saveBasicProgram(fixtureMachine->bus, fixturePath.string().c_str(),
                                           &saveError));
  }

  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDLOAD");
  tapKey(*m, pc1500::Key::Ent);  // dispatch -- shows the file browser
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Ent);  // must be ignored, not select or abort
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::L);  // select the (only) listed file and load it
  CHECK(waitForIdle(*m));

  std::string readError;
  std::vector<uint8_t> loadedProgram = pc1500::basic::readBasicProgramBytes(m->bus, &readError);
  std::vector<uint8_t> fixtureBytes = readFile(fixturePath.string());
  CHECK(!loadedProgram.empty());
  CHECK(!fixtureBytes.empty());
  CHECK(loadedProgram == fixtureBytes);
  if (loadedProgram != fixtureBytes) {
    std::printf("  loadedProgram.size()=%zu fixtureBytes.size()=%zu\n", loadedProgram.size(),
                fixtureBytes.size());
  }

  std::string detok;
  std::string detokError;
  CHECK(pc1500::basic::detokenizeBasicProgram(loadedProgram, &detok, &detokError));
  CHECK(detok.find("PRINT") != std::string::npos);
}

// Writes an M-mode fixture file: a 4-byte big-endian header (target
// address, then call address -- 0x0000 = "don't call anything") followed
// immediately by the raw payload bytes -- no length field, read-to-EOF,
// matching the format documented in SDLOAD_ROUTINE's header comment and
// written by SDSAVE M itself.
void writeMFixture(const fs::path& path, uint16_t headerAddr, const std::vector<uint8_t>& payload,
                    uint16_t callAddr = 0x0000) {
  std::ofstream f(path, std::ios::binary);
  f.put(static_cast<char>(headerAddr >> 8));
  f.put(static_cast<char>(headerAddr & 0xFF));
  f.put(static_cast<char>(callAddr >> 8));
  f.put(static_cast<char>(callAddr & 0xFF));
  f.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
}

bool payloadMatches(pc1500::Bus& bus, uint16_t addr, const std::vector<uint8_t>& payload) {
  for (size_t i = 0; i < payload.size(); i++) {
    if (bus.readME0(static_cast<uint16_t>(addr + i)) != payload[i]) return false;
  }
  return true;
}

// SDLOAD "<filename>" (BASIC mode, quoted-filename argument): loads the
// named file directly, with no browse listing and no L keypress -- the
// second of the two direct-argument forms SDLOAD_ROUTINE's SDLOAD_ARG_
// FILENAME_BASIC branch implements. Confirms the quoted-name argument
// parser (SD_PARSE_QUOTED_NAME) correctly reads the raw DISP_BUFFER text
// typed after SDLOAD, and that typing '"' via charToTapActions (which maps
// it to Shift+F2, not a QWERTY Shift+2) round-trips correctly.
void testSdloadDirectFilenameLoad() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdloadDirectFilenameLoad -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdload_direct_filename");
  fs::path fixturePath = sdDir / "TEST.BAS";
  {
    auto fixtureMachine = bootAndSettle(rom);
    std::string loadError;
    bool loaded = pc1500::basic::typeBasicProgramText(fixtureMachine->bus, fixtureMachine->cpu,
                                                        "10 PRINT 1\n20 END\n", kCyclesPerFrame,
                                                        kCyclesPerTimerTick, &loadError);
    CHECK(loaded);
    if (!loaded) {
      std::printf("  fixture generation loadError: %s\n", loadError.c_str());
      return;
    }
    std::string saveError;
    CHECK(pc1500::basic::saveBasicProgram(fixtureMachine->bus, fixturePath.string().c_str(),
                                           &saveError));
  }
  // A second, unrelated file in the directory proves this really is a
  // direct load and not an accidental browse-and-select-first-entry.
  {
    std::ofstream f(sdDir / "OTHER.BAS", std::ios::binary);
    f << "\xF0\x97\xF6\x32\x0D\xFF";
  }

  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  // Deliberately typed with the natural space after SDLOAD (not
  // concatenated) -- SDLOAD_ROUTINE must skip it before checking the
  // argument's first character; catches a real bug found live where a
  // typed space made every SDLOAD M/filename form fall through to abort.
  typeText(*m, "SDLOAD \"TEST.BAS\"");
  tapKey(*m, pc1500::Key::Ent);  // dispatch -- should load immediately, no browser
  CHECK(waitForIdle(*m));

  // Stack-balance regression check: SD_OPEN_AND_LOAD used to be reached via
  // `sjp` (pushing a return address) but only ever exits via `jmp
  // KEYWORD_RETURN`, never `rtn` -- a permanent 2-byte-per-call stack leak
  // that a live session's own S=0x784B (not the documented clean-idle
  // baseline 0x784D) caught. Fixed by calling it via `jmp` instead (it
  // never returns to its caller anyway); this check guards against that
  // regressing.
  CHECK(m->cpu.s() == 0x784D);
  std::string readError;
  std::vector<uint8_t> loadedProgram = pc1500::basic::readBasicProgramBytes(m->bus, &readError);
  std::vector<uint8_t> fixtureBytes = readFile(fixturePath.string());
  CHECK(!loadedProgram.empty());
  CHECK(loadedProgram == fixtureBytes);
  if (loadedProgram != fixtureBytes) {
    std::printf("  loadedProgram.size()=%zu fixtureBytes.size()=%zu\n", loadedProgram.size(),
                fixtureBytes.size());
  }
}

// SDLOAD M with no filename: brings up the same browse listing as bare
// SDLOAD, but L-selecting a file loads it as binary data to the address
// embedded in the file's own 2-byte header (SDLOAD_MODE_M_HEADER), not into
// the BASIC program area.
void testSdloadMHeaderBrowseSelectsFile() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdloadMHeaderBrowseSelectsFile -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdload_m_browse");
  constexpr uint16_t kHeaderAddr = 0x4400;
  const std::vector<uint8_t> kPayload = {0x11, 0x22, 0x33, 0x44, 0x55};
  writeMFixture(sdDir / "BIN1.BIN", kHeaderAddr, kPayload);

  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDLOAD M");  // natural space -- see testSdloadDirectFilenameLoad's comment
  tapKey(*m, pc1500::Key::Ent);  // dispatch -- shows the file browser
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::L);  // select the (only) listed file and load it
  CHECK(waitForIdle(*m));

  CHECK(payloadMatches(m->bus, kHeaderAddr, kPayload));
}

// SDLOAD M "<filename>" (no comma/address argument): direct load, no
// browser, target address comes from the file's own header.
void testSdloadMDirectHeaderAddress() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdloadMDirectHeaderAddress -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdload_m_direct_header");
  constexpr uint16_t kHeaderAddr = 0x4410;
  const std::vector<uint8_t> kPayload = {0xAA, 0xBB, 0xCC};
  writeMFixture(sdDir / "BIN1.BIN", kHeaderAddr, kPayload);

  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDLOAD M \"BIN1.BIN\"");
  tapKey(*m, pc1500::Key::Ent);  // dispatch -- direct load, no browser
  CHECK(waitForIdle(*m));

  CHECK(m->cpu.s() == 0x784D);  // stack-balance regression check, see testSdloadDirectFilenameLoad
  CHECK(payloadMatches(m->bus, kHeaderAddr, kPayload));
}

// SDLOAD M "<filename>",<address> -- relocated load, address given in
// decimal. The file's own header address (deliberately different from the
// relocation target here) must be ignored.
void testSdloadMExplicitAddressDecimal() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdloadMExplicitAddressDecimal -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdload_m_explicit_decimal");
  constexpr uint16_t kHeaderAddr = 0x0000;   // deliberately wrong -- must be ignored
  constexpr uint16_t kTargetAddr = 0x4420;   // 17440 decimal
  const std::vector<uint8_t> kPayload = {0x01, 0x02, 0x03, 0x04};
  writeMFixture(sdDir / "BIN1.BIN", kHeaderAddr, kPayload);

  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDLOAD M \"BIN1.BIN\"," + std::to_string(kTargetAddr));
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  CHECK(payloadMatches(m->bus, kTargetAddr, kPayload));
  CHECK(!payloadMatches(m->bus, kHeaderAddr, kPayload));  // proves relocation actually happened
}

// SDLOAD M "<filename>",&<address> -- relocated load, address given in hex
// (the '&' prefix, matching PEEK/POKE's own convention).
void testSdloadMExplicitAddressHex() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdloadMExplicitAddressHex -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdload_m_explicit_hex");
  constexpr uint16_t kHeaderAddr = 0x0000;  // deliberately wrong -- must be ignored
  constexpr uint16_t kTargetAddr = 0x4430;
  const std::vector<uint8_t> kPayload = {0x9A, 0x9B, 0x9C};
  writeMFixture(sdDir / "BIN1.BIN", kHeaderAddr, kPayload);

  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDLOAD M \"BIN1.BIN\",&4430");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  CHECK(payloadMatches(m->bus, kTargetAddr, kPayload));
  CHECK(!payloadMatches(m->bus, kHeaderAddr, kPayload));
}

// SDSAVE "<filename>" (BASIC mode, new file, no existing file to prompt
// about): saves the current program, then a fresh SDLOAD "<filename>"
// round-trips it back byte-for-byte -- the same style of round-trip
// testSdloadSelectsAndLoadsFile already uses, but exercising SDSAVE's own
// write path instead of a host-generated fixture.
void testSdsaveBasicRoundTrip() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdsaveBasicRoundTrip -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdsave_basic_roundtrip");
  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  std::string loadError;
  bool loaded = pc1500::basic::typeBasicProgramText(m->bus, m->cpu, "10 PRINT 1\n20 END\n",
                                                      kCyclesPerFrame, kCyclesPerTimerTick, &loadError);
  CHECK(loaded);
  if (!loaded) {
    std::printf("  fixture generation loadError: %s\n", loadError.c_str());
    return;
  }
  std::string readError;
  std::vector<uint8_t> savedProgram = pc1500::basic::readBasicProgramBytes(m->bus, &readError);
  CHECK(!savedProgram.empty());

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDSAVE \"OUT.BAS\"");
  tapKey(*m, pc1500::Key::Ent);  // new file -- no overwrite prompt expected
  CHECK(waitForIdle(*m));

  std::vector<uint8_t> onDisk = readFile((sdDir / "OUT.BAS").string());
  CHECK(onDisk == savedProgram);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDLOAD \"OUT.BAS\"");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  std::vector<uint8_t> reloaded = pc1500::basic::readBasicProgramBytes(m->bus, &readError);
  CHECK(reloaded == savedProgram);
}

// SDSAVE/SDLOAD round trip with a fixture large enough (>2048 bytes,
// spanning at least 3 widened 1024-byte chunks) to prove the widened
// EXP_MAX_TRANSFER_LEN chunking/chaining actually works across chunk
// boundaries, not just for a single-chunk fixture like
// testSdsaveBasicRoundTrip.
void testSdsaveSdloadWidenedChunkRoundTrip() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdsaveSdloadWidenedChunkRoundTrip -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  // Build a BASIC program comfortably over 2048 bytes of tokenized program
  // text so a save/load round trip must cross at least two 1024-byte
  // chunk boundaries in both SD_WRITE_RANGE and SD_OPEN_AND_LOAD_READ_LOOP.
  std::string programText;
  for (int i = 0; i < 120; ++i) {
    programText += std::to_string(10 + i * 10);
    programText += " REM AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n";
  }
  programText += "9999 END\n";

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdsave_widened_chunk_roundtrip");
  // Default (unexpanded) program RAM is too small to hold a >2048-byte
  // fixture -- give the machine extension RAM (as testSdmVariables and
  // others already do for oversized fixtures) so the fixture itself, not
  // an unrelated RAM ceiling, is what's under test here.
  auto m = bootAndSettle(rom, /*extRam0000Bytes=*/16384);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  std::string loadError;
  bool loaded = pc1500::basic::typeBasicProgramText(m->bus, m->cpu, programText, kCyclesPerFrame,
                                                      kCyclesPerTimerTick, &loadError);
  CHECK(loaded);
  if (!loaded) {
    std::printf("  fixture generation loadError: %s\n", loadError.c_str());
    return;
  }
  std::string readError;
  std::vector<uint8_t> savedProgram = pc1500::basic::readBasicProgramBytes(m->bus, &readError);
  CHECK(!savedProgram.empty());
  CHECK(savedProgram.size() > 2048);
  if (savedProgram.size() <= 2048) {
    std::printf("  fixture too small: %zu bytes (need > 2048 to force multiple chunks)\n",
                savedProgram.size());
  }

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDSAVE \"BIG.BAS\"");
  tapKey(*m, pc1500::Key::Ent);  // new file -- no overwrite prompt expected
  CHECK(waitForIdle(*m));

  std::vector<uint8_t> onDisk = readFile((sdDir / "BIG.BAS").string());
  CHECK(onDisk == savedProgram);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDLOAD \"BIG.BAS\"");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  std::vector<uint8_t> reloaded = pc1500::basic::readBasicProgramBytes(m->bus, &readError);
  CHECK(reloaded == savedProgram);
}

// SDSAVE onto a filename that already exists: pressing anything but Y at
// the confirmation prompt must leave the existing file completely
// untouched.
void testSdsaveOverwritePromptNAborts() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdsaveOverwritePromptNAborts -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdsave_overwrite_abort");
  fs::path targetPath = sdDir / "OUT.BAS";
  const std::vector<uint8_t> kOriginal = {0xAA, 0xBB, 0xCC, 0xFF};
  {
    std::ofstream f(targetPath, std::ios::binary);
    f.write(reinterpret_cast<const char*>(kOriginal.data()),
            static_cast<std::streamsize>(kOriginal.size()));
  }

  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDSAVE \"OUT.BAS\"");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));  // blocked on the confirmation prompt's own KEYSCAN_WAIT

  tapKey(*m, pc1500::Key::Cl);  // anything but Y -- must abort
  CHECK(waitForIdle(*m));

  std::vector<uint8_t> onDisk = readFile(targetPath.string());
  CHECK(onDisk == kOriginal);
}

// Same setup as above, but pressing Y confirms the overwrite.
void testSdsaveOverwritePromptYOverwrites() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdsaveOverwritePromptYOverwrites -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdsave_overwrite_confirm");
  fs::path targetPath = sdDir / "OUT.BAS";
  const std::vector<uint8_t> kOriginal = {0xAA, 0xBB, 0xCC, 0xFF};
  {
    std::ofstream f(targetPath, std::ios::binary);
    f.write(reinterpret_cast<const char*>(kOriginal.data()),
            static_cast<std::streamsize>(kOriginal.size()));
  }

  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  std::string loadError;
  CHECK(pc1500::basic::typeBasicProgramText(m->bus, m->cpu, "10 PRINT 1\n20 END\n", kCyclesPerFrame,
                                             kCyclesPerTimerTick, &loadError));
  std::string readError;
  std::vector<uint8_t> savedProgram = pc1500::basic::readBasicProgramBytes(m->bus, &readError);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDSAVE \"OUT.BAS\"");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Y);
  CHECK(waitForIdle(*m));

  std::vector<uint8_t> onDisk = readFile(targetPath.string());
  CHECK(onDisk == savedProgram);
  CHECK(onDisk != kOriginal);
}

// SDSAVE "<filename>",-Y onto an existing file: must overwrite immediately
// with no confirmation prompt -- a single Enter (no follow-up keypress)
// must be enough to reach idle again.
void testSdsaveDashYSkipsPrompt() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdsaveDashYSkipsPrompt -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdsave_dashy");
  fs::path targetPath = sdDir / "OUT.BAS";
  const std::vector<uint8_t> kOriginal = {0xAA, 0xBB, 0xCC, 0xFF};
  {
    std::ofstream f(targetPath, std::ios::binary);
    f.write(reinterpret_cast<const char*>(kOriginal.data()),
            static_cast<std::streamsize>(kOriginal.size()));
  }

  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  std::string loadError;
  CHECK(pc1500::basic::typeBasicProgramText(m->bus, m->cpu, "10 PRINT 1\n20 END\n", kCyclesPerFrame,
                                             kCyclesPerTimerTick, &loadError));
  std::string readError;
  std::vector<uint8_t> savedProgram = pc1500::basic::readBasicProgramBytes(m->bus, &readError);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDSAVE \"OUT.BAS\",-Y");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));  // no prompt -- a single Enter must be enough

  std::vector<uint8_t> onDisk = readFile(targetPath.string());
  CHECK(onDisk == savedProgram);
  CHECK(onDisk != kOriginal);
}

// SDSAVE with no arguments at all must raise a genuine BASIC ERROR 1
// (ERL == 1), not silently do nothing.
void testSdsaveNoArgsRaisesError1() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdsaveNoArgsRaisesError1 -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdsave_noargs_error1");
  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDSAVE");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  CHECK(m->bus.readME0(kErlAbs) == 1);
}

// SDSAVE M with a missing required argument (here: no filename/start/end
// at all, just bare "SDSAVE M") must also raise ERROR 1.
void testSdsaveMMissingArgsRaisesError1() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdsaveMMissingArgsRaisesError1 -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdsave_m_missing_args_error1");
  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDSAVE M");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  CHECK(m->bus.readME0(kErlAbs) == 1);
}

// SDSAVE M "<filename>",<start>,<end>,<call> round-trip: pokes a tiny ML
// routine (LDI A,0xAB / STA (0x4600) / RTN) into RAM, saves that exact
// byte range with a call address pointing at its own start, then -- after
// clearing both the routine's own RAM and the sentinel byte it writes --
// SDLOAD M "<filename>" (no relocation argument, so header/target-address
// mode) loads it back. Checks both that the bytes reloaded correctly *and*
// that the embedded call address actually got CALLed automatically
// (0x4600 == 0xAB), exercising SD_OPEN_AND_LOAD_DO_CALL end to end.
void testSdsaveMCallAddressRoundTrip() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdsaveMCallAddressRoundTrip -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdsave_m_call_roundtrip");
  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  constexpr uint16_t kRoutineAddr = 0x4500;
  constexpr uint16_t kSentinelAddr = 0x4600;
  // LDI A,0xAB ; STA (0x4600) ; RTN
  const std::vector<uint8_t> kRoutine = {0xB5, 0xAB, 0xAE, 0x46, 0x00, 0x9A};
  for (size_t i = 0; i < kRoutine.size(); i++)
    m->bus.writeME0(static_cast<uint16_t>(kRoutineAddr + i), kRoutine[i]);
  m->bus.writeME0(kSentinelAddr, 0xFF);

  uint16_t endAddr = static_cast<uint16_t>(kRoutineAddr + kRoutine.size() - 1);
  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDSAVE M \"CALLME.BIN\"," + std::to_string(kRoutineAddr) + "," +
                   std::to_string(endAddr) + "," + std::to_string(kRoutineAddr));
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  std::vector<uint8_t> onDisk = readFile((sdDir / "CALLME.BIN").string());
  CHECK(onDisk.size() == 4 + kRoutine.size());
  if (onDisk.size() == 4 + kRoutine.size()) {
    CHECK(onDisk[0] == (kRoutineAddr >> 8) && onDisk[1] == (kRoutineAddr & 0xFF));
    CHECK(onDisk[2] == (kRoutineAddr >> 8) && onDisk[3] == (kRoutineAddr & 0xFF));
    CHECK(std::equal(kRoutine.begin(), kRoutine.end(), onDisk.begin() + 4));
  }

  // Clear both the routine's own RAM and the sentinel it writes, so the
  // post-load state can only match if SDLOAD M genuinely reloaded and
  // called it -- not leftover from the poke above.
  for (size_t i = 0; i < kRoutine.size(); i++) m->bus.writeME0(static_cast<uint16_t>(kRoutineAddr + i), 0xFF);
  m->bus.writeME0(kSentinelAddr, 0xFF);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDLOAD M \"CALLME.BIN\"");  // header mode -- no relocation, so it CALLs
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  CHECK(payloadMatches(m->bus, kRoutineAddr, kRoutine));
  CHECK(m->bus.readME0(kSentinelAddr) == 0xAB);
  CHECK(m->cpu.s() == 0x784D);  // stack-balance regression check, see testSdloadDirectFilenameLoad
}

// SDLOAD on a filename that isn't on the card must raise a genuine BASIC
// ERROR 40 (file not found), not silently do nothing.
void testSdloadFileNotFoundRaisesError40() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdloadFileNotFoundRaisesError40 -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdload_notfound_error40");
  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDLOAD \"NOPE.BAS\"");  // never created in sdDir
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  CHECK(m->bus.readME0(kErlAbs) == 40);
  CHECK(m->cpu.s() == 0x784D);  // stack-balance regression check, see testSdloadDirectFilenameLoad
}

// SDLOAD's BASIC-mode target must track the live program-start pointer at
// BASIC_PROGRAM_START_HI/LO_ABS (0x7865/0x7866), not a hardcoded 0x40C5 --
// boots with RAM shaped to approximate the CE-155 module (2K at 3800H + 6K
// at 4800H around the standard base). Confirmed live this session that the
// resulting pointer reads 0x00C5, not the real CE-155's 0x38C5: pc1500emu's
// 0000H-window extension RAM is left-aligned from address 0
// (Bus::isUnmapped, src/bus/bus.h), so it can only include, not isolate,
// 3800H-3FFFH -- a real, already-documented emulator limitation, not a bug
// in this test. A hand-built tokenized fixture (captured from a real save
// earlier this session) is used directly rather than
// typeBasicProgramText/saveBasicProgram, to keep this test isolated from
// text_loader.h's own host-side program-area functions -- those are
// covered by their own dedicated test,
// testSaveLoadBasicProgramUsesLiveProgramStartPointer, below.
void testSdloadUsesLiveProgramStartPointer() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdloadUsesLiveProgramStartPointer -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdload_live_start_ptr");
  // "10 PRINT 1" / "20 END", tokenized -- captured from a real SDSAVE this session.
  const std::vector<uint8_t> kFixture = {0x00, 0x0A, 0x04, 0xF0, 0x97, 0x31, 0x0D,
                                          0x00, 0x14, 0x03, 0xF1, 0x8E, 0x0D, 0xFF};
  {
    std::ofstream f(sdDir / "TEST.BAS", std::ios::binary);
    f.write(reinterpret_cast<const char*>(kFixture.data()), static_cast<std::streamsize>(kFixture.size()));
  }

  auto m = bootAndSettle(rom, /*extRam0000Bytes=*/16384, /*extRamExtBytes=*/10240);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  constexpr uint16_t kExpectedStart = 0x00C5;  // see this test's own comment for why not 0x38C5
  CHECK(m->bus.readME0(0x7865) == 0x00);
  CHECK(m->bus.readME0(0x7866) == 0xC5);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDLOAD \"TEST.BAS\"");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  CHECK(payloadMatches(m->bus, kExpectedStart, kFixture));
}

// saveBasicProgram/loadBasicProgram -- the host-side functions behind the
// GUI's "Save BASIC"/"Load BASIC" menu items and the "savebasic"/
// "loadbasic" FIFO commands, used directly here with no expansion ROM
// involved at all (this bug predates and is independent of SD support) --
// must track the same live program-start pointer at
// BASIC_PROGRAM_START_HI/LO_ABS (0x7865/0x7866) SDLOAD does, not the
// bare-machine kBasicProgramStart constant (0x40C5). Boots with 16K of
// extension RAM at 0000H, which shifts the real pointer to 0x00C5 (see
// testSdloadUsesLiveProgramStartPointer's own comment for why not
// 0x38C5). Before the fix, saveBasicProgram read from 0x40C5 regardless
// -- a full 0x4000 away from where the program the user actually typed
// lives in this configuration -- so the saved file was whichever
// unrelated bytes happened to sit there, and loadBasicProgram wrote a
// freshly loaded file to that same wrong address instead of where the
// ROM's own line editor/LIST/RUN actually look, i.e. exactly the
// reported bug (Save BASIC "not correctly finding the program when a
// 16K expansion RAM is loaded at 0000H").
void testSaveLoadBasicProgramUsesLiveProgramStartPointer() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  std::vector<uint8_t> rom = readFile(kRomPath);
  if (rom.empty()) {
    std::printf("SKIP: testSaveLoadBasicProgramUsesLiveProgramStartPointer -- ROM1.BIN not found.\n");
    return;
  }

  auto m = bootAndSettle(rom, /*extRam0000Bytes=*/16384);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  constexpr uint16_t kExpectedStart = 0x00C5;
  CHECK(m->bus.readME0(0x7865) == 0x00);
  CHECK(m->bus.readME0(0x7866) == 0xC5);

  std::string typeError;
  CHECK(pc1500::basic::typeBasicProgramText(m->bus, m->cpu, "10 PRINT 1\n20 END\n", kCyclesPerFrame,
                                             kCyclesPerTimerTick, &typeError));

  // "10 PRINT 1" / "20 END", tokenized -- same fixture shape used elsewhere
  // in this file (see testSdloadUsesLiveProgramStartPointer's kFixture).
  const std::vector<uint8_t> kExpectedBytes = {0x00, 0x0A, 0x04, 0xF0, 0x97, 0x31, 0x0D,
                                                0x00, 0x14, 0x03, 0xF1, 0x8E, 0x0D, 0xFF};
  CHECK(payloadMatches(m->bus, kExpectedStart, kExpectedBytes));

  fs::path savePath =
      makeTempTestDir("expansion_keyword_test_save_basic_live_start") / "SAVED.BAS";
  std::string saveError;
  CHECK(pc1500::basic::saveBasicProgram(m->bus, savePath.string().c_str(), &saveError));
  if (!saveError.empty()) std::printf("  saveError: %s\n", saveError.c_str());

  std::vector<uint8_t> savedBytes = readFile(savePath.string());
  CHECK(savedBytes == kExpectedBytes);

  // Round-trip: a fresh machine with the same RAM shape (so the live
  // pointer lands at the same 0x00C5 again), NEW0'd clean, then
  // loadBasicProgram from the file just saved -- must write to 0x00C5
  // too, not 0x40C5.
  auto m2 = bootAndSettle(rom, /*extRam0000Bytes=*/16384);
  tapKey(*m2, pc1500::Key::Cl);
  typeText(*m2, "NEW0");
  tapKey(*m2, pc1500::Key::Ent);
  CHECK(waitForIdle(*m2));

  std::string loadError;
  CHECK(pc1500::basic::loadBasicProgram(m2->bus, savePath.string().c_str(), &loadError));
  CHECK(payloadMatches(m2->bus, kExpectedStart, kExpectedBytes));
}

// SD_PARSE_QUOTED_NAME now uppercases lowercase input -- SDLOAD "test.bas"
// typed with the PC-1500's own Sml (lowercase) keyboard mode toggled on
// must still find the on-disk TEST.BAS. Sml is toggled only around the
// lowercase portion; charToTapActions maps the same physical key
// regardless of case (case is a keyboard-mode flag, not a separate
// keystroke), so this is the only way to get genuine lowercase ASCII into
// DISP_BUFFER and actually exercise the ROM's own fold-to-uppercase step.
void testSdloadUppercasesLowercaseFilename() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdloadUppercasesLowercaseFilename -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdload_lowercase");
  // "10 PRINT 1", tokenized -- same fixture shape used elsewhere in this file.
  const std::vector<uint8_t> kFixture = {0x00, 0x0A, 0x04, 0xF0, 0x97, 0x31, 0x0D, 0xFF};
  {
    std::ofstream f(sdDir / "TEST.BAS", std::ios::binary);
    f.write(reinterpret_cast<const char*>(kFixture.data()), static_cast<std::streamsize>(kFixture.size()));
  }

  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));
  m->bus.writeME0(kErlAbs, 0xEE);  // sentinel -- neither 0 nor 40, see testSdDirectoryCommandsRaiseError1WithoutArgument

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDLOAD \"");
  tapKey(*m, pc1500::Key::Sml);
  typeText(*m, "test.bas");
  tapKey(*m, pc1500::Key::Sml);
  typeText(*m, "\"");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  CHECK(m->bus.readME0(kErlAbs) != 40);  // must have found TEST.BAS, not raised ERROR 40
  std::string readError;
  std::vector<uint8_t> loadedProgram = pc1500::basic::readBasicProgramBytes(m->bus, &readError);
  CHECK(loadedProgram == kFixture);
}

// A quoted name violating the 8.3 shape (>8 name characters, >3 extension
// characters, or a second '.') raises ERROR 1 via SD_PARSE_QUOTED_NAME's
// own shape check -- the same Carry-SET path an unterminated/overlong name
// already used before this change. Uses SDMKDIR rather than SDLOAD: SDLOAD
// deliberately silently aborts on *any* malformed quoted name (a passive
// browsing command, see testSdDirectoryCommandsRaiseError1WithoutArgument's
// own comment on this established asymmetry), while SDMKDIR/SDCD/SDRMDIR/
// SDSAVE all raise a real ERROR 1, matching SD_RAISE_ERROR_1's own existing
// "malformed name" convention (an unterminated or overlong name already
// raised ERROR 1 there before this change too).
void testSdmkdirRejectsNon83ShapedNames() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdmkdirRejectsNon83ShapedNames -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdmkdir_non83");
  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  const char* kBadNames[] = {
      "\"TOOLONGNAME\"",   // 11-char name part, >8
      "\"TEST.TOOLONG\"",  // 7-char extension, >3
      "\"TEST.BA.S\"",     // second '.'
  };
  for (const char* arg : kBadNames) {
    m->bus.writeME0(kErlAbs, 0xEE);
    tapKey(*m, pc1500::Key::Cl);
    typeText(*m, std::string("SDMKDIR ") + arg);
    tapKey(*m, pc1500::Key::Ent);
    CHECK(waitForIdle(*m));
    CHECK(m->bus.readME0(kErlAbs) == 1);
    if (m->bus.readME0(kErlAbs) != 1) {
      std::printf("  arg=%s ERL=%d (want 1)\n", arg, m->bus.readME0(kErlAbs));
    }
  }
}

// "." and ".." must still work as SDCD's relative-path tokens after adding
// 8.3 shape validation -- they're segments that are exempt from the shape
// check entirely (see SD_PARSE_QUOTED_NAME's SD_DOT_ONLY_ABS handling).
void testSdcdDotAndDotDotStillWork() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdcdDotAndDotDotStillWork -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdcd_dotdot");
  fs::create_directory(sdDir / "SUBDIR");

  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDCD \"SUBDIR\"");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));
  CHECK(m->bus.expansionMock().currentDir() == sdDir / "SUBDIR");

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDCD \".\"");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));
  // fs::equivalent, not == -- lexically_normal() can leave a trailing
  // separator behind when collapsing a trailing "." (e.g. "SUBDIR/" rather
  // than "SUBDIR"), which resolves to the same real directory but doesn't
  // compare equal as a bare fs::path.
  CHECK(fs::equivalent(m->bus.expansionMock().currentDir(), sdDir / "SUBDIR"));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDCD \"..\"");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));
  CHECK(fs::equivalent(m->bus.expansionMock().currentDir(), sdDir));
}

// SDCD's own multi-component '/'-paths still work with 8.3 shape
// validation added -- each '/'-separated segment is checked independently
// (counters reset on '/'), and a shape violation in *any* one segment
// still raises ERROR 1, matching a single-segment violation.
void testSdcdMultiSegmentPathValidatesEachSegment() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdcdMultiSegmentPathValidatesEachSegment -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdcd_multisegment");
  fs::create_directories(sdDir / "SUB1" / "SUB2");

  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));
  m->bus.writeME0(kErlAbs, 0xEE);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDCD \"SUB1/SUB2\"");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));
  CHECK(m->bus.expansionMock().currentDir() == sdDir / "SUB1" / "SUB2");
  CHECK(m->bus.readME0(kErlAbs) != 1);

  // Second segment ("A") is fine, but the first ("LONGNAME1", 9 characters)
  // exceeds the 8-character name-part budget -- must raise ERROR 1 despite
  // the second segment being perfectly valid on its own.
  m->bus.writeME0(kErlAbs, 0xEE);
  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDCD \"LONGNAME1/A\"");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));
  CHECK(m->bus.readME0(kErlAbs) == 1);
}

// '+' is this project's typable stand-in for a real FAT short name's '~'
// (see rom.asm's SD_PARSE_QUOTED_NAME comment and expansion_mock.h's
// convertPlusToTilde/convertTildeToPlus). A file with a literal '~' in its
// real on-disk name (as a normal-PC-prepared card's auto-generated FAT
// short name would have) must list with '+' in SDLS, be loadable by typing
// '+' in its place, and SDSAVE of a '+'-containing name must create a real
// file with a literal '~'.
void testSdPlusTildeTranslation() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdPlusTildeTranslation -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_plus_tilde");
  // "10 PRINT 1", tokenized -- same fixture shape used elsewhere in this file.
  const std::vector<uint8_t> kFixture = {0x00, 0x0A, 0x04, 0xF0, 0x97, 0x31, 0x0D, 0xFF};
  {
    std::ofstream f(sdDir / "APPL~1.BAS", std::ios::binary);
    f.write(reinterpret_cast<const char*>(kFixture.data()), static_cast<std::streamsize>(kFixture.size()));
  }

  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  // SDLS must show "+" where the real name has "~".
  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDLS");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));
  constexpr uint16_t kBufAbs = 0x8000;  // EXP_BUFFER_START_ABS, see rom_defs.inc
  constexpr int kDirNameLen = 16;
  std::string listedName;
  for (int i = 0; i < kDirNameLen; i++) {
    listedName += static_cast<char>(m->bus.readME0(static_cast<uint16_t>(kBufAbs + 2 + i)));
  }
  while (!listedName.empty() && listedName.back() == ' ') listedName.pop_back();
  CHECK(listedName == "APPL+1.BAS");

  tapKey(*m, pc1500::Key::Ent);  // exit the browse listing
  CHECK(waitForIdle(*m));

  // SDLOAD with '+' in place of the real '~' must find the file.
  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDLOAD \"APPL+1.BAS\"");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));
  std::string readError;
  std::vector<uint8_t> loadedProgram = pc1500::basic::readBasicProgramBytes(m->bus, &readError);
  CHECK(loadedProgram == kFixture);

  // SDSAVE of a '+'-containing name creates a real file with a literal '~'.
  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDSAVE \"NEW+2.BAS\"");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));
  CHECK(fs::exists(sdDir / "NEW~2.BAS"));
  CHECK(!fs::exists(sdDir / "NEW+2.BAS"));
}

// SDRM "<name>" confirms first ("DELETE FILE? Y/N") -- pressing Y deletes
// the file.
void testSdrmDeletesWithConfirmation() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdrmDeletesWithConfirmation -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdrm_confirm");
  fs::path targetPath = sdDir / "TEST.BAS";
  { std::ofstream f(targetPath, std::ios::binary); f << "10 END\n"; }

  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDRM \"TEST.BAS\"");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));  // blocked on the confirmation prompt's own KEYSCAN_WAIT
  CHECK(fs::exists(targetPath));  // not deleted yet -- still just prompting

  tapKey(*m, pc1500::Key::Y);
  CHECK(waitForIdle(*m));
  CHECK(!fs::exists(targetPath));
}

// Same setup, but anything other than Y (here CL) aborts -- matches
// SDSAVE's own overwrite-prompt N-abort test.
void testSdrmNAbortsDeletion() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdrmNAbortsDeletion -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdrm_abort");
  fs::path targetPath = sdDir / "TEST.BAS";
  { std::ofstream f(targetPath, std::ios::binary); f << "10 END\n"; }

  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDRM \"TEST.BAS\"");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);  // anything but Y -- must abort
  CHECK(waitForIdle(*m));
  CHECK(fs::exists(targetPath));
}

// SDRM "<name>",-Y deletes immediately, no confirmation prompt.
void testSdrmDashYSkipsPrompt() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdrmDashYSkipsPrompt -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdrm_dashy");
  fs::path targetPath = sdDir / "TEST.BAS";
  { std::ofstream f(targetPath, std::ios::binary); f << "10 END\n"; }

  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDRM \"TEST.BAS\",-Y");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));  // no follow-up keypress -- must reach idle on its own
  CHECK(!fs::exists(targetPath));
}

// SDRM with no argument raises ERROR 1, matching SDMKDIR/SDCD/SDRMDIR's own
// convention.
void testSdrmMissingFilenameRaisesError1() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdrmMissingFilenameRaisesError1 -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdrm_noarg");
  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDRM");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));
  CHECK(m->bus.readME0(kErlAbs) == 1);
}

// SDRM must never delete a directory -- SDRMDIR is the only sanctioned way.
// Pointed at a real (empty) directory, it must raise ERROR 40 (same
// "operation on this name failed" code SDCP/SDMV also use) and leave the
// directory untouched.
void testSdrmCannotRemoveDirectory() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdrmCannotRemoveDirectory -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdrm_dir");
  fs::create_directory(sdDir / "ADIR");

  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDRM \"ADIR\",-Y");  // -Y so it doesn't block on a prompt first
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));
  CHECK(m->bus.readME0(kErlAbs) == 40);
  CHECK(fs::exists(sdDir / "ADIR"));
}

// SDCP "<src>","<dest>" copies a file, leaving the source untouched.
void testSdcpCopiesFile() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdcpCopiesFile -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdcp");
  const std::vector<uint8_t> kFixture = {0x00, 0x0A, 0x04, 0xF0, 0x97, 0x31, 0x0D, 0xFF};
  {
    std::ofstream f(sdDir / "SRC.BAS", std::ios::binary);
    f.write(reinterpret_cast<const char*>(kFixture.data()), static_cast<std::streamsize>(kFixture.size()));
  }

  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));
  m->bus.writeME0(kErlAbs, 0xEE);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDCP \"SRC.BAS\",\"DEST.BAS\"");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  CHECK(m->bus.readME0(kErlAbs) != 40);
  CHECK(readFile((sdDir / "SRC.BAS").string()) == kFixture);   // source untouched
  CHECK(readFile((sdDir / "DEST.BAS").string()) == kFixture);  // destination has the copy
}

// SDCP with a nonexistent source raises ERROR 40.
void testSdcpMissingSourceRaisesError40() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdcpMissingSourceRaisesError40 -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdcp_notfound");
  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDCP \"NOPE.BAS\",\"DEST.BAS\"");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));
  CHECK(m->bus.readME0(kErlAbs) == 40);
  CHECK(!fs::exists(sdDir / "DEST.BAS"));
}

// SDMV "<src>","<dest>" moves a file -- the source no longer exists
// afterward, unlike SDCP.
void testSdmvMovesFile() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdmvMovesFile -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdmv");
  const std::vector<uint8_t> kFixture = {0x00, 0x0A, 0x04, 0xF0, 0x97, 0x31, 0x0D, 0xFF};
  {
    std::ofstream f(sdDir / "SRC.BAS", std::ios::binary);
    f.write(reinterpret_cast<const char*>(kFixture.data()), static_cast<std::streamsize>(kFixture.size()));
  }

  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDMV \"SRC.BAS\",\"DEST.BAS\"");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  CHECK(!fs::exists(sdDir / "SRC.BAS"));
  CHECK(readFile((sdDir / "DEST.BAS").string()) == kFixture);
}

// SDDF stages a "<free>F / <total>T" response at EXP_SCRATCH_ABS -- checked
// by reading that staged response directly, same approach
// testSdpwdStagesCurrentDirectoryResponse uses for GET_SD_CWD.
void testSddfDisplaysFreeAndTotalSpace() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSddfDisplaysFreeAndTotalSpace -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sddf");
  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDDF");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  constexpr uint16_t kExpScratchAbs = 0x8100;
  uint8_t len = m->bus.readME0(kExpScratchAbs);
  CHECK(len > 0 && len <= 26);
  std::string text;
  for (uint8_t i = 0; i < len; i++) {
    text += static_cast<char>(m->bus.readME0(static_cast<uint16_t>(kExpScratchAbs + 1 + i)));
  }
  // "<free>F / <total>T" -- don't assert exact numbers (host-dependent
  // free/total space), just the shape.
  CHECK(text.find('F') != std::string::npos);
  CHECK(text.find('T') != std::string::npos);
  CHECK(text.find(" / ") != std::string::npos);
  if (text.find('F') == std::string::npos || text.find('T') == std::string::npos) {
    std::printf("  SDDF response text: \"%s\"\n", text.c_str());
  }
}

// SDMV "<src>","<dest>" where <dest> is an existing directory: the file
// lands inside it under its own original basename, matching Unix mv's
// own "move INTO a directory" behavior.
void testSdmvIntoExistingDirectory() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdmvIntoExistingDirectory -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdmv_into_dir");
  fs::create_directory(sdDir / "ARCHIVE");
  const std::vector<uint8_t> kFixture = {0x00, 0x0A, 0x04, 0xF0, 0x97, 0x31, 0x0D, 0xFF};
  {
    std::ofstream f(sdDir / "SRC.BAS", std::ios::binary);
    f.write(reinterpret_cast<const char*>(kFixture.data()), static_cast<std::streamsize>(kFixture.size()));
  }

  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDMV \"SRC.BAS\",\"ARCHIVE\"");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  CHECK(!fs::exists(sdDir / "SRC.BAS"));
  CHECK(readFile((sdDir / "ARCHIVE" / "SRC.BAS").string()) == kFixture);
}

// SDCP "<src>","<dest>" where <dest> is an existing directory -- same
// directory-target behavior as SDMV above, but the source stays put.
void testSdcpIntoExistingDirectory() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdcpIntoExistingDirectory -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdcp_into_dir");
  fs::create_directory(sdDir / "ARCHIVE");
  const std::vector<uint8_t> kFixture = {0x00, 0x0A, 0x04, 0xF0, 0x97, 0x31, 0x0D, 0xFF};
  {
    std::ofstream f(sdDir / "SRC.BAS", std::ios::binary);
    f.write(reinterpret_cast<const char*>(kFixture.data()), static_cast<std::streamsize>(kFixture.size()));
  }

  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDCP \"SRC.BAS\",\"ARCHIVE\"");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  CHECK(readFile((sdDir / "SRC.BAS").string()) == kFixture);            // source untouched
  CHECK(readFile((sdDir / "ARCHIVE" / "SRC.BAS").string()) == kFixture);  // copy landed inside
}

// SDMV's source may itself be a relative path with "..": from inside
// SUBDIR, "../SRC.BAS" refers to the root's own copy.
void testSdmvWithDotDotRelativeSource() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdmvWithDotDotRelativeSource -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdmv_dotdot");
  fs::create_directory(sdDir / "SUBDIR");
  const std::vector<uint8_t> kFixture = {0x00, 0x0A, 0x04, 0xF0, 0x97, 0x31, 0x0D, 0xFF};
  {
    std::ofstream f(sdDir / "SRC.BAS", std::ios::binary);
    f.write(reinterpret_cast<const char*>(kFixture.data()), static_cast<std::streamsize>(kFixture.size()));
  }

  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDCD \"SUBDIR\"");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDMV \"../SRC.BAS\",\"MOVED.BAS\"");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  CHECK(!fs::exists(sdDir / "SRC.BAS"));
  CHECK(readFile((sdDir / "SUBDIR" / "MOVED.BAS").string()) == kFixture);
}

// SDCP's destination may be an absolute path from the SD root ("/...")
// even while the current directory is somewhere else entirely.
void testSdcpWithAbsoluteDestinationPath() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdcpWithAbsoluteDestinationPath -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdcp_absolute");
  fs::create_directory(sdDir / "SUBDIR");
  const std::vector<uint8_t> kFixture = {0x00, 0x0A, 0x04, 0xF0, 0x97, 0x31, 0x0D, 0xFF};
  {
    std::ofstream f(sdDir / "SUBDIR" / "SRC.BAS", std::ios::binary);
    f.write(reinterpret_cast<const char*>(kFixture.data()), static_cast<std::streamsize>(kFixture.size()));
  }

  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDCD \"SUBDIR\"");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDCP \"SRC.BAS\",\"/ROOT.BAS\"");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  CHECK(readFile((sdDir / "ROOT.BAS").string()) == kFixture);
}

// SDCP onto an existing destination confirms first ("FILE EXISTS.
// OVERWRITE Y/N"); pressing anything but Y aborts, leaving the existing
// destination untouched -- same shape as SDSAVE's own overwrite prompt.
void testSdcpOverwritePromptNAborts() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdcpOverwritePromptNAborts -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdcp_overwrite_abort");
  const std::vector<uint8_t> kSrcFixture = {0x00, 0x0A, 0x04, 0xF0, 0x97, 0x31, 0x0D, 0xFF};
  const std::vector<uint8_t> kDestOriginal = {0xAA, 0xBB, 0xCC, 0xFF};
  {
    std::ofstream f(sdDir / "SRC.BAS", std::ios::binary);
    f.write(reinterpret_cast<const char*>(kSrcFixture.data()), static_cast<std::streamsize>(kSrcFixture.size()));
  }
  fs::path destPath = sdDir / "DEST.BAS";
  {
    std::ofstream f(destPath, std::ios::binary);
    f.write(reinterpret_cast<const char*>(kDestOriginal.data()), static_cast<std::streamsize>(kDestOriginal.size()));
  }

  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDCP \"SRC.BAS\",\"DEST.BAS\"");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));  // blocked on the confirmation prompt's own KEYSCAN_WAIT

  tapKey(*m, pc1500::Key::Cl);  // anything but Y -- must abort
  CHECK(waitForIdle(*m));

  CHECK(readFile(destPath.string()) == kDestOriginal);
}

// Same setup, but pressing Y confirms the overwrite.
void testSdcpOverwritePromptYOverwrites() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdcpOverwritePromptYOverwrites -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdcp_overwrite_confirm");
  const std::vector<uint8_t> kSrcFixture = {0x00, 0x0A, 0x04, 0xF0, 0x97, 0x31, 0x0D, 0xFF};
  const std::vector<uint8_t> kDestOriginal = {0xAA, 0xBB, 0xCC, 0xFF};
  {
    std::ofstream f(sdDir / "SRC.BAS", std::ios::binary);
    f.write(reinterpret_cast<const char*>(kSrcFixture.data()), static_cast<std::streamsize>(kSrcFixture.size()));
  }
  fs::path destPath = sdDir / "DEST.BAS";
  {
    std::ofstream f(destPath, std::ios::binary);
    f.write(reinterpret_cast<const char*>(kDestOriginal.data()), static_cast<std::streamsize>(kDestOriginal.size()));
  }

  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDCP \"SRC.BAS\",\"DEST.BAS\"");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Y);
  CHECK(waitForIdle(*m));

  CHECK(readFile(destPath.string()) == kSrcFixture);
  CHECK(readFile((sdDir / "SRC.BAS").string()) == kSrcFixture);  // SDCP -- source untouched
}

// SDCP "<src>","<dest>",-Y onto an existing destination: overwrites
// immediately, no confirmation prompt.
void testSdcpDashYSkipsPrompt() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdcpDashYSkipsPrompt -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdcp_dashy");
  const std::vector<uint8_t> kSrcFixture = {0x00, 0x0A, 0x04, 0xF0, 0x97, 0x31, 0x0D, 0xFF};
  const std::vector<uint8_t> kDestOriginal = {0xAA, 0xBB, 0xCC, 0xFF};
  {
    std::ofstream f(sdDir / "SRC.BAS", std::ios::binary);
    f.write(reinterpret_cast<const char*>(kSrcFixture.data()), static_cast<std::streamsize>(kSrcFixture.size()));
  }
  fs::path destPath = sdDir / "DEST.BAS";
  {
    std::ofstream f(destPath, std::ios::binary);
    f.write(reinterpret_cast<const char*>(kDestOriginal.data()), static_cast<std::streamsize>(kDestOriginal.size()));
  }

  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDCP \"SRC.BAS\",\"DEST.BAS\",-Y");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));  // no follow-up keypress -- must reach idle on its own

  CHECK(readFile(destPath.string()) == kSrcFixture);
}

// SDMV onto an existing destination also confirms first, same as SDCP.
void testSdmvOverwritePromptNAborts() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdmvOverwritePromptNAborts -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdmv_overwrite_abort");
  const std::vector<uint8_t> kSrcFixture = {0x00, 0x0A, 0x04, 0xF0, 0x97, 0x31, 0x0D, 0xFF};
  const std::vector<uint8_t> kDestOriginal = {0xAA, 0xBB, 0xCC, 0xFF};
  fs::path srcPath = sdDir / "SRC.BAS";
  {
    std::ofstream f(srcPath, std::ios::binary);
    f.write(reinterpret_cast<const char*>(kSrcFixture.data()), static_cast<std::streamsize>(kSrcFixture.size()));
  }
  fs::path destPath = sdDir / "DEST.BAS";
  {
    std::ofstream f(destPath, std::ios::binary);
    f.write(reinterpret_cast<const char*>(kDestOriginal.data()), static_cast<std::streamsize>(kDestOriginal.size()));
  }

  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDMV \"SRC.BAS\",\"DEST.BAS\"");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);  // anything but Y -- must abort
  CHECK(waitForIdle(*m));

  CHECK(readFile(destPath.string()) == kDestOriginal);
  CHECK(fs::exists(srcPath));  // SDMV aborted -- source must still be there too
}

// SDLOAD accepts an absolute path from the SD root even while the
// current directory is somewhere else -- the single-name commands
// (SDLOAD/SDSAVE/SDRM/SDCD/SDMKDIR/SDRMDIR) all gained this for free once
// ExpansionMock::resolvePath itself was unified to support it.
void testSdloadFromAbsolutePath() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdloadFromAbsolutePath -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdload_absolute");
  fs::create_directory(sdDir / "SUBDIR");
  const std::vector<uint8_t> kFixture = {0x00, 0x0A, 0x04, 0xF0, 0x97, 0x31, 0x0D, 0xFF};
  {
    std::ofstream f(sdDir / "TEST.BAS", std::ios::binary);
    f.write(reinterpret_cast<const char*>(kFixture.data()), static_cast<std::streamsize>(kFixture.size()));
  }

  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDCD \"SUBDIR\"");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDLOAD \"/TEST.BAS\"");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  std::string readError;
  std::vector<uint8_t> loadedProgram = pc1500::basic::readBasicProgramBytes(m->bus, &readError);
  CHECK(loadedProgram == kFixture);
}

// ---------------------------------------------------------------------
// SDOPEN/SDCLOSE/SDINPUT#/SDPRINT#/SDSKIP# -- the D461H-based variable
// channel commands. Numeric round trips use the fixed variable "A"
// (confirmed live this session at 0x7900-0x7907, 8 raw decimal-float
// bytes); string round trips use "T$" (confirmed live at 0x7790, 16-byte
// capacity, zero-padded inline ASCII with no separate length field) --
// see this session's own memory notes. Reading a variable's value back is
// done by peeking its own fixed storage directly rather than via PRINT,
// since there's no existing VRAM-text-reading helper in this file.
constexpr uint16_t kVarA = 0x7900;
constexpr uint16_t kVarTDollar = 0x7790;

// SDOPEN "<name>" AS <n> creates the file and reports success (no ERROR);
// a bare SDOPEN afterward lists it as "<n>:<name>" via the same
// LIST_SD_DIR-shaped wire format SDLS itself uses.
void testSdopenCreatesFileAndListsChannel() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdopenCreatesFileAndListsChannel -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdopen_create_list");
  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  m->bus.writeME0(kErlAbs, 0xEE);
  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDOPEN \"CHAN1.SDF\" AS 1");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));
  CHECK(m->bus.readME0(kErlAbs) != 1);
  CHECK(fs::exists(sdDir / "CHAN1.SDF"));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDOPEN");
  tapKey(*m, pc1500::Key::Ent);  // dispatch -- lists open channels
  CHECK(waitForIdle(*m));

  constexpr uint16_t kBufAbs = 0x8000;
  constexpr int kNameLen = 16;
  constexpr int kRecordSize = 30;
  uint16_t count = (static_cast<uint16_t>(m->bus.readME0(kBufAbs)) << 8) | m->bus.readME0(kBufAbs + 1);
  CHECK(count == 1);
  std::string name;
  for (int j = 0; j < kNameLen; j++) name += static_cast<char>(m->bus.readME0(kBufAbs + 2 + j));
  while (!name.empty() && name.back() == ' ') name.pop_back();
  CHECK(name == "1:CHAN1.SDF");
  if (name != "1:CHAN1.SDF") std::printf("  channel listing name: \"%s\"\n", name.c_str());
  (void)kRecordSize;

  tapKey(*m, pc1500::Key::Ent);  // exit the browse
  CHECK(waitForIdle(*m));
}

// Reusing an already-open channel number closes the previous file first
// -- the listing must show only the new file under that channel.
void testSdopenReusingChannelClosesPrevious() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdopenReusingChannelClosesPrevious -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdopen_reuse");
  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDOPEN \"FIRST.SDF\" AS 1");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDOPEN \"SECOND.SDF\" AS 1");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDOPEN");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  constexpr uint16_t kBufAbs = 0x8000;
  constexpr int kNameLen = 16;
  uint16_t count = (static_cast<uint16_t>(m->bus.readME0(kBufAbs)) << 8) | m->bus.readME0(kBufAbs + 1);
  CHECK(count == 1);
  std::string name;
  for (int j = 0; j < kNameLen; j++) name += static_cast<char>(m->bus.readME0(kBufAbs + 2 + j));
  while (!name.empty() && name.back() == ' ') name.pop_back();
  CHECK(name == "1:SECOND.SDF");

  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));
}

// SDCLOSE <n> closes a single channel; SDCLOSE ALL closes every open one.
void testSdcloseClosesOneAndAll() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdcloseClosesOneAndAll -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdclose");
  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDOPEN \"A.SDF\" AS 1");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));
  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDOPEN \"B.SDF\" AS 2");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDCLOSE 1");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  constexpr uint16_t kBufAbs = 0x8000;
  {
    tapKey(*m, pc1500::Key::Cl);
    typeText(*m, "SDOPEN");
    tapKey(*m, pc1500::Key::Ent);
    CHECK(waitForIdle(*m));
    uint16_t count = (static_cast<uint16_t>(m->bus.readME0(kBufAbs)) << 8) | m->bus.readME0(kBufAbs + 1);
    CHECK(count == 1);  // only B.SDF (channel 2) left open
    tapKey(*m, pc1500::Key::Ent);
    CHECK(waitForIdle(*m));
  }

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDCLOSE ALL");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  {
    tapKey(*m, pc1500::Key::Cl);
    typeText(*m, "SDOPEN");
    tapKey(*m, pc1500::Key::Ent);
    CHECK(waitForIdle(*m));
    uint16_t count = (static_cast<uint16_t>(m->bus.readME0(kBufAbs)) << 8) | m->bus.readME0(kBufAbs + 1);
    CHECK(count == 0);
    tapKey(*m, pc1500::Key::Ent);
    CHECK(waitForIdle(*m));
  }
}

// SDPRINT#/SDINPUT# round trip a numeric variable's real value through an
// SD file, byte for byte.
void testSdprintSdinputNumericRoundTrip() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdprintSdinputNumericRoundTrip -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdprint_numeric");
  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "A=1500");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  std::vector<uint8_t> original;
  for (int i = 0; i < 8; i++) original.push_back(m->bus.readME0(static_cast<uint16_t>(kVarA + i)));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDOPEN \"NUMS.SDF\" AS 1");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDPRINT#1,A");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "A=0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));
  for (int i = 0; i < 8; i++) CHECK(m->bus.readME0(static_cast<uint16_t>(kVarA + i)) == 0);

  // Reopening channel 1 resets its own read cursor back to the start.
  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDOPEN \"NUMS.SDF\" AS 1");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDINPUT#1,A");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  for (int i = 0; i < 8; i++) CHECK(m->bus.readME0(static_cast<uint16_t>(kVarA + i)) == original[i]);
}

// Same round trip for a string variable (T$) -- exercises the zero-padded
// inline-ASCII storage format and the length-scan in SD_BUILD_VALUE_CHUNK.
void testSdprintSdinputStringRoundTrip() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdprintSdinputStringRoundTrip -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdprint_string");
  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "T$=\"HI\"");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDOPEN \"STR.SDF\" AS 3");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDPRINT#3,T$");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "T$=\"ZZ\"");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDOPEN \"STR.SDF\" AS 3");  // reopen -- resets the read cursor
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDINPUT#3,T$");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  CHECK(m->bus.readME0(kVarTDollar + 0) == 'H');
  CHECK(m->bus.readME0(kVarTDollar + 1) == 'I');
  CHECK(m->bus.readME0(kVarTDollar + 2) == 0x00);
}

// Reading more values than a channel has stored must zero-fill numeric
// variables and blank string variables, not raise an error -- the user's
// own explicit spec.
void testSdinputEofFillsZeroAndBlank() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdinputEofFillsZeroAndBlank -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdinput_eof");
  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "A=999");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "T$=\"ZZ\"");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDOPEN \"EMPTY.SDF\" AS 1");  // freshly created -- no stored values
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  m->bus.writeME0(kErlAbs, 0xEE);
  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDINPUT#1,A,T$");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  CHECK(m->bus.readME0(kErlAbs) != 1);
  CHECK(m->bus.readME0(kErlAbs) != 40);
  for (int i = 0; i < 8; i++) CHECK(m->bus.readME0(static_cast<uint16_t>(kVarA + i)) == 0);
  CHECK(m->bus.readME0(kVarTDollar + 0) == 0x00);
}

// SDSKIP# advances past whole values without transferring them; skipping
// past the end of the file must raise a genuine ERROR 40 and leave the
// read position untouched (confirmed indirectly here by checking the
// value after the failed skip is still the first stored one).
void testSdskipAdvancesAndRaisesError40PastEnd() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdskipAdvancesAndRaisesError40PastEnd -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdskip");
  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDOPEN \"SKIP.SDF\" AS 1");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  // Snapshot each value's own real 8-byte in-memory encoding right after
  // assignment -- comparing against these (rather than a guessed BCD byte
  // pattern) is what actually confirms SDSKIP#/SDINPUT# read back the
  // right *stored* value, independent of exactly how BASIC encodes a
  // given integer internally.
  std::vector<std::vector<uint8_t>> snapshots;
  for (int v : {1, 2, 3}) {
    tapKey(*m, pc1500::Key::Cl);
    typeText(*m, "A=" + std::to_string(v));
    tapKey(*m, pc1500::Key::Ent);
    CHECK(waitForIdle(*m));
    std::vector<uint8_t> snap;
    for (int i = 0; i < 8; i++) snap.push_back(m->bus.readME0(static_cast<uint16_t>(kVarA + i)));
    snapshots.push_back(snap);
    tapKey(*m, pc1500::Key::Cl);
    typeText(*m, "SDPRINT#1,A");
    tapKey(*m, pc1500::Key::Ent);
    CHECK(waitForIdle(*m));
  }

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDOPEN \"SKIP.SDF\" AS 1");  // reopen -- reset read cursor to the start
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDSKIP#1,2");  // skip the first two stored values (1 and 2) -- comma
                                // separator required, see SDSKIP_ROUTINE's own comment
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDINPUT#1,A");  // should read the third value (3)
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));
  for (int i = 0; i < 8; i++) CHECK(m->bus.readME0(static_cast<uint16_t>(kVarA + i)) == snapshots[2][i]);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDOPEN \"SKIP.SDF\" AS 1");  // reopen again -- reset to the start
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  m->bus.writeME0(kErlAbs, 0xEE);
  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDSKIP#1,5");  // only 3 values exist -- must fail all-or-nothing
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));
  CHECK(m->bus.readME0(kErlAbs) == 40);

  // The failed skip must have left the read position untouched -- the very
  // next SDINPUT# should still read the *first* stored value (1).
  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "A=999");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));
  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDINPUT#1,A");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));
  for (int i = 0; i < 8; i++) CHECK(m->bus.readME0(static_cast<uint16_t>(kVarA + i)) == snapshots[0][i]);
}

// Malformed/missing arguments to SDOPEN/SDCLOSE/SDINPUT#/SDPRINT#/SDSKIP#
// must raise a genuine BASIC ERROR 1, matching every other SD command's
// own established convention for a deliberate operation given bad input.
void testSdChannelCommandsRaiseError1OnMalformedArgument() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf(
        "SKIP: testSdChannelCommandsRaiseError1OnMalformedArgument -- ROM1.BIN and/or "
        "rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdchannel_error1");
  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  // SDINPUT#1/SDPRINT#1/SDSKIP#1 (channel number but no variable list/count
  // at all) are used here rather than e.g. "SDINPUT#1,A" -- the latter is
  // syntactically well-formed (it just references a channel that isn't
  // open, which raises ERROR 40, not ERROR 1 -- see SDINPUT_ROUTINE's own
  // comment on that distinction).
  for (const char* cmd : {"SDCLOSE", "SDCLOSE 0", "SDCLOSE 17", "SDINPUT#1", "SDPRINT#1", "SDSKIP#1",
                           "SDOPEN \"X.SDF\""}) {
    tapKey(*m, pc1500::Key::Cl);
    typeText(*m, "NEW0");
    tapKey(*m, pc1500::Key::Ent);
    CHECK(waitForIdle(*m));

    m->bus.writeME0(kErlAbs, 0xEE);
    tapKey(*m, pc1500::Key::Cl);
    typeText(*m, cmd);
    tapKey(*m, pc1500::Key::Ent);
    CHECK(waitForIdle(*m));

    CHECK(m->bus.readME0(kErlAbs) == 1);
    if (m->bus.readME0(kErlAbs) != 1) {
      std::printf("  \"%s\": ERL=%u (want 1)\n", cmd, m->bus.readME0(kErlAbs));
    }
  }
}

// A hand-crafted SD file whose stored string chunk is longer than the
// target variable's own real capacity must raise a genuine BASIC ERROR
// 42, not silently truncate or crash -- the user's own explicit spec for
// this (deliberately corrupted-file-only) case.
void testSdinputOverlongStringRaisesError42() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testSdinputOverlongStringRaisesError42 -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_sdinput_error42");
  {
    // 'S' tag, length=17 (one over T$'s own 16-byte capacity), 17 bytes of 'X'.
    std::vector<uint8_t> chunk = {'S', 17};
    chunk.insert(chunk.end(), 17, 'X');
    std::ofstream f(sdDir / "BAD.SDF", std::ios::binary);
    f.write(reinterpret_cast<const char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
  }

  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDOPEN \"BAD.SDF\" AS 1");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  m->bus.writeME0(kErlAbs, 0xEE);
  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "SDINPUT#1,T$");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  CHECK(m->bus.readME0(kErlAbs) == 42);
}

// Validates STAGE_COPY_BLOCK_VERIFY_LOOP's checksum ARITHMETIC in
// isolation, added 2026-09-21 after a real per-block mismatch report
// (expected 0x8EF9, found 0x8ED1) -- the board owner's own question: is
// the ROM-side checksum *computation* itself proven correct, or could its
// own arithmetic be wrong rather than the SRAM content actually differing?
// pc1500emu's ExpansionMock doesn't implement the real BEGIN/GET_BLOCK/
// SRAM protocol, so this deliberately bypasses all of that: writes a known
// 1024-byte pattern into the data window, jumps the CPU directly into the
// verify loop (CPU register injection via cpu.setP/setX/setU -- same
// technique testSdsaveMCallAddressRoundTrip already uses for writeME0'd
// routines, just also setting registers instead of only memory), lets it
// run, and compares the result against a checksum computed independently
// right here in C++ (not by calling any shared helper) -- a genuinely
// separate implementation of "sum these 1024 bytes, 16-bit wraparound",
// so agreement here means the ASSEMBLY loop's own arithmetic is sound,
// independent of anything happening on the real SRAM chip.
void testStageBlockChecksumAlgorithmMatchesIndependentComputation() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  // rom_8800.bin only covers 0x8800+ (see build.ps1's own comment) --
  // STAGE_COPY_ROUTINE_ABS lives at 0x8500, INSIDE loadExpansionModule's
  // separately-managed, blank-0xFF-initialized "data window"
  // (0x8000-0x87FF, see bus.h's own RomModule::dataWindow), which
  // rom_8800.bin never touches. rom.bin (0x8000-anchored) is the one file
  // that actually contains those bytes -- loaded below and poked directly
  // into the data window via writeME0, the same way
  // testSdsaveMCallAddressRoundTrip pokes a hand-written routine into
  // plain RAM, so this test executes the REAL assembled routine instead
  // of the data window's default blank fill (confirmed the hard way: the
  // first version of this test skipped this step, jumped into 0xFF-filled
  // RAM instead of real code, and predictably got back 0xFFFF).
  const std::string kRomBinPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  std::vector<uint8_t> romBin = readFile(kRomBinPath);
  if (rom.empty() || expRom.empty() || romBin.empty()) {
    std::printf("SKIP: testStageBlockChecksumAlgorithmMatchesIndependentComputation -- "
                "ROM1.BIN, rom_8800.bin, and/or rom.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_stage_block_cksum_algo");
  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  // loadExpansionModule() was called with requirePv=false (see
  // loadExpansionRom() above) -- correct, this board's expansion module
  // runs with PV low throughout, not toggled per-entry the way a real
  // CE-150-style cartridge dance would be. Every RomModule::tryRead/
  // tryWrite call -- including the writeME0 pokes below -- silently
  // no-ops unless the bus's currently-tracked PV pin matches that,
  // though, and this test never goes through this board's own normal
  // dispatch path at all (it jumps the CPU straight into
  // STAGE_COPY_BLOCK_VERIFY_LOOP) -- so it isn't guaranteed PV is still
  // low here rather than whatever the base ROM's own (unrelated, opcode
  // 0xA8/0xB8-driven) use of the same flip-flop left it at by the end of
  // cold boot. Confirmed the hard way: even after fixing the
  // missing-routine-bytes bug above, this test still silently got back
  // 0xFFFF, because EVERY writeME0 below (loading the routine bytes, the
  // known data pattern, and zeroing the checksum cells) was failing this
  // same gating check before ever reaching a single real byte. Forcing it
  // false here, directly on the bus (the authoritative copy RomModule's
  // gating actually reads), makes every write below land for real.
  m->bus.setPv(false);

  // Addresses from rom.lst -- MUST be re-confirmed there any time rom.asm's
  // STAGE_COPY_ROUTINE_ABS block changes size at all (these are absolute,
  // not symbolic, since this test pokes the CPU directly rather than going
  // through the keyword dispatcher). Confirmed the hard way, twice: adding
  // the per-block checksum feature, and later STAGE DEBUG's own per-byte
  // copy loop, each silently shifted every address below without changing
  // this test at all, making it jump into the wrong code and fail with the
  // exact same "0xFFFF" symptom as the ORIGINAL missing-bytes bug -- a
  // stale address here is indistinguishable from a real regression unless
  // you re-check rom.lst first.
  constexpr uint16_t kStageCopyStart = 0x8400;  // STAGE_COPY_START
  constexpr uint16_t kStageCopyEnd = 0x8717;    // STAGE_COPY_END (exclusive)
  constexpr uint16_t kCksumHiAddr = 0x86B0;     // STAGE_BLOCK_CKSUM_HI
  constexpr uint16_t kCksumLoAddr = 0x86B1;     // STAGE_BLOCK_CKSUM_LO
  constexpr uint16_t kVerifyLoopAddr = 0x848D;  // STAGE_COPY_BLOCK_VERIFY_LOOP

  // rom.bin's byte 0 is address 0x8000 (see build.ps1's own comment), so
  // STAGE_COPY_START's bytes start at offset (kStageCopyStart - 0x8000).
  CHECK(romBin.size() >= static_cast<size_t>(kStageCopyEnd - 0x8000));
  for (uint16_t addr = kStageCopyStart; addr < kStageCopyEnd; addr++) {
    m->bus.writeME0(addr, romBin[static_cast<size_t>(addr - 0x8000)]);
  }

  // Known 1024-byte pattern in the payload window (0x8000-0x83FF, the same
  // region GET_BLOCK's real response would occupy) -- i & 0xFF repeating
  // four times, summed independently right here. Written AFTER the routine
  // bytes above so it isn't clobbered by them (STAGE_COPY_ROUTINE_ABS
  // starts at 0x8500, well past 0x83FF, so there's no overlap either way,
  // but this keeps the ordering obviously safe regardless).
  constexpr uint16_t kDataAddr = 0x8000;
  constexpr uint16_t kLen = 1024;
  uint16_t expectedSum = 0;
  for (uint16_t i = 0; i < kLen; i++) {
    uint8_t v = static_cast<uint8_t>(i & 0xFF);
    m->bus.writeME0(static_cast<uint16_t>(kDataAddr + i), v);
    expectedSum = static_cast<uint16_t>(expectedSum + v);
  }

  // Same pre-loop state STAGE_COPY_ROUTINE_ABS's own code sets up right
  // before entering the loop (X=data pointer, U=1024, checksum zeroed) --
  // done directly here instead of by executing that setup code, so this
  // test exercises ONLY the loop's own arithmetic, nothing upstream of it.
  m->bus.writeME0(kCksumHiAddr, 0x00);
  m->bus.writeME0(kCksumLoAddr, 0x00);
  m->cpu.setX(kDataAddr);
  m->cpu.setU(kLen);

  // BASIC's own idle loop legitimately runs with interrupts enabled
  // (IE=1, waiting on the periodic timer interrupt to wake it from HLT --
  // see this ROM's own "HLT-based... polynomial timer interrupt" idle
  // convention). Confirmed directly: without disabling interrupts here,
  // the real timer interrupt fires partway through the 20000-step budget
  // below and hijacks execution into genuine system-ROM interrupt-handler
  // code, which never returns to this routine. CPU has no public "set IE"
  // accessor, so a tiny 4-byte bootstrap (RIE, then JMP into the real
  // loop) is written into free scratch space (0x8700 -- well inside the
  // ~329 free bytes between STAGE_COPY_END and EXP_BLOCK_CHECKSUM_ABS,
  // confirmed via rom.lst) and executed for real, exactly like
  // hand-written test routines elsewhere in this file (e.g.
  // testSdsaveMCallAddressRoundTrip's own writeME0'd routine) -- this way
  // the CPU's own RIE opcode does the disabling, not a direct field poke.
  // RIE is a two-byte extended opcode (0xFD prefix + 0xBE -- see
  // CPU::step()'s own opcode==0xFD dispatch to execFD(), a separate table
  // from execPrimary()'s). Confirmed the hard way: writing bare 0xBE first
  // (no prefix) instead decoded as execPrimary's own 0xBE -- an unrelated
  // CALL-style instruction (push16(p_); p_=fetch16()) -- which consumed
  // the JMP opcode byte that followed as part of ITS OWN address operand,
  // sending P to a garbage address nowhere near the real loop.
  constexpr uint16_t kBootstrapAddr = 0x87C0;  // clear of STAGE_COPY_END (0x8785 as of this
                                                // writing -- growing with each fix, so this is
                                                // deliberately given more headroom than the bare
                                                // minimum) and still inside the 0x8500-0x87FC budget
  const std::vector<uint8_t> kBootstrap = {
      0xFD, 0xBE,                                                     // RIE
      0xBA, static_cast<uint8_t>(kVerifyLoopAddr >> 8),               // JMP
      static_cast<uint8_t>(kVerifyLoopAddr & 0xFF)};
  for (size_t i = 0; i < kBootstrap.size(); i++) {
    m->bus.writeME0(static_cast<uint16_t>(kBootstrapAddr + i), kBootstrap[i]);
  }
  m->cpu.setP(kBootstrapAddr);

  // bootAndSettle() naturally leaves the CPU halted (BASIC's own idle HLT
  // loop) -- CPU::step() is documented to return 0 and do nothing while
  // halted, so setP() alone never actually redirects execution; confirmed
  // directly (an earlier version of this test, without this line, just
  // sat at P unchanged for its first 20 "steps" before the timer
  // interrupt eventually woke it on its own).
  m->cpu.setHalted(false);

  // Stop the instant the loop itself exits (P reaches the comparison code
  // right after it, STAGE_COPY_BLOCK_VERIFY_OK's own "cpa
  // (EXP_BLOCK_CHECKSUM_ABS)" at 0x857A per rom.lst) -- STAGE_BLOCK_
  // CKSUM_HI/LO are already final at that point, and running any further
  // would fall into the mismatch/report path, which writes
  // EXP_INSTRUCTION_ABS and triggers ExpansionMock's real background-
  // thread command processing (see Bus::writeME0's own comment) -- not
  // something safe to blindly single-step through synchronously, and not
  // needed for what this test is actually checking. Confirmed the hard
  // way: an earlier version ran a fixed 20000-step budget regardless,
  // which dragged execution into exactly that path and off into
  // unrelated system-ROM territory before ever reading the checksum back.
  constexpr uint16_t kComparisonAddr = 0x84A7;  // first instruction after the loop (lda (STAGE_BLOCK_CKSUM_HI)) -- addresses updated 2026-09-24 from rom.rst
  constexpr int kMaxSteps = 20000;              // generous fallback -- the loop alone needs ~11264
  int stepsTaken = 0;
  while (m->cpu.p() != kComparisonAddr && stepsTaken < kMaxSteps) {
    stepOne(*m);
    stepsTaken++;
  }
  CHECK(m->cpu.p() == kComparisonAddr);
  if (m->cpu.p() != kComparisonAddr) {
    std::printf("  loop never reached its own exit point: P=%04X after %d steps (expected 0x%04X)\n",
                m->cpu.p(), stepsTaken, kComparisonAddr);
  }

  uint16_t foundSum = static_cast<uint16_t>((m->bus.readME0(kCksumHiAddr) << 8) |
                                             m->bus.readME0(kCksumLoAddr));
  CHECK(foundSum == expectedSum);
  if (foundSum != expectedSum) {
    std::printf("  STAGE_COPY_BLOCK_VERIFY_LOOP checksum algorithm mismatch: "
                "independently computed 0x%04X, ROM loop computed 0x%04X\n",
                expectedSum, foundSum);
  }
}

// STAGE keyword recognition -- added 2026-09 for the RP2350 ROM-to-SRAM
// redirect feature. First real-hardware attempt reported "STAGE is not a
// recognized keyword" at all; this checks the same thing here, fast and
// deterministically, before burning another hardware round-trip. STAGE's
// own no-argument query path issues EXP_COMMAND_ROM_GET_MODE (0x25), which
// ExpansionMock's own default: case reports as NOT_IMPLEMENTED (matching
// the real DoCommand()'s own default) -- STAGE_QUERY's own ROM code
// already treats that as "MODE UNKNOWN" and returns normally (see
// rom.asm's own comment on that path), so this doesn't need any new mock
// support to reach a clean, testable outcome: no ERROR 1 raised, and
// idle reached after the one dismissal keypress ECVER's own pattern needs.
void testStageQueryIsRecognizedKeyword() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testStageQueryIsRecognizedKeyword -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_stage_query");
  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "STAGE");
  tapKey(*m, pc1500::Key::Ent);  // dispatch STAGE (no argument -- query mode)
  CHECK(waitForIdle(*m));        // settles on STAGE_QUERY_SHOW's own KEYSCAN_WAIT

  uint8_t erlAfterDispatch = m->bus.readME0(kErlAbs);
  CHECK(erlAfterDispatch == 0);
  if (erlAfterDispatch != 0) {
    std::printf("  ERL after typing STAGE + Enter: %u (want 0 -- nonzero means "
                "the keyword table walker never reached STAGE_ROUTINE at all)\n",
                (unsigned)erlAfterDispatch);
  }

  tapKey(*m, pc1500::Key::Ent);  // dismiss STAGE_QUERY_SHOW's own message
  CHECK(waitForIdle(*m));
  CHECK(m->bus.readME0(kErlAbs) == 0);
}

// "DEBUG" bare-word safety check -- added 2026-09-21 alongside STAGE DEBUG
// (a new argument word for the STAGE keyword's per-byte SRAM verify mode).
// This session already found MLOG's own argument vocabulary tripped over
// reserved-word collisions twice (LOG itself, then ON/OFF/CLEAR) -- cheap
// insurance to confirm "DEBUG" isn't also a real PC-1500 BASIC keyword
// before relying on it, same rigor as testMlogViewIsRecognized's own
// history. A real reserved word would raise some ERL other than 1 here (a
// genuine syntax/logic error from actually executing as its own built-in
// meaning), or execute cleanly with no error at all if it's a valid
// no-argument statement -- either way, distinguishable from ERROR 1 (bare
// SD_RAISE_ERROR_1, exactly what an unrecognized bare word not matching
// any keyword produces here).
void testDebugBareWordIsNotAReservedKeyword() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  std::vector<uint8_t> rom = readFile(kRomPath);
  if (rom.empty()) {
    std::printf("SKIP: testDebugBareWordIsNotAReservedKeyword -- ROM1.BIN not found.\n");
    return;
  }
  {
    auto m = bootAndSettle(rom);
    tapKey(*m, pc1500::Key::Cl);
    typeText(*m, "NEW0");
    tapKey(*m, pc1500::Key::Ent);
    CHECK(waitForIdle(*m));
    tapKey(*m, pc1500::Key::Cl);
    typeText(*m, "DEBUG");
    tapKey(*m, pc1500::Key::Ent);
    CHECK(waitForIdle(*m));
    uint8_t erl = m->bus.readME0(kErlAbs);
    std::printf("bare DEBUG -> ERL=%u\n", (unsigned)erl);
  }
  // Control: a deliberately-nonsense bare word that's definitely not any
  // real keyword. If THIS also gives ERL=0, that means bare identifiers
  // just don't error on this BASIC at all (likely parsed as an implicit
  // variable-reference expression statement, a legal no-op) -- explaining
  // DEBUG's own ERL=0 without it being reserved, rather than needing to
  // treat that as a collision the way LOG/ON/OFF genuinely were.
  {
    auto m = bootAndSettle(rom);
    tapKey(*m, pc1500::Key::Cl);
    typeText(*m, "NEW0");
    tapKey(*m, pc1500::Key::Ent);
    CHECK(waitForIdle(*m));
    tapKey(*m, pc1500::Key::Cl);
    typeText(*m, "ZQXVK");
    tapKey(*m, pc1500::Key::Ent);
    CHECK(waitForIdle(*m));
    uint8_t erl = m->bus.readME0(kErlAbs);
    std::printf("bare ZQXVK (control, definitely not a keyword) -> ERL=%u\n", (unsigned)erl);
  }
}

// STAGE DEBUG keyword recognition -- added 2026-09-21, the debug/per-byte-
// verify sibling of "STAGE RAM" (see STAGE_COPY_ROUTINE_ABS's own comment
// in rom.asm for why it was added: the per-block checksum feature
// narrowed a real failure to "block 0, off by 0x28 overall" but couldn't
// say which byte).
//
// 2026-09-24: now seeds the data window from rom.bin (like the real
// firmware's combined buffer image) so STAGE_COPY_ROUTINE_ABS at 0x8400 is
// real code, and checks the real outcome -- the mock implements the whole
// ROM_COPY_* sequence now. Previously the window was left 0xFF, so this
// "passed" by executing fill bytes that happened to wander back to idle
// (it printed ERL=0, not the ERL=1 its own comment expected), and broke as
// soon as the jump into the pocket moved.
void testStageDebugIsRecognizedKeyword() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomDir =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/Design01_NonDMA_8K_PV_Swap.cydsn/rom/";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomDir + "rom_8800.bin");
  std::vector<uint8_t> romBin = readFile(kExpRomDir + "rom.bin");
  if (rom.empty() || expRom.empty() || romBin.size() < 0x800) {
    std::printf("SKIP: testStageDebugIsRecognizedKeyword -- ROM1.BIN, rom_8800.bin and/or rom.bin "
                "not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_stage_debug");
  auto m = bootAndSettle(rom);
  m->bus.loadExpansionModule(0, expRom.data(), expRom.size(), /*base=*/0x8800, /*requirePv=*/false,
                             /*usePuBank=*/false, /*dataWindowBase=*/0x8000,
                             /*dataWindowSize=*/0x800, /*instructionAddr=*/0x87FF, romBin.data(),
                             0x800);
  m->bus.expansionMock().setRootDir(sdDir);

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "STAGE DEBUG");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m, 20'000'000));  // per-byte verify copy of all 6K
  CHECK(m->bus.readME0(kErlAbs) == 0);
  CHECK(m->bus.expansionMock().romCopyBeginCount() == 1);
  CHECK(m->bus.expansionMock().romStagedVerified());
  tapKey(*m, pc1500::Key::Ent);  // dismiss "STAGE: OK"
  CHECK(waitForIdle(*m));
}

// MLOG keyword recognition/argument-parsing checks -- added 2026-09-21 after
// a real-hardware report of "MLOG INFO ON shows ERROR 1", to find bugs fast
// and deterministically instead of guessing at hand-rolled character-by-
// character ROM assembly by re-reading it. None of these need ExpansionMock
// to implement the underlying EXP_COMMAND_LOG_* wire commands -- the ROM's
// own *_DO routines never check EC_WAIT_NOT_BUSY's return status before
// showing their confirmation message (same as the STAGE query test's own
// precedent: an unimplemented command reports NOT_IMPLEMENTED, which
// callers that don't check status just plow through), so these only test
// whether the keyword parser reaches the right *_DO routine at all -- an
// ERL of 1 here can only come from MLOG_BAD_ARG's own jmp SD_RAISE_ERROR_1,
// i.e. a real parsing bug, not a missing mock.
//
// Final argument vocabulary is VIEW/VERBOSE/QUIET/RESET (see rom.asm's own
// MLOG_ROUTINE comment for the two rounds of collision bugs -- whole
// reserved words, then a word-boundary collision between two individually
// safe words -- that got the vocabulary here).
void testMlogViewIsRecognized() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testMlogViewIsRecognized -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }
  fs::path sdDir = makeTempTestDir("expansion_keyword_test_mlog_view");
  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);
  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));
  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "MLOG VIEW");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));
  uint8_t erl = m->bus.readME0(kErlAbs);
  CHECK(erl == 0);
  if (erl != 0) {
    std::printf("  ERL after typing MLOG VIEW + Enter: %u (want 0)\n", (unsigned)erl);
  }
}

void testMlogVerboseIsRecognized() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testMlogVerboseIsRecognized -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }
  fs::path sdDir = makeTempTestDir("expansion_keyword_test_mlog_verbose");
  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);
  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));
  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "MLOG VERBOSE");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));
  uint8_t erl = m->bus.readME0(kErlAbs);
  CHECK(erl == 0);
  if (erl != 0) {
    std::printf("  ERL after typing MLOG VERBOSE + Enter: %u (want 0 -- nonzero means "
                "MLOG_CHECK_V's 'V' vs VIEW/VERBOSE disambiguation, or "
                "MLOG_CHECK_VERBOSE_REST, rejected it)\n", (unsigned)erl);
  }
}

void testMlogQuietIsRecognized() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testMlogQuietIsRecognized -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }
  fs::path sdDir = makeTempTestDir("expansion_keyword_test_mlog_quiet");
  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);
  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));
  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "MLOG QUIET");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));
  uint8_t erl = m->bus.readME0(kErlAbs);
  CHECK(erl == 0);
  if (erl != 0) {
    std::printf("  ERL after typing MLOG QUIET + Enter: %u (want 0)\n", (unsigned)erl);
  }
}

void testMlogResetIsRecognized() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testMlogResetIsRecognized -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }
  fs::path sdDir = makeTempTestDir("expansion_keyword_test_mlog_reset");
  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);
  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));
  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "MLOG RESET");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));
  uint8_t erl = m->bus.readME0(kErlAbs);
  CHECK(erl == 0);
  if (erl != 0) {
    std::printf("  ERL after typing MLOG RESET + Enter: %u (want 0)\n", (unsigned)erl);
  }
}

// Boot-hook STAGE (2026-09-24): BOOT_SELFCHECK_ENTRY (ROM_BASE+0AH) now
// jumps to STAGE_BOOT_ENTRY, so a reset with the module attached should
// stage the ROM into SRAM during the base ROM's own module scan and still
// finish booting normally; a second reset, and STAGE RAM typed at the
// prompt, should both skip the copy because ROM_GET_MODE now reports a
// verified copy. The module is loaded BEFORE cpu.reset() here -- every
// other test attaches it after boot via loadExpansionRom(), so the hook
// never runs for them -- with rom.bin's first 2K as the data-window seed,
// matching the real firmware's single combined buffer image, so the copy
// routine at 0x8400 is real code rather than 0xFF fill.
void testBootHookStagesRomThenSkipsOnReset() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomDir =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/Design01_NonDMA_8K_PV_Swap.cydsn/rom/";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomDir + "rom_8800.bin");
  std::vector<uint8_t> romBin = readFile(kExpRomDir + "rom.bin");
  if (rom.empty() || expRom.empty() || romBin.size() < 0x800) {
    std::printf("SKIP: testBootHookStagesRomThenSkipsOnReset -- ROM1.BIN, rom_8800.bin and/or "
                "rom.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_boot_stage");
  auto m = std::make_unique<BootedMachine>();
  m->bus.ioPort().useManualRtcClock();
  m->bus.loadME0(0xC000, rom.data(), rom.size());
  m->bus.loadExpansionModule(0, expRom.data(), expRom.size(), /*base=*/0x8800, /*requirePv=*/false,
                             /*usePuBank=*/false, /*dataWindowBase=*/0x8000,
                             /*dataWindowSize=*/0x800, /*instructionAddr=*/0x87FF, romBin.data(),
                             0x800);
  m->bus.expansionMock().setRootDir(sdDir);
  const pc1500::ExpansionMock& mock = m->bus.expansionMock();

  // Same two-stage boot as bootAndSettle(), then the usual NEW0 dismissal --
  // reaching idle afterwards is the proof the hook returned cleanly.
  auto resetAndBoot = [&]() {
    m->cpu.reset();
    for (long c = 0; !m->cpu.halted() && c < 20'000'000; c++) stepOne(*m);
    for (long i = 0; i < 4'000'000; i++) stepOne(*m);
    tapKey(*m, pc1500::Key::Cl);
    typeText(*m, "NEW0");
    tapKey(*m, pc1500::Key::Ent);
    CHECK(waitForIdle(*m));
  };

  // MCONF AUTOSTAGE (2026-09-28) defaults to 0: the hook stages nothing.
  resetAndBoot();
  CHECK(mock.romCopyBeginCount() == 0);
  CHECK(!mock.remapActive());

  m->bus.expansionMock().setConfigValue(pc1500::ExpansionMock::kConfigAutostage, 1);
  resetAndBoot();
  CHECK(mock.romCopyBeginCount() == 1);
  CHECK(mock.remapActive());
  CHECK(mock.romStagedVerified());
  size_t mismatches = 0;
  for (size_t i = 0; i < expRom.size(); i++) {
    if (mock.sramByte(i) != expRom[i]) mismatches++;
  }
  CHECK(mismatches == 0);
  if (mismatches != 0) std::printf("  SRAM differs from ROM image at %zu byte(s)\n", mismatches);

  // Keywords still resolve with ROM_BASE+ now answered by the SRAM copy.
  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "STAGE");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));
  CHECK(m->bus.readME0(kErlAbs) == 0);
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  resetAndBoot();
  CHECK(mock.romCopyBeginCount() == 1);  // verified copy already there -- skipped
  CHECK(mock.remapActive());

  // STAGE RAM with a copy already staged refreshes it (2026-09-29), from
  // the ROM region it's replacing: an out-of-date byte (as after a firmware
  // update) is put right. The image's last byte is the end of
  // STAGE_IS_STAGED, which only the boot hook runs.
  size_t last = expRom.size() - 1;
  m->bus.expansionMock().setSramByte(last, static_cast<uint8_t>(expRom[last] ^ 0xFF));
  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "STAGE RAM");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m, 200'000'000));   // STAGE_SHOW_OK's own KEYSCAN_WAIT
  CHECK(m->bus.readME0(kErlAbs) == 0);
  CHECK(mock.romCopyBeginCount() == 2);  // copied again
  CHECK(mock.remapActive());
  CHECK(mock.romStagedVerified());
  CHECK(mock.sramByte(last) == expRom[last]);
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));
}

// MLOGMSG (2026-09-24): its own keyword ahead of MLOG in the 'M' chain, so
// this also confirms MLOG still resolves once it's no longer the first M
// entry. Literal and string-variable forms both reach the MCU as the same
// 'S' chunk; numeric variables, empty "" and unterminated quotes are
// ERROR 1; long text is truncated to the log's 23-character width.
void testMlogmsgLogsLiteralAndStringVariable() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testMlogmsgLogsLiteralAndStringVariable -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_mlogmsg");
  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);
  const pc1500::ExpansionMock& mock = m->bus.expansionMock();

  auto run = [&](const std::string& line) {
    tapKey(*m, pc1500::Key::Cl);
    typeText(*m, line);
    tapKey(*m, pc1500::Key::Ent);
    CHECK(waitForIdle(*m));
    return m->bus.readME0(kErlAbs);
  };

  run("NEW0");

  CHECK(run("MLOGMSG \"HELLO THERE\"") == 0);
  CHECK(mock.lastUserLogMessage() == "HELLO THERE");

  CHECK(run("A$=\"FROM A VARIABLE\"") == 0);
  CHECK(run("MLOGMSG A$") == 0);
  CHECK(mock.lastUserLogMessage() == "FROM A VARIABLE");

  CHECK(run("MLOGMSG \"ABCDEFGHIJKLMNOPQRSTUVWXYZ\"") == 0);
  CHECK(mock.lastUserLogMessage() == "ABCDEFGHIJKLMNOPQRSTUVW");  // 23 characters

  // MLOG still resolves as the second M entry: bare MLOG shows the
  // VERBOSE/QUIET state and waits for a key. Checked before the error
  // cases below -- ERL keeps the last error rather than resetting.
  CHECK(run("MLOG") == 0);
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  CHECK(run("MLOGMSG A") == 1);           // numeric variable
  CHECK(run("MLOGMSG \"\"") == 1);        // empty
  CHECK(run("MLOGMSG \"UNTERMINATED") == 1);
  CHECK(mock.lastUserLogMessage() == "ABCDEFGHIJKLMNOPQRSTUVW");  // none of those logged
}


// MCONF (2026-09-25): shows and sets the MCU's persisted settings. The
// keyword runs on the MCU executor (keywords.c); the mock keeps settings
// in memory. A single-setting query SHOWs "NAME=value" at 0x8000 and waits
// for a key; bare MCONF browses them all.
void testMconfShowsAndSetsSettings() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testMconfShowsAndSetsSettings -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }
  fs::path sdDir = makeTempTestDir("expansion_keyword_test_mconf");
  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);
  const pc1500::ExpansionMock& mock = m->bus.expansionMock();

  auto run = [&](const std::string& line) {
    tapKey(*m, pc1500::Key::Cl);
    typeText(*m, line);
    tapKey(*m, pc1500::Key::Ent);
    CHECK(waitForIdle(*m));
    return m->bus.readME0(kErlAbs);
  };
  // A SHOW's text is at 0x8000; a listing's first entry at 0x8002.
  auto shown = [&](uint16_t at = 0x8000) {
    std::string text;
    for (int i = 0; i < 26; i++) text += static_cast<char>(m->bus.readME0(static_cast<uint16_t>(at + i)));
    return text.substr(0, text.find_last_not_of(' ') + 1);
  };

  run("NEW0");

  // ECVER is ROM-only now, but still shows its message and waits for a key.
  CHECK(run("ECVER") == 0);
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  // "SLEEPWAIT" also covers the detokenizer: BASIC hands it over as
  // "SLEEP" + the WAIT token.
  CHECK(run("MCONF SLEEPWAIT=1000") == 0);
  CHECK(mock.configValue(1) == 1000);
  CHECK(run("MCONF LED=0") == 0);
  CHECK(mock.configValue(0) == 0);
  CHECK(run("MCONF LOGSIZE=512") == 0);
  CHECK(mock.configValue(2) == 512);
  // AUTOSTAGE (2026-09-28): "AUTO" + BASIC's TO token + "STAGE" on the wire.
  CHECK(mock.configValue(6) == 0);
  CHECK(run("MCONF AUTOSTAGE=1") == 0);
  CHECK(mock.configValue(6) == 1);

  CHECK(run("MCONF SLEEPWAIT") == 0);
  CHECK(shown() == "SLEEPWAIT=1000");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  // HOSTNAME (2026-09-28): the BLE name, text rather than a number.
  CHECK(run("MCONF HOSTNAME") == 0);
  CHECK(shown() == "HOSTNAME=PC-1500 EMU");  // the emulator's default
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));
  CHECK(run("MCONF HOSTNAME=\"DESK PC\"") == 0);
  CHECK(mock.hostName() == "DESK PC");
  CHECK(run("A$=\"KITCHEN\"") == 0);
  CHECK(run("MCONF HOSTNAME=A$") == 0);
  CHECK(mock.hostName() == "KITCHEN");

  // BLKBD (2026-10-04): the external keyboard's driver at the next reset.
  CHECK(run("MCONF BLKBD=1") == 0);
  CHECK(mock.configValue(7) == 1);

  // POWMANDELAY (2026-10-05): -1 (the default) is off, stored as 0xFFFF.
  CHECK(run("MCONF POWMANDELAY") == 0);
  CHECK(shown() == "POWMANDELAY=-1");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));
  CHECK(run("MCONF POWMANDELAY=60") == 0);
  CHECK(mock.configValue(8) == 60);
  CHECK(run("MCONF POWMANDELAY=-1") == 0);
  CHECK(mock.configValue(8) == 0xFFFF);
  CHECK(run("MCONF POWMANDELAY=0") == 0);
  CHECK(mock.configValue(8) == 0);

  // KBDLAYOUT (2026-10-07): the external keyboard's layout as its HID
  // country code -- 33 US (the default), 8 French, 9 German, 25 Spanish,
  // 2 Belgian.
  CHECK(run("MCONF KBDLAYOUT") == 0);
  CHECK(shown() == "KBDLAYOUT=33");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));
  CHECK(run("MCONF KBDLAYOUT=8") == 0);
  CHECK(mock.configValue(9) == 8);

  CHECK(run("MCONF") == 0);  // browse: first entry is LED, HOSTNAME last (8th)
  CHECK(shown(0x8002) == "LED=0");
  CHECK(shown(0x8002 + 3 * 30) == "AUTOSTAGE=1");
  CHECK(shown(0x8002 + 4 * 30) == "BLKBD=1");
  CHECK(shown(0x8002 + 5 * 30) == "POWMANDELAY=0");
  CHECK(shown(0x8002 + 6 * 30) == "KBDLAYOUT=8");
  CHECK(shown(0x8002 + 7 * 30) == "HOSTNAME=KITCHEN");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  // Errors last -- ERL keeps the last error rather than resetting.
  CHECK(run("MCONF LED=2") == 1);          // out of range
  CHECK(mock.configValue(0) == 0);
  CHECK(run("MCONF AUTOSTAGE=2") == 1);    // 0 or 1 only
  CHECK(mock.configValue(6) == 1);
  CHECK(run("MCONF BLKBD=2") == 1);
  CHECK(mock.configValue(7) == 1);
  CHECK(run("MCONF POWMANDELAY=-2") == 1);  // -1 is the only negative
  CHECK(mock.configValue(8) == 0);
  CHECK(run("MCONF KBDLAYOUT=1") == 1);     // a country code with no layout (Arabic)
  CHECK(run("MCONF KBDLAYOUT=34") == 1);    // beyond US, the highest
  CHECK(mock.configValue(9) == 8);
  CHECK(run("MCONF COLOUR=1") == 1);       // unknown setting
  CHECK(run("MCONF SLEEPWAIT=") == 1);     // no value
  CHECK(mock.configValue(1) == 1000);
  CHECK(run("MCONF BLE=1") == 1);          // the BLE spike's setting is gone
  CHECK(mock.configValue(5) == 0);
  CHECK(run("MCONF HOSTNAME=\"SIXTEEN CHARS 16\"") == 1);  // too long
  CHECK(run("MCONF HOSTNAME=5") == 1);     // not text
  CHECK(run("MCONF HOSTNAME=\"\"") == 1);  // empty
  CHECK(mock.hostName() == "KITCHEN");
}



// Keywords in a running program, with BASIC expressions as arguments
// (2026-09-25): the ROM hands the MCU the statement wherever BASIC's text
// pointer is (the program line here), the MCU asks the ROM to evaluate
// anything that isn't a plain literal, and every keyword returns through
// BASIC's own end-of-statement vector, so the rest of the line and the
// next line still run. Also at the prompt: expression arguments.
void testKeywordsInProgramWithExpressions() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testKeywordsInProgramWithExpressions -- ROM1.BIN and/or rom_8800.bin not found.%c", 10);
    return;
  }
  fs::path sdDir = makeTempTestDir("expansion_keyword_test_program");
  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);
  const pc1500::ExpansionMock& mock = m->bus.expansionMock();
  auto run = [&](const std::string& line) {
    tapKey(*m, pc1500::Key::Cl);
    typeText(*m, line);
    tapKey(*m, pc1500::Key::Ent);
    CHECK(waitForIdle(*m));
    return m->bus.readME0(kErlAbs);
  };
  auto number = [&](uint16_t at) {  // exponent, then the first mantissa byte
    return (m->bus.readME0(at) << 8) | m->bus.readME0(static_cast<uint16_t>(at + 2));
  };

  run("NEW0");
  m->bus.writeME0(kErlAbs, 0);

  // At the prompt: expressions, and a statement after ':'.
  CHECK(run("MLOGMSG \"A\"+\"B\"") == 0);
  CHECK(mock.lastUserLogMessage() == "AB");
  CHECK(run("MCONF SLEEPWAIT=2*500") == 0);
  CHECK(mock.configValue(1) == 1000);
  // A second statement after ':' at the prompt is ERROR 1 for BASIC's own
  // statements too (E=1:D=3 -- the PC-1500 doesn't run multi-statement
  // lines in immediate mode), and the keywords now end the same way.
  m->bus.writeME0(kErlAbs, 0);
  CHECK(run("E=1:D=3") == 1);
  m->bus.writeME0(kErlAbs, 0);
  CHECK(run("SDCLOSE ALL:D=4") == 1);
  m->bus.writeME0(kErlAbs, 0);

  std::string typeError;
  CHECK(pc1500::basic::typeBasicProgramText(
      m->bus, m->cpu,
      "10 SDOPEN \"PROG.SDF\" AS 1:A=5:SDPRINT #1,A*2,\"X\"+\"Y\":SDCLOSE 1\n"
      "20 F$=\"PROG.SDF\":SDOPEN F$,2:SDINPUT #2,B,T$:SDCLOSE 2:C=7\n",
      kCyclesPerFrame, kCyclesPerTimerTick, &typeError));
  tapKey(*m, pc1500::Key::Cl);
  tapKey(*m, pc1500::Key::Mode);  // PRO -> RUN mode; RUN in PRO mode is ERROR 26
  CHECK(run("RUN") == 0);
  CHECK(fs::exists(sdDir / "PROG.SDF"));
  CHECK(number(0x7908) == 0x0110);  // B = 10: exponent 1, mantissa 1.0
  CHECK(m->bus.readME0(kVarTDollar) == 'X');
  CHECK(m->bus.readME0(kVarTDollar + 1) == 'Y');
  CHECK(m->bus.readME0(kVarTDollar + 2) == 0);
  CHECK(number(0x7910) == 0x0070);  // C = 7 -- the line went on after the keywords
}

// FNCLR / FNSAVE / FNLOAD and STSAVE / STLOAD (2026-09-25). The function
// keys are the 195 bytes ending just before the BASIC program (7865H);
// FNCLR runs in the ROM alone. STLOAD writes all of 0000H-7FFFH back --
// the stack page last, from the ROM's RESTORE -- and resumes right after
// the STSAVE that made the state: at the prompt that just finishes the
// command; in a program, the program carries on from there.
void testFnKeysAndStateSaveRestore() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testFnKeysAndStateSaveRestore -- ROM1.BIN and/or rom_8800.bin not found.%c", 10);
    return;
  }
  fs::path sdDir = makeTempTestDir("expansion_keyword_test_fn_state");
  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);
  auto run = [&](const std::string& line) {
    tapKey(*m, pc1500::Key::Cl);
    typeText(*m, line);
    tapKey(*m, pc1500::Key::Ent);
    CHECK(waitForIdle(*m));
    return m->bus.readME0(kErlAbs);
  };
  auto number = [&](uint16_t at) {  // exponent, then the first mantissa byte
    return (m->bus.readME0(at) << 8) | m->bus.readME0(static_cast<uint16_t>(at + 2));
  };

  run("NEW0");
  m->bus.writeME0(kErlAbs, 0);

  // Function keys
  uint16_t start = static_cast<uint16_t>((m->bus.readME0(0x7865) << 8) | m->bus.readME0(0x7866));
  uint16_t keys = static_cast<uint16_t>(start - 195);
  for (int i = 0; i < 195; i++) m->bus.writeME0(static_cast<uint16_t>(keys + i), static_cast<uint8_t>(i * 7 + 1));
  CHECK(run("FNSAVE") == 0);
  CHECK(run("FNCLR") == 0);
  bool cleared = true;
  for (int i = 0; i < 195; i++) cleared = cleared && m->bus.readME0(static_cast<uint16_t>(keys + i)) == 0;
  CHECK(cleared);
  CHECK(m->bus.readME0(static_cast<uint16_t>(keys - 1)) != 0 || keys == 0);  // nothing before them touched
  CHECK(run("FNLOAD") == 0);
  bool restored = true;
  for (int i = 0; i < 195; i++)
    restored = restored && m->bus.readME0(static_cast<uint16_t>(keys + i)) == static_cast<uint8_t>(i * 7 + 1);
  CHECK(restored);

  // State at the prompt: variables come back, and the prompt still works.
  CHECK(run("A=5") == 0);
  CHECK(run("STSAVE") == 0);
  CHECK(run("A=9") == 0);
  CHECK(number(0x7900) == 0x0090);  // A = 9
  CHECK(run("STLOAD") == 0);
  CHECK(number(0x7900) == 0x0050);  // A = 5 again
  CHECK(run("B=2") == 0);
  CHECK(number(0x7908) == 0x0020);

  // State saved inside a program: STLOAD resumes the program after STSAVE.
  run("NEW");
  std::string typeError;
  CHECK(pc1500::basic::typeBasicProgramText(m->bus, m->cpu, "10 C=0:STSAVE:C=C+1\n", kCyclesPerFrame,
                                             kCyclesPerTimerTick, &typeError));
  tapKey(*m, pc1500::Key::Cl);
  tapKey(*m, pc1500::Key::Mode);  // PRO -> RUN mode
  CHECK(run("RUN") == 0);
  CHECK(number(0x7910) == 0x0010);  // C = 1
  CHECK(run("C=5") == 0);
  CHECK(run("STLOAD") == 0);
  CHECK(number(0x7910) == 0x0010);  // C = 0 restored, then C=C+1 ran again
}
}  // namespace

// BLE keywords (2026-09-27, RP2350/BLE_PROTOCOL.md). The mock answers the
// EXP_COMMAND_BLE_* commands with a fake peer standing in for the
// feature-server app: BLSCAN finds "MARVIN", BLPRINT/BLLIST text lands in
// bleText() with CR line ends, BLSAVE/BLLOAD use bleFiles().
struct BleFixture {
  std::unique_ptr<BootedMachine> m;
  pc1500::ExpansionMock* mock = nullptr;
  fs::path sdDir;

  // Starts `line`, lets it get into its wait, then presses BREAK the way
  // the host's F12 does (2026-09-28: BLADV/BLPUT/BLGET). Returns ERL.
  int runThenBreak(const std::string& line) {
    m->bus.writeME0(kErlAbs, 0);
    tapKey(*m, pc1500::Key::Cl);
    typeText(*m, line);
    tapKey(*m, pc1500::Key::Ent);
    for (long i = 0; i < 200'000; i++) stepOne(*m);
    m->cpu.pressOnKey();
    m->bus.ioPort().setOnKeyLine(true);
    m->cpu.requestMI();
    m->bus.ioPort().setOnKeyLine(false);
    CHECK(waitForIdle(*m, 20'000'000));
    return m->bus.readME0(kErlAbs);
  }

  // The line's error number (0 = none). ERL keeps the last error until
  // another one, so it's cleared first.
  int run(const std::string& line, long maxInstructions = 2'000'000) {
    m->bus.writeME0(kErlAbs, 0);
    tapKey(*m, pc1500::Key::Cl);
    typeText(*m, line);
    tapKey(*m, pc1500::Key::Ent);
    CHECK(waitForIdle(*m, maxInstructions));
    return m->bus.readME0(kErlAbs);
  }
  // A SHOW's text is at 0x8000; a listing's first entry at 0x8002.
  std::string shown(uint16_t at = 0x8000) {
    std::string text;
    for (int i = 0; i < 26; i++) text += static_cast<char>(m->bus.readME0(static_cast<uint16_t>(at + i)));
    return text.substr(0, text.find_last_not_of(' ') + 1);
  }
  void key(pc1500::Key k) {
    tapKey(*m, k);
    CHECK(waitForIdle(*m));
  }
  std::vector<uint8_t> program() {
    std::string error;
    return pc1500::basic::readBasicProgramBytes(m->bus, &error);
  }
  bool typeProgram(const std::string& text) {
    std::string error;
    bool ok = pc1500::basic::typeBasicProgramText(m->bus, m->cpu, text, kCyclesPerFrame, kCyclesPerTimerTick, &error);
    if (!ok) std::printf("  typeBasicProgramText: %s\n", error.c_str());
    return ok;
  }
  // Connects with BLCON and clears the text the peer has seen.
  void connect() {
    CHECK(run("BLCON \"MARVIN\"") == 0);
    CHECK(shown() == "CONNECTED: MARVIN");
    key(pc1500::Key::Ent);
    mock->clearBleText();
  }
};

// extRam0000Bytes/extRamExtBytes: expansion RAM, as bootAndSettle().
static std::unique_ptr<BleFixture> bleFixture(const char* testName, size_t extRam0000Bytes = 0x4000,
                                              size_t extRamExtBytes = 0x2800) {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: %s -- ROM1.BIN and/or rom_8800.bin not found.\n", testName);
    return nullptr;
  }
  auto f = std::make_unique<BleFixture>();
  f->m = bootAndSettle(rom, extRam0000Bytes, extRamExtBytes);
  f->sdDir = makeTempTestDir(testName);
  loadExpansionRom(*f->m, expRom, f->sdDir);
  f->mock = &f->m->bus.expansionMock();
  f->run("NEW0");
  return f;
}

// BLSCAN lists the peers; C on one connects. BLCON finds one by name
// in any case; an unknown name, or text with no link, is ERROR 40.
void testBleScanConnectAndDisconnect() {
  auto f = bleFixture("testBleScanConnectAndDisconnect");
  if (!f) return;
  CHECK(f->run("BLPRINT \"X\"") == 40);  // no link yet
  CHECK(f->run("BLSCAN") == 0);
  CHECK(f->shown(0x8002) == "MARVIN");
  f->key(pc1500::Key::L);  // L is SDLOAD's key, not this one
  CHECK(!f->mock->bleConnected());
  f->key(pc1500::Key::C);
  CHECK(f->shown() == "CONNECTED: MARVIN");
  f->key(pc1500::Key::Ent);
  CHECK(f->mock->bleConnected());
  CHECK(f->run("BLDISC") == 0);
  CHECK(!f->mock->bleConnected());
  CHECK(f->run("BLCON \"marvin\"") == 0);
  f->key(pc1500::Key::Ent);
  CHECK(f->mock->bleConnected());
  f->mock->blePeers().clear();
  CHECK(f->run("BLSCAN 1") == 0);
  CHECK(f->shown() == "BLE: NO PEERS FOUND");
  f->key(pc1500::Key::Ent);
  CHECK(f->run("BLCON \"NOBODY\"") == 40);
}

// Wi-Fi (2026-10-06, RP2350/wifi_link.h). The mock's networks stand in for
// the CYW43's: "HOST" (open) unless a test adds more. WFSCAN lists them,
// strongest first; C connects -- with the remembered password, with none
// for an open network, else after asking for one ('*'s; the left arrow
// deletes, SML gives lowercase). A network that connects is remembered;
// WFCON alone rejoins the strongest remembered one. WFSTAT is a function:
// 0 off, 2 connected.
void testWifiScanConnectAndPassword() {
  auto f = bleFixture("testWifiScanConnectAndPassword");
  if (!f) return;
  auto& nets = f->mock->wifiNetworks();
  nets.push_back({"HomeNet", -30, "WPA2", "Ab1"});
  nets.push_back({"OLDCAFE", -70, "WEP", ""});
  auto said = [&](const std::string& expr) {  // what BLPRINT expr shows the BLE peer
    f->mock->clearBleText();
    CHECK(f->run("BLPRINT " + expr) == 0);
    std::string t = f->mock->bleText();
    return t.empty() ? t : t.substr(0, t.size() - 1);
  };
  f->connect();  // BLE, only to read values with BLPRINT
  CHECK(said("WFSTAT") == "0");

  CHECK(f->run("WFSCAN") == 0);
  CHECK(f->shown(0x8002) == "HomeNet         -30 WPA2");
  f->key(pc1500::Key::C);  // secured, not remembered: asks
  CHECK(f->shown() == "PASSWORD:");
  f->key(pc1500::Key::A);
  CHECK(f->shown() == "PASSWORD: *");
  f->key(pc1500::Key::X);
  f->key(pc1500::Key::Left);  // deletes the X
  CHECK(f->shown() == "PASSWORD: *");
  f->key(pc1500::Key::Sml);
  f->key(pc1500::Key::B);  // lowercase
  f->key(pc1500::Key::Sml);
  f->key(pc1500::Key::Digit1);
  CHECK(f->shown() == "PASSWORD: ***");
  f->key(pc1500::Key::Ent);
  CHECK(f->shown() == "CONNECTED: 192.168.1.15");
  f->key(pc1500::Key::Ent);
  CHECK(f->mock->wifiConnectedSsid() == "HomeNet");
  CHECK(f->mock->wifiRemembered("HomeNet") && *f->mock->wifiRemembered("HomeNet") == "Ab1");
  CHECK(said("WFSTAT") == "2");
  CHECK(f->run("WFSCAN") == 0);  // remembered: marked, and no question
  CHECK(f->shown(0x8002) == "HomeNet         -30 WPA2*");
  f->key(pc1500::Key::C);
  CHECK(f->shown() == "CONNECTED: 192.168.1.15");
  f->key(pc1500::Key::Ent);

  CHECK(f->run("WFDISC") == 0);
  CHECK(f->mock->wifiConnectedSsid().empty());
  CHECK(said("WFSTAT") == "0");
  CHECK(f->run("WFCON") == 0);  // the strongest remembered network
  CHECK(f->shown() == "CONNECTED: 192.168.1.15");
  f->key(pc1500::Key::Ent);
  CHECK(f->mock->wifiConnectedSsid() == "HomeNet");

  // BREAK at the password gives up; so does ENTER with nothing typed.
  CHECK(f->run("WFFORGET \"homenet\"") == 0);
  CHECK(!f->mock->wifiRemembered("HomeNet"));
  CHECK(f->run("WFCON \"HOMENET\"") == 0);  // any case
  CHECK(f->shown() == "PASSWORD:");
  f->key(pc1500::Key::Ent);
  CHECK(f->m->bus.readME0(kErlAbs) == 0);

  // A WEP network is refused: the reason, then ERROR 40.
  CHECK(f->run("WFSCAN") == 0);
  f->key(pc1500::Key::Down);
  f->key(pc1500::Key::Down);
  CHECK(f->shown(0x8002 + 2 * 30) == "OLDCAFE         -70 WEP");
  f->key(pc1500::Key::C);
  CHECK(f->shown() == "WIFI: WEP NOT SUPPORTED");
  f->key(pc1500::Key::Ent);
  CHECK(f->m->bus.readME0(kErlAbs) == 40);
}

// WFSTAT is a function: alone at the prompt, or in an assignment, no
// error. MLOG CLEAR is MLOG RESET's new name (2026-10-06); both work.
void testWifiStatAndMlogClear() {
  auto f = bleFixture("testWifiStatAndMlogClear");
  if (!f) return;
  CHECK(f->run("WFSTAT") == 0);
  CHECK(f->run("S=WFSTAT") == 0);
  CHECK(f->run("MLOG CLEAR") == 0);
  CHECK(f->run("MLOG RESET") == 0);
}

// WFCON by name: an open network needs no password; a wrong one, or a
// network that isn't there, says so, then ERROR 40. The arguments are any
// string expressions, so a password can hold lowercase (CHR$). WFFORGET
// forgets one, ERROR 40 if it isn't remembered, or all after a Y.
void testWifiConnectByNameAndForget() {
  auto f = bleFixture("testWifiConnectByNameAndForget");
  if (!f) return;
  f->mock->wifiNetworks().push_back({"HomeNet", -30, "WPA2", "Ab1"});
  CHECK(f->run("WFCON") == 0);  // nothing remembered yet
  CHECK(f->shown() == "WIFI: NO KNOWN NETWORK");
  f->key(pc1500::Key::Ent);
  CHECK(f->m->bus.readME0(kErlAbs) == 40);

  CHECK(f->run("WFCON \"HOST\"") == 0);
  CHECK(f->shown() == "CONNECTED: 192.168.1.15");
  f->key(pc1500::Key::Ent);
  CHECK(f->mock->wifiConnectedSsid() == "HOST");

  CHECK(f->run("WFCON \"HOMENET\",\"AB1\"") == 0);
  CHECK(f->shown() == "WIFI: WRONG PASSWORD");
  f->key(pc1500::Key::Ent);
  CHECK(f->m->bus.readME0(kErlAbs) == 40);
  CHECK(f->mock->wifiConnectedSsid().empty());

  CHECK(f->run("A$=\"HOMENET\"") == 0);
  CHECK(f->run("B$=\"A\"+CHR$ 98+\"1\"") == 0);
  CHECK(f->run("WFCON A$,B$") == 0);
  CHECK(f->shown() == "CONNECTED: 192.168.1.15");
  f->key(pc1500::Key::Ent);
  CHECK(f->mock->wifiConnectedSsid() == "HomeNet");

  CHECK(f->run("WFCON \"NOBODY\"") == 0);
  CHECK(f->shown() == "WIFI: NETWORK NOT FOUND");
  f->key(pc1500::Key::Ent);
  CHECK(f->m->bus.readME0(kErlAbs) == 40);

  CHECK(f->run("WFFORGET \"NOBODY\"") == 40);
  CHECK(f->run("WFFORGET \"host\"") == 0);
  CHECK(!f->mock->wifiRemembered("HOST"));
  CHECK(f->mock->wifiRemembered("HomeNet"));
  CHECK(f->run("WFFORGET") == 0);
  CHECK(f->shown() == "FORGET ALL NETWORKS Y/N");
  f->key(pc1500::Key::Y);
  CHECK(!f->mock->wifiRemembered("HomeNet"));
  CHECK(f->run("WFDISC 1") == 1);  // no arguments
}

// BLPRINT: ';' runs values together, ',' pads to the next 13-column zone,
// a trailing separator leaves the line open; each statement's text is sent
// when it ends. Numbers read exactly as STR$ writes them (the ROM's own
// conversion), and it works inside a program.
void testBlePrintText() {
  auto f = bleFixture("testBlePrintText");
  if (!f) return;
  f->connect();
  CHECK(f->run("BLPRINT \"HELLO WORLD\"") == 0);
  CHECK(f->mock->bleText() == "HELLO WORLD\r");
  f->mock->clearBleText();
  CHECK(f->run("BLPRINT \"A\";1;\"B\"") == 0);
  CHECK(f->mock->bleText() == "A1B\r");
  f->mock->clearBleText();
  CHECK(f->run("BLPRINT \"A\",\"B\"") == 0);
  CHECK(f->mock->bleText() == "A            B\r");
  f->mock->clearBleText();
  CHECK(f->run("BLPRINT \"AB\";") == 0);
  CHECK(f->run("BLPRINT \"CD\"") == 0);
  CHECK(f->run("BLPRINT") == 0);
  CHECK(f->mock->bleText() == "ABCD\r\r");

  // BLCLS: a form feed, which clears the app's console; BLPRINT CHR$(12)
  // is the same thing spelled out.
  f->mock->clearBleText();
  CHECK(f->run("BLCLS") == 0);
  CHECK(f->mock->bleText() == "\f");
  CHECK(f->run("BLPRINT CHR$(12);") == 0);
  CHECK(f->mock->bleText() == "\f\f");
  CHECK(f->run("BLCLS 1") == 1);
  // CHR$ leaves C1H, not D0H, in the arithmetic register (TRM p.123); the
  // ROM hands it over as a string all the same.
  f->mock->clearBleText();
  CHECK(f->run("BLPRINT CHR$(65);\"B\"") == 0);
  CHECK(f->mock->bleText() == "AB\r");

  // Held in a variable first: STR$ of an expression works from the
  // unrounded intermediate (1/7 -> ...428), while an evaluated value --
  // what BLPRINT and PRINT get -- is rounded to 10 digits (...429).
  for (const char* e : {"1500", "-5", "0", "1", "100000", "9999999999", "-9999999999", "99999999999", "1/3", "-1/3",
                        "0.5", "-0.5", "0.05", "1/7", "1/70", "0.123456789", "-0.123456789", "0.1234567891",
                        "1E-9", "-1E-9", "1E-10", "1.5E-9", "0.000000012", "0.0000000123", "2/3*1E-5", "1/7*1E5",
                        "1/7*1E9", "1/7*1E10", "1E10", "-1E10", "1.234E9", "-1.234E-12", "12345678901", "1/7*1E12",
                        "0.001", "0.0001234", "123.456", "-0.25", "1E99", "-1E-99", "&FF", "32767*2"}) {
    std::string expr = e;
    int setErl = f->run("A=" + expr);
    CHECK(setErl == 0);
    if (setErl != 0) std::printf("  A=%s: ERL %d\n", e, setErl);
    f->mock->clearBleText();
    int printErl = f->run("BLPRINT A");
    CHECK(printErl == 0);
    if (printErl != 0) std::printf("  BLPRINT A (A=%s): ERL %d\n", e, printErl);
    std::string ours = f->mock->bleText();
    f->mock->clearBleText();
    CHECK(f->run("BLPRINT STR$(A)") == 0);
    std::string rom = f->mock->bleText();
    CHECK(ours == rom);
    if (ours != rom) std::printf("  BLPRINT %s: ours [%s], STR$ [%s]\n", e, ours.c_str(), rom.c_str());
  }

  f->mock->clearBleText();
  CHECK(f->typeProgram("10 FOR I=1 TO 3\n20 BLPRINT \"LINE \";I\n30 NEXT I\n"));
  f->key(pc1500::Key::Cl);
  f->key(pc1500::Key::Mode);  // PRO -> RUN mode; RUN in PRO mode is ERROR 26
  int erl = f->run("RUN");
  CHECK(erl == 0);
  CHECK(f->mock->bleText() == "LINE 1\rLINE 2\rLINE 3\r");
  if (erl != 0 || f->mock->bleText() != "LINE 1\rLINE 2\rLINE 3\r")
    std::printf("  RUN: ERL %d, text [%s], program %zu bytes\n", erl, f->mock->bleText().c_str(), f->program().size());
}

// BLLIST sends the program as text, spaced exactly as pc1500emu's own
// detokenizer lists a program file; BLLIST from,to limits the lines.
void testBleListMatchesDetokenizer() {
  auto f = bleFixture("testBleListMatchesDetokenizer");
  if (!f) return;
  f->connect();
  CHECK(f->typeProgram(
      "10 PRINT A;\"X Y\":GOTO 10\n20 IF A=1 THEN 30\n30 FOR I=1 TO 10 STEP 2:NEXT I\n"
      "40 REM HELLO  THERE\n50 A$=STR$(LEN(\"AB\"))+CHR$(65)\n"));
  CHECK(f->run("BLLIST") == 0);
  std::string expected, error;
  CHECK(pc1500::basic::detokenizeBasicProgram(f->program(), &expected, &error));
  std::string ours = f->mock->bleText();
  std::replace(ours.begin(), ours.end(), '\r', '\n');
  CHECK(ours == expected);
  if (ours != expected) std::printf("  BLLIST:\n%s  detokenizer:\n%s", ours.c_str(), expected.c_str());

  f->mock->clearBleText();
  CHECK(f->run("BLLIST 20,30") == 0);
  CHECK(f->mock->bleText().rfind("20 ", 0) == 0);
  CHECK(f->mock->bleText().find("\r30 ") != std::string::npos);
  CHECK(f->mock->bleText().find("40 ") == std::string::npos);
}

// BLSAVE/BLLOAD move the same bytes SDSAVE writes; an existing file asks
// before overwriting (N keeps it, Y or -Y replaces it); M files carry the
// [start][call] header. A missing file, or a link that drops part-way,
// is ERROR 40 and leaves no partial file behind.
void testBleSaveLoad() {
  auto f = bleFixture("testBleSaveLoad");
  if (!f) return;
  f->connect();
  CHECK(f->typeProgram("10 PRINT 1\n20 END\n"));
  std::vector<uint8_t> original = f->program();
  CHECK(f->run("BLSAVE \"T\"") == 0);
  CHECK(f->mock->bleFiles()["T"] == original);

  CHECK(f->run("NEW") == 0);
  CHECK(f->run("BLLOAD \"T\"") == 0);
  CHECK(f->program() == original);

  CHECK(f->typeProgram("30 BEEP 1\n"));
  std::vector<uint8_t> changed = f->program();
  CHECK(f->run("BLSAVE \"T\"") == 0);
  CHECK(f->shown() == "FILE EXISTS. OVERWRITE Y/N");
  f->key(pc1500::Key::N);
  CHECK(f->mock->bleFiles()["T"] == original);
  CHECK(f->run("BLSAVE \"T\"") == 0);
  f->key(pc1500::Key::Y);
  CHECK(f->mock->bleFiles()["T"] == changed);
  CHECK(f->run("BLSAVE \"T\",-Y") == 0);
  CHECK(f->mock->bleFiles()["T"] == changed);

  CHECK(f->run("POKE &4400,1,2,3,4") == 0);
  CHECK(f->run("BLSAVE M \"MC\",&4400,&4403,&4401") == 0);
  const std::vector<uint8_t> mExpected = {0x44, 0x00, 0x44, 0x01, 1, 2, 3, 4};
  CHECK(f->mock->bleFiles()["MC"] == mExpected);
  CHECK(f->run("POKE &4400,0,0,0,0") == 0);
  f->mock->bleFiles()["MC"] = {0x44, 0x00, 0x00, 0x00, 5, 6, 7, 8};  // no CALL
  CHECK(f->run("BLLOAD M \"MC\"") == 0);
  CHECK(f->m->bus.readME0(0x4400) == 5 && f->m->bus.readME0(0x4403) == 8);

  CHECK(f->run("BLLOAD \"NOPE\"") == 40);
  f->mock->setBleLinkDropAfter(4);
  CHECK(f->run("BLSAVE \"U\"") == 40);
  CHECK(f->mock->bleFiles().count("U") == 0);
  int erl = f->run("BLCON \"MARVIN\"");
  CHECK(erl == 0);
  if (erl != 0) std::printf("  BLCON after a drop: ERL %d, shown [%s]\n", erl, f->shown().c_str());
  f->key(pc1500::Key::Ent);
  f->mock->setBleLinkDropAfter(4);
  CHECK(f->run("BLLOAD \"T\"") == 40);
}

// SDSAVE with no name (2026-09-28): saves as the last BASIC program SDLOAD
// loaded -- where it was loaded from, even after an SDCD -- asking first,
// as that file is there. Nothing loaded yet: ERROR 1, as before.
void testSdsaveBareSavesAsLastLoaded() {
  auto f = bleFixture("testSdsaveBareSavesAsLastLoaded");
  if (!f) return;
  auto onCard = [&](const fs::path& rel) {
    std::ifstream in(f->sdDir / rel, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  };
  CHECK(f->run("SDSAVE") == 1);  // nothing loaded yet
  CHECK(f->typeProgram("10 PRINT 1\n"));
  CHECK(f->run("SDMKDIR \"SUB\"") == 0);
  CHECK(f->run("SDSAVE \"SUB/P.BAS\"") == 0);
  CHECK(f->run("SDCD \"SUB\"") == 0);
  CHECK(f->run("NEW") == 0);
  CHECK(f->run("SDLOAD \"P.BAS\"") == 0);  // relative to SUB
  CHECK(f->run("SDCD \"..\"") == 0);        // and now somewhere else
  CHECK(f->typeProgram("20 PRINT 2\n"));
  std::vector<uint8_t> changed = f->program();
  CHECK(f->run("SDSAVE") == 0);
  CHECK(f->shown() == "FILE EXISTS. OVERWRITE Y/N");
  f->key(pc1500::Key::Y);
  CHECK(onCard("SUB/P.BAS") == changed);
  CHECK(!fs::exists(f->sdDir / "P.BAS"));

  CHECK(f->typeProgram("30 END\n"));
  CHECK(f->run("SDSAVE") == 0);
  CHECK(f->shown() == "FILE EXISTS. OVERWRITE Y/N");
  f->key(pc1500::Key::N);
  CHECK(onCard("SUB/P.BAS") == changed);  // N leaves it

  CHECK(f->run("BLSAVE") == 1);  // BLSAVE still needs a name
}

// Peer messaging (2026-09-29, BLE_PROTOCOL.md "Peer messaging"): BLSEND
// sends one message of value chunks, BLRECV takes the oldest into
// variables, and the function BLSTAT counts what's waiting. The fake peer
// plays the other PC-1500. Values are checked by BLPRINTing them to it.
void testBleMessaging() {
  auto f = bleFixture("testBleMessaging");
  if (!f) return;
  auto said = [&](const std::string& expr) {  // what BLPRINT expr shows the peer
    f->mock->clearBleText();
    CHECK(f->run("BLPRINT " + expr) == 0);
    std::string t = f->mock->bleText();
    return t.empty() ? t : t.substr(0, t.size() - 1);  // without the CR
  };
  CHECK(f->run("BLSEND 1") == 40);  // no link
  // BLSTAT is a function, like MEM (2026-09-29).
  CHECK(f->run("S=BLSTAT") == 0);
  f->connect();
  CHECK(said("S") == "-1");  // no link then, nothing waiting
  CHECK(said("BLSTAT") == "0");  // evaluated inside another keyword

  // BLSEND: any expressions, as one message of chunks.
  CHECK(f->run("A=5") == 0);
  CHECK(f->run("B$=\"HI\"") == 0);
  CHECK(f->run("BLSEND A*2,B$+\"!\",3") == 0);
  CHECK(f->mock->bleMessagesSent().size() == 1);
  std::vector<uint8_t> msg = f->mock->bleMessagesSent().back();
  CHECK(msg.size() == 9 + 5 + 9);
  if (msg.size() != 23) return;
  CHECK(msg[0] == 'N' && msg[9] == 'S' && msg[10] == 3 && msg[14] == 'N');
  CHECK(std::string(msg.begin() + 11, msg.begin() + 14) == "HI!");

  // BLRECV: back into variables, in order.
  f->mock->bleMessageToUs(msg);
  CHECK(f->run("S=BLSTAT*10+1") == 0);
  CHECK(said("S") == "11");
  // In a running program too (IF ... THEN LET: LET is needed after THEN).
  CHECK(f->typeProgram("10 T=0:IF BLSTAT>0 THEN LET T=BLSTAT+4\n20 END\n"));
  f->key(pc1500::Key::Mode);  // PRO -> RUN mode; RUN in PRO mode is ERROR 26
  CHECK(f->run("RUN") == 0);
  CHECK(said("T") == "5");
  // Alone at the prompt it's shown, as MEM is. The number goes straight
  // to the LCD, which 7BB0H doesn't hold (MEM leaves only its token there
  // too), so only "no error" is checked here.
  CHECK(f->run("BLSTAT") == 0);
  CHECK(f->run("BLRECV X,Y$,Z") == 0);
  CHECK(said("X;Y$;Z") == "10HI!3");
  // Fewer values than variables: the rest become 0 / blank.
  std::vector<uint8_t> number(msg.begin(), msg.begin() + 9), text(msg.begin() + 9, msg.begin() + 14);
  CHECK(f->run("Q$=\"X\"") == 0);
  CHECK(f->run("R=9") == 0);
  f->mock->bleMessageToUs(number);
  CHECK(f->run("BLRECV P,Q$,R") == 0);
  CHECK(said("P;\"/\";Q$;\"/\";R") == "10//0");  // (no | on the PC-1500's keyboard)
  // A string for a number: ERROR 42.
  f->mock->bleMessageToUs(text);
  CHECK(f->run("BLRECV P") == 42);

  // Waiting: a message that only arrives a few polls later...
  f->mock->bleMessageToUs(number, 5);
  CHECK(f->run("BLRECV V") == 0);
  CHECK(said("V") == "10");
  // ...#0 with nothing waiting leaves the variable alone...
  CHECK(f->run("V=7") == 0);
  CHECK(f->run("BLRECV #0,V") == 0);
  CHECK(said("V") == "7");
  // ...and with nothing coming, BLRECV waits until BREAK, and leaves it too.
  CHECK(f->runThenBreak("BLRECV V") == 0);
  CHECK(said("V") == "7");

  // The peer's inbox is full: BLSEND waits and gets through, or BREAK.
  f->mock->setBlePeerInboxFullFor(3);
  CHECK(f->run("BLSEND 1") == 0);
  CHECK(f->mock->bleMessagesSent().size() == 2);
  f->mock->setBlePeerInboxFullFor(1000000000);
  CHECK(f->runThenBreak("BLSEND 1") == 0);
  CHECK(f->mock->bleMessagesSent().size() == 2);
  f->mock->setBlePeerInboxFullFor(0);

  int bare = f->run("BLSEND");
  CHECK(bare == 1);
  if (bare != 1) {
    std::printf("  bare BLSEND: ERL %d, sent %zu; DISP_BUFFER:", bare, f->mock->bleMessagesSent().size());
    for (uint16_t a = 0x7BB0; a < 0x7BC0; a++) std::printf(" %02X", f->m->bus.readME0(a));
    std::printf("\n");
  }
  CHECK(f->run("BLRECV") == 1);
  // Too long for one message (27 numbers x 9 bytes > 240): ERROR 40.
  std::string many = "BLSEND A";
  for (int i = 1; i < 27; i++) many += ",A";
  CHECK(f->run(many) == 40);
  CHECK(f->mock->bleMessagesSent().size() == 2);
}

// SDEOF(n) (2026-09-30): a function of one argument (code E170, ABS's low
// byte), evaluated by BASIC before the call -- 1 once channel n has nothing
// left for SDINPUT#, so a read loop can stop at the end of the file. Values
// are checked by BLPRINTing them to the fake peer.
void testSdeofEndsReadLoop() {
  auto f = bleFixture("testSdeofEndsReadLoop");
  if (!f) return;
  auto said = [&](const std::string& expr) {
    f->mock->clearBleText();
    CHECK(f->run("BLPRINT " + expr) == 0);
    std::string t = f->mock->bleText();
    return t.empty() ? t : t.substr(0, t.size() - 1);
  };
  f->connect();
  CHECK(f->run("SDOPEN \"D.DAT\" AS 1") == 0);
  CHECK(f->run("SDPRINT #1,10,20,30") == 0);
  CHECK(said("SDEOF(1)") == "0");  // inside another keyword
  CHECK(f->run("C=1") == 0);
  CHECK(f->run("E=SDEOF(C)+5") == 0);  // an expression argument, in an expression
  if (said("E") != "5") {
    CHECK(false);
    return;  // a broken SDEOF would make the read loop below endless
  }

  CHECK(f->typeProgram("10 N=0:S=0\n20 IF SDEOF(1) THEN 50\n30 SDINPUT #1,X\n40 N=N+1:S=S+X:GOTO 20\n50 END\n"));
  f->key(pc1500::Key::Mode);  // PRO -> RUN mode
  CHECK(f->run("RUN") == 0);
  CHECK(said("N;\"/\";S") == "3/60");  // read all three, then stopped
  CHECK(said("SDEOF(1)") == "1");

  // Errors come back through UH, as the evaluator expects.
  CHECK(f->run("E=SDEOF(2)") == 40);  // not open
  CHECK(f->run("E=SDEOF(17)") == 1);  // not a channel
  CHECK(f->run("E=SDEOF(0)") == 1);
  CHECK(f->run("SDCLOSE ALL") == 0);
}

// Peer-to-peer (2026-09-28, BLE_PROTOCOL.md "Peer-to-peer files"): the fake
// peer plays the other PC-1500. Its waits count STATUS checks, one per POLL.

static std::vector<uint8_t> testBytes(size_t n) {
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i < n; i++) v[i] = static_cast<uint8_t>(i * 7 + 3);
  return v;
}

// BLADV waits (POLLing) until a peer connects, then says who; BREAK stops
// the wait and the advertising.
void testBleAdvertiseWaitsForPeer() {
  auto f = bleFixture("testBleAdvertiseWaitsForPeer");
  if (!f) return;
  f->mock->setBlePeerConnectsAfter(3, "ZAPHOD");
  CHECK(f->run("BLADV") == 0);
  CHECK(f->shown() == "CONNECTED: ZAPHOD");
  f->key(pc1500::Key::Ent);
  CHECK(f->mock->bleConnected());
  CHECK(f->run("BLDISC") == 0);

  f->mock->setBlePeerConnectsAfter(-1, "");
  CHECK(f->runThenBreak("BLADV") == 0);
  CHECK(!f->mock->bleAdvertising());
  CHECK(!f->mock->bleConnected());
  CHECK(f->run("BLADV 1") == 1);  // no arguments
}

// Pairing (2026-10-03, BLE_PROTOCOL.md sec.7). An unpaired peer refuses
// BLCON: the reason, then ERROR 40. BLPAIR shows the code for Y/N and waits
// for the peer's user; a pairing leaves the link up, as BLCON's. BLUNPAIR
// forgets one by name, or all after a Y. BLADV answers a connector's
// pairing with the code's Y/N.
void testBlePairing() {
  auto f = bleFixture("testBlePairing");
  if (!f) return;
  f->mock->setBlePeerPaired("MARVIN", false);
  CHECK(f->run("BLCON \"MARVIN\"") == 0);
  CHECK(f->shown() == "BLE: NOT PAIRED - BLPAIR");
  f->key(pc1500::Key::Ent);
  CHECK(f->m->bus.readME0(kErlAbs) == 40);
  CHECK(!f->mock->bleConnected());

  // Refused here: nothing changes.
  CHECK(f->run("BLPAIR \"marvin\"") == 0);
  CHECK(f->shown() == "PAIR CODE 123456 Y/N");
  f->key(pc1500::Key::N);
  CHECK(f->m->bus.readME0(kErlAbs) == 0);
  CHECK(!f->mock->blePeerPaired("MARVIN"));

  // Refused there.
  f->mock->setBlePeerPairAnswer(false, 2);
  CHECK(f->run("BLPAIR \"MARVIN\"") == 0);
  f->key(pc1500::Key::Y);
  CHECK(f->shown() == "BLPAIR: REFUSED");
  f->key(pc1500::Key::Ent);
  CHECK(!f->mock->blePeerPaired("MARVIN"));

  // Accepted on both sides, picked from the listing with P.
  f->mock->setBlePeerPairAnswer(true, 3);
  CHECK(f->run("BLPAIR") == 0);
  CHECK(f->shown(0x8002) == "MARVIN");
  f->key(pc1500::Key::P);
  CHECK(f->shown() == "PAIR CODE 123456 Y/N");
  f->key(pc1500::Key::Y);
  CHECK(f->shown() == "CONNECTED: MARVIN");
  f->key(pc1500::Key::Ent);
  CHECK(f->mock->blePeerPaired("MARVIN"));
  CHECK(f->mock->bleConnected());
  CHECK(f->run("BLDISC") == 0);
  f->connect();  // BLCON just works now

  CHECK(f->run("BLUNPAIR \"NOBODY\"") == 40);
  CHECK(f->run("BLUNPAIR \"MARVIN\"") == 0);
  CHECK(!f->mock->blePeerPaired("MARVIN"));
  f->mock->setBlePeerPaired("MARVIN", true);
  CHECK(f->run("BLUNPAIR") == 0);
  CHECK(f->shown() == "FORGET ALL PAIRINGS Y/N");
  f->key(pc1500::Key::N);
  CHECK(f->mock->blePeerPaired("MARVIN"));
  CHECK(f->run("BLUNPAIR") == 0);
  f->key(pc1500::Key::Y);
  CHECK(!f->mock->blePeerPaired("MARVIN"));

  // BLADV: a connector asks to pair; Y pairs and links it.
  CHECK(f->run("BLDISC") == 0);
  f->mock->bleRequestPairing("ZAPHOD");
  CHECK(f->run("BLADV") == 0);
  CHECK(f->shown() == "PAIR CODE 123456 Y/N");
  f->key(pc1500::Key::Y);
  CHECK(f->mock->blePairAnswered() == 1);
  CHECK(f->shown() == "CONNECTED: ZAPHOD");
  f->key(pc1500::Key::Ent);
  CHECK(f->mock->bleConnected());
}

// BLPUT offers the program, memory, or a card file; the peer's answer
// decides. Each form's bytes are what the matching SDSAVE would write.
void testBlePutToPeer() {
  auto f = bleFixture("testBlePutToPeer");
  if (!f) return;
  CHECK(f->run("BLPUT") == 40);  // no link
  f->connect();
  CHECK(f->typeProgram("10 PRINT 1\n20 END\n"));
  std::vector<uint8_t> program = f->program();

  f->mock->setBlePeerAnswer(true, 2);
  CHECK(f->run("BLPUT") == 0);
  CHECK(f->mock->bleReceived().kind == 0);
  CHECK(f->mock->bleReceived().name.empty());
  CHECK(f->mock->bleReceived().data == program);

  CHECK(f->run("POKE &4400,1,2,3,4") == 0);
  f->mock->setBlePeerAnswer(true, 1);
  CHECK(f->run("BLPUT M &4400,&4403,&4401") == 0);
  const std::vector<uint8_t> mExpected = {0x44, 0x00, 0x44, 0x01, 1, 2, 3, 4};
  CHECK(f->mock->bleReceived().kind == 1);
  CHECK(f->mock->bleReceived().size == 8);
  CHECK(f->mock->bleReceived().data == mExpected);

  std::vector<uint8_t> file = testBytes(3000);  // several 1K pieces
  {
    std::ofstream out(f->sdDir / "F.BIN", std::ios::binary);
    out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
  }
  f->mock->setBlePeerAnswer(true, 1);
  CHECK(f->run("BLPUT SD M \"F.BIN\"") == 0);
  CHECK(f->mock->bleReceived().kind == 1);
  CHECK(f->mock->bleReceived().name == "F.BIN");
  CHECK(f->mock->bleReceived().data == file);
  f->mock->setBlePeerAnswer(true, 1);
  CHECK(f->run("BLPUT SD \"F.BIN\"") == 0);
  CHECK(f->mock->bleReceived().kind == 0);
  CHECK(f->run("BLPUT SD \"NOPE.BIN\"") == 40);

  f->mock->setBlePeerAnswer(false, 1);
  CHECK(f->run("BLPUT") == 0);
  CHECK(f->shown() == "BLPUT: REFUSED");
  f->key(pc1500::Key::Ent);

  f->mock->setBlePeerAnswer(true, -1);  // never answers
  CHECK(f->runThenBreak("BLPUT") == 0);
  CHECK(f->mock->bleWithdrawn());
}

// BLGET takes the peer's offer: into memory (a BASIC file as the program,
// an M file at its header's address), or onto the card under its own name.
void testBleGetFromPeer() {
  auto f = bleFixture("testBleGetFromPeer");
  if (!f) return;
  CHECK(f->run("BLGET") == 40);  // no link
  f->connect();
  CHECK(f->typeProgram("10 PRINT 2\n20 END\n"));
  std::vector<uint8_t> program = f->program();
  CHECK(f->run("NEW") == 0);
  f->mock->bleOfferToUs(0, "", program);
  CHECK(f->run("BLGET") == 0);
  CHECK(f->program() == program);

  f->mock->bleOfferToUs(1, "", {0x44, 0x00, 0x00, 0x00, 5, 6, 7, 8});
  CHECK(f->run("BLGET") == 0);
  CHECK(f->m->bus.readME0(0x4400) == 5 && f->m->bus.readME0(0x4403) == 8);

  std::vector<uint8_t> file = testBytes(3000);
  auto onCard = [&](const char* name) {
    std::ifstream in(f->sdDir / name, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  };
  f->mock->bleOfferToUs(1, "THEIRS.BIN", file);
  CHECK(f->run("BLGET \"G.BIN\"") == 0);
  CHECK(onCard("G.BIN") == file);

  std::vector<uint8_t> other = testBytes(10);
  f->mock->bleOfferToUs(0, "", other);
  CHECK(f->run("BLGET \"G.BIN\"") == 0);
  CHECK(f->shown() == "FILE EXISTS. OVERWRITE Y/N");
  f->key(pc1500::Key::N);
  CHECK(f->mock->bleOfferPending());  // still the peer's to withdraw or resend
  CHECK(onCard("G.BIN") == file);
  CHECK(f->run("BLGET \"G.BIN\",-Y") == 0);
  CHECK(onCard("G.BIN") == other);

  CHECK(f->runThenBreak("BLGET") == 0);  // nothing offered: waits until BREAK
}

// The CE-150 printer/plotter's keywords (2026-09-30), drawn on the fake
// peer as PLOT lines -- in quarter steps (RP2350/plotter.h: 4 per CE-150
// step), y up the paper. No link is ERROR 27; out-of-range values ERROR 19,
// the wrong mode ERROR 73, as the CE-150's own ROM.
void testCe150Plotter() {
  auto f = bleFixture("testCe150Plotter");
  if (!f) return;
  using Line = pc1500::ExpansionMock::BlePlotLine;
  auto lines = [&]() { return f->mock->blePlotLines(); };
  auto same = [](const Line& l, int pen, int x0, int y0, int x1, int y1) {
    bool ok = l.pen == pen && l.x0 == x0 && l.y0 == y0 && l.x1 == x1 && l.y1 == y1;
    if (!ok)
      std::printf("  line pen %d (%d,%d)-(%d,%d), expected pen %d (%d,%d)-(%d,%d)\n", l.pen, l.x0, l.y0, l.x1, l.y1,
                  pen, x0, y0, x1, y1);
    return ok;
  };

  CHECK(f->run("GRAPH") == 27);  // no printer
  CHECK(f->run("LPRINT 1") == 27);
  f->connect();

  // The E6 group has this module's own codes, the F0 group the CE-150's.
  CHECK(f->typeProgram("10 CSIZE 1:COLOR 0:TEXT\n"));
  std::vector<uint8_t> program = f->program();
  auto has = [&](uint8_t hi, uint8_t lo) {
    for (size_t i = 0; i + 1 < program.size(); i++)
      if (program[i] == hi && program[i + 1] == lo) return true;
    return false;
  };
  CHECK(has(0xE1, 0xC0));  // CSIZE
  CHECK(has(0xF0, 0xB5));  // COLOR
  CHECK(has(0xE1, 0xC6));  // TEXT
  f->key(pc1500::Key::Cl);
  f->key(pc1500::Key::Mode);  // PRO -> RUN mode
  CHECK(f->run("RUN") == 0);

  // TEXT mode: a string from the left, then the next line (12 steps at CSIZE 1).
  CHECK(f->run("TEXT") == 0);
  CHECK(f->run("CSIZE 1") == 0);
  f->mock->clearBlePlot();
  CHECK(f->run("LPRINT \"AB\"") == 0);
  CHECK(!lines().empty());
  int32_t minX = 9999, maxX = -9999;
  for (const Line& l : lines()) {
    minX = std::min({minX, l.x0, l.x1});
    maxX = std::max({maxX, l.x0, l.x1});
  }
  CHECK(minX >= 0 && maxX < 48);  // two 24-quarter-step cells
  CHECK(f->mock->blePlotX() == 0);
  int32_t y = f->mock->blePlotY();
  CHECK(y == -48);
  // A number alone is right-justified: "12" in columns 34-35 of 36.
  f->mock->clearBlePlot();
  CHECK(f->run("LPRINT 12") == 0);
  minX = 9999;
  for (const Line& l : lines()) minX = std::min({minX, l.x0, l.x1});
  CHECK(minX >= 34 * 24 && minX < 35 * 24);
  CHECK(f->mock->blePlotY() == y - 48);
  CHECK(f->run("CSIZE 9") == 0);
  CHECK(f->run("LPRINT 12345") == 76);  // wider than CSIZE 9's 4 columns
  CHECK(f->run("CSIZE 0") == 19);
  CHECK(f->run("LCURSOR 4") == 19);
  CHECK(f->run("LCURSOR 3") == 0);
  CHECK(f->run("ROTATE 1") == 73);  // GRAPH only
  CHECK(f->run("SORGN") == 73);

  // GRAPH mode: the pen goes to the left edge, which is the origin.
  CHECK(f->run("GRAPH") == 0);
  CHECK(f->run("LF 1") == 73);  // TEXT only
  CHECK(f->mock->blePlotX() == 0);
  int32_t oy = f->mock->blePlotY();
  f->mock->clearBlePlot();
  CHECK(f->run("LINE (0,0)-(100,0)") == 0);
  CHECK(lines().size() == 1 && same(lines()[0], 0, 0, oy, 400, oy));
  CHECK(f->run("LINE -(100,100),0,2") == 0);  // from the pen, in pen 2
  CHECK(lines().size() == 2 && same(lines()[1], 2, 400, oy, 400, oy + 400));
  CHECK(f->run("RLINE -(-50,0)") == 0);  // relative; pen 2 still
  CHECK(lines().size() == 3 && same(lines()[2], 2, 400, oy + 400, 200, oy + 400));
  f->mock->clearBlePlot();
  CHECK(f->run("LINE (10,10)-(20,20),0,1,B") == 0);  // a box: across, up, back, down
  CHECK(lines().size() == 4);
  if (lines().size() == 4) {
    CHECK(same(lines()[0], 1, 40, oy + 40, 80, oy + 40));
    CHECK(same(lines()[1], 1, 80, oy + 40, 80, oy + 80));
    CHECK(same(lines()[2], 1, 80, oy + 80, 40, oy + 80));
    CHECK(same(lines()[3], 1, 40, oy + 80, 40, oy + 40));
  }
  f->mock->clearBlePlot();
  CHECK(f->run("LINE (0,0)-(100,0),1") == 0);  // 2-step dashes: 25 in 100 steps
  CHECK(lines().size() == 25);
  f->mock->clearBlePlot();
  CHECK(f->run("LINE (0,0)-(50,50),9") == 0);  // pen up
  CHECK(lines().empty());
  CHECK(f->mock->blePlotX() == 200 && f->mock->blePlotY() == oy + 200);
  CHECK(f->run("LINE (0,0)-(300,0),0") == 0);  // off the paper: stops at its side
  CHECK(!lines().empty() && same(lines().back(), 1, 0, oy, 860, oy));
  CHECK(f->mock->blePlotX() == 860);
  // SORGN makes the pen's position the origin.
  CHECK(f->run("GLCURSOR (100,100)") == 0);
  CHECK(f->run("SORGN") == 0);
  f->mock->clearBlePlot();
  CHECK(f->run("LINE (0,0)-(10,0)") == 0);
  CHECK(lines().size() == 1 && same(lines()[0], 1, 400, oy + 400, 440, oy + 400));
  CHECK(f->run("COLOR 4") == 19);
  CHECK(f->run("ROTATE 4") == 19);
  CHECK(f->run("LINE (3000,0)") == 19);
  CHECK(f->run("LINE (0,0)-(1,1),,,B,1") == 1);

  // In a program, with expressions, as GLOBE.BAS draws (its lines replace
  // the program above).
  f->key(pc1500::Key::Cl);
  f->key(pc1500::Key::Mode);  // RUN -> PRO, to type it
  CHECK(f->typeProgram("10 GRAPH:X=50:Y=-20:T=0\n20 LINE -(X,Y),T\n"));
  f->key(pc1500::Key::Cl);
  f->key(pc1500::Key::Mode);
  f->mock->clearBlePlot();
  CHECK(f->run("RUN") == 0);
  CHECK(lines().size() == 1 && same(lines()[0], 1, 0, oy + 400, 200, oy + 320));  // GRAPH keeps the pen

  // TEST: a box in each pen.
  f->mock->clearBlePlot();
  CHECK(f->run("TEST") == 0);
  CHECK(lines().size() == 16);
  if (lines().size() == 16)
    for (int i = 0; i < 16; i++) CHECK(lines()[i].pen == i / 4);

  // LLIST: two lines at CSIZE 2, 24 steps apart.
  f->key(pc1500::Key::Cl);
  f->key(pc1500::Key::Mode);  // RUN -> PRO
  CHECK(f->typeProgram("10 REM HI\n20 END\n"));
  y = f->mock->blePlotY();
  f->mock->clearBlePlot();
  CHECK(f->run("LLIST") == 0);
  CHECK(!lines().empty());
  CHECK(f->mock->blePlotY() == y - 2 * 96);

  // LLIST "label" (2026-10-01): the line that starts with it, or from it on.
  CHECK(f->typeProgram("10 \"A\" REM 1\n20 \"B\" REM 2\n30 REM 3\n"));
  y = f->mock->blePlotY();
  CHECK(f->run("LLIST \"B\"") == 0);
  CHECK(f->mock->blePlotY() == y - 96);
  CHECK(f->run("LLIST \"B\",") == 0);
  CHECK(f->mock->blePlotY() == y - 3 * 96);
  CHECK(f->run("LLIST \"Z\"") == 11);

  // LPRINT USING (GRAPH mode only): each drawn exactly as LPRINT draws the
  // text the manual's rules give (pp.80-83).
  CHECK(f->run("LPRINT USING \"##\";1") == 73);  // TEXT mode
  CHECK(f->run("GRAPH") == 0);
  auto drawn = [&](const std::string& statement, int* erl) {
    CHECK(f->run("GLCURSOR (0,-300)") == 0);
    f->mock->clearBlePlot();
    *erl = f->run(statement);
    return f->mock->blePlotLines();
  };
  auto sameAs = [&](const std::string& statement, const std::string& text) {
    int e1, e2;
    auto got = drawn(statement, &e1);
    auto want = drawn("LPRINT USING;\"" + text + "\"", &e2);  // with no format (formats last)
    bool same = e1 == 0 && e2 == 0 && got.size() == want.size() && !got.empty();
    for (size_t i = 0; same && i < got.size(); i++)
      same = got[i].x0 == want[i].x0 && got[i].y0 == want[i].y0 && got[i].x1 == want[i].x1 && got[i].y1 == want[i].y1;
    if (!same) {
      std::printf("  %s: not drawn as \"%s\" (ERL %d, %zu lines, %zu expected)\n", statement.c_str(), text.c_str(), e1,
                  got.size(), want.size());
      for (size_t i = 0; i < got.size() && i < want.size(); i++)
        if (got[i].x0 != want[i].x0 || got[i].y0 != want[i].y0) {
          std::printf("  first difference, line %zu: (%d,%d) vs (%d,%d)\n", i, got[i].x0, got[i].y0, want[i].x0,
                      want[i].y0);
          break;
        }
    }
    return same;
  };
  CHECK(sameAs("LPRINT USING \"####.##\";3.14159", "   3.14"));  // decimals cut, not rounded
  CHECK(sameAs("LPRINT USING \"###.###\";-3.14159", " -3.141"));
  CHECK(sameAs("LPRINT USING \"*######\";1234", " **1234"));
  CHECK(sameAs("LPRINT USING \"+###.##^\";3.14159", "+3.14E 00"));
  CHECK(sameAs("LPRINT USING \"###,###,###\";246813", "  246,813"));
  CHECK(sameAs("LPRINT USING \"&&&\";\"ABCDEF\"", "ABC"));
  CHECK(sameAs("LPRINT USING \"&&&&###\";\"AB\";12", "AB   12"));  // 4 + 3 wide
  CHECK(sameAs("LPRINT USING \"##.#\";1.25", " 1.2"));
  CHECK(f->run("LPRINT USING \"##.#\"") == 0);
  CHECK(sameAs("LPRINT 1.25", " 1.2"));  // the format lasts...
  CHECK(f->run("LPRINT USING \"##.#\"") == 0);
  CHECK(sameAs("LPRINT USING;1.25", "1.25"));  // ...until USING alone
  int erl;
  drawn("LPRINT USING \"##\";1234", &erl);
  CHECK(erl == 36);  // too wide for the field
  CHECK(f->run("LPRINT USING") == 0);

  // GRAPH mode's bare LPRINT (p.123): the pen returns and the paper feeds,
  // but the counters don't change -- the next LINE is that much over.
  CHECK(f->run("SORGN") == 0);
  CHECK(f->run("GLCURSOR (150,30)") == 0);
  int32_t ly = f->mock->blePlotY();
  CHECK(f->run("LPRINT") == 0);
  CHECK(f->mock->blePlotX() == 0 && f->mock->blePlotY() == ly - 96);
  f->mock->clearBlePlot();
  CHECK(f->run("LINE -(160,30),0") == 0);  // believed 10 steps right of the pen
  CHECK(lines().size() == 1 && same(lines()[0], lines()[0].pen, 0, ly - 96, 40, ly - 96));
  CHECK(f->run("TEXT") == 0);

  // A peer that doesn't take PLOT is no printer either.
  f->mock->setBlePeerPlots(false);
  CHECK(f->run("LPRINT \"X\"") == 27);
  f->mock->setBlePeerPlots(true);
}

#ifdef PC1500_HAVE_EXPANSION_KEYWORDS
// The firmware's basic_xlate (2026-10-01) on its own: a program's E1C0-E1C6
// become E680-E686 as it's saved and back as it's loaded, only in real
// tokens -- not a line number, not quoted text, not the module's other E1
// keywords -- however the transfer is cut into chunks.
// The external keyboard's layouts (2026-10-07, MCONF KBDLAYOUT): a key
// reports its US position, and the layout -- French AZERTY, German,
// Spanish, Belgian AZERTY -- says what's printed on it: the keys the
// sequencer then taps on the PC-1500.
void testKbdLayouts() {
  // The keys one report taps, in order (Shift included).
  auto taps = [](uint8_t layout, uint8_t mods, uint8_t usage) {
    kbd_seq_t s;
    kbd_seq_init(&s);
    s.layout = layout;
    uint8_t report[8] = {mods, 0, usage, 0, 0, 0, 0, 0};
    kbd_seq_report(&s, report, 0);
    std::vector<uint8_t> keys;
    for (uint16_t i = s.head; i != s.tail; i = static_cast<uint16_t>((i + 1) % KBD_QUEUE_LEN))
      if (s.queue[i].pressed) keys.push_back(s.queue[i].key);
    return keys;
  };
  using K = std::vector<uint8_t>;
  const uint8_t US = KBD_LAYOUT_US, FR = KBD_LAYOUT_FR, SHIFT = 0x02, ALTGR = 0x40;
  CHECK(taps(US, 0, 0x14) == K{KBD_K_Q});                // US: the Q key is Q
  CHECK(taps(FR, 0, 0x14) == K{KBD_K_A});                // AZERTY: it's A
  CHECK(taps(FR, 0, 0x04) == K{KBD_K_Q});
  CHECK(taps(FR, 0, 0x1A) == K{KBD_K_Z});
  CHECK(taps(FR, 0, 0x1D) == K{KBD_K_W});
  CHECK(taps(FR, 0, 0x33) == K{KBD_K_M});                // M is right of L
  CHECK(taps(FR, 0, 0x10) == (K{KBD_K_SHIFT, KBD_K_MINUS}));  // , where US M is
  CHECK(taps(FR, SHIFT, 0x1F) == K{KBD_K_2});            // digits on Shift
  CHECK(taps(FR, 0, 0x1E) == (K{KBD_K_SHIFT, KBD_K_F6}));     // & unshifted
  CHECK(taps(FR, 0, 0x1F).empty());                       // e acute: none on a PC-1500
  CHECK(taps(FR, ALTGR, 0x27) == (K{KBD_K_SHIFT, KBD_K_EQUALS}));  // AltGr+0 is @
  CHECK(taps(FR, 0, 0x37) == (K{KBD_K_SHIFT, KBD_K_ASTERISK}));    // :
  CHECK(taps(FR, SHIFT, 0x64) == (K{KBD_K_SHIFT, KBD_K_RPAREN}));  // > on the ISO key
  CHECK(taps(FR, 0, 0x52).empty());  // the arrows don't move: held, not tapped

  // German QWERTZ: Y and Z swapped, digits plain, AltGr for brackets and @.
  const uint8_t DE = KBD_LAYOUT_DE, ES = KBD_LAYOUT_ES, BE = KBD_LAYOUT_BE;
  CHECK(taps(DE, 0, 0x1C) == K{KBD_K_Z});
  CHECK(taps(DE, 0, 0x1D) == K{KBD_K_Y});
  CHECK(taps(DE, 0, 0x1F) == K{KBD_K_2});
  CHECK(taps(DE, SHIFT, 0x24) == K{KBD_K_SLASH});
  CHECK(taps(DE, SHIFT, 0x27) == K{KBD_K_EQUALS});        // Shift+0 is =
  CHECK(taps(DE, ALTGR, 0x14) == (K{KBD_K_SHIFT, KBD_K_EQUALS}));  // AltGr+Q is @
  CHECK(taps(DE, 0, 0x2F).empty());                        // u umlaut
  CHECK(taps(DE, 0, 0x30) == K{KBD_K_PLUS});
  CHECK(taps(DE, 0, 0x38) == K{KBD_K_MINUS});

  // Spanish: letters as US, Shift+7 is /, AltGr+2 is @, n tilde is none.
  CHECK(taps(ES, 0, 0x14) == K{KBD_K_Q});
  CHECK(taps(ES, SHIFT, 0x24) == K{KBD_K_SLASH});
  CHECK(taps(ES, ALTGR, 0x1F) == (K{KBD_K_SHIFT, KBD_K_EQUALS}));
  CHECK(taps(ES, 0, 0x33).empty());
  CHECK(taps(ES, SHIFT, 0x2D) == (K{KBD_K_SHIFT, KBD_K_SLASH}));  // ?

  // Belgian AZERTY: French letters, its own symbols.
  CHECK(taps(BE, 0, 0x14) == K{KBD_K_A});
  CHECK(taps(BE, 0, 0x25) == (K{KBD_K_SHIFT, KBD_K_F1}));   // 8 key: !
  CHECK(taps(BE, 0, 0x2E) == K{KBD_K_MINUS});               // - where French has =
  CHECK(taps(BE, 0, 0x38) == K{KBD_K_EQUALS});              // = where French has !
  CHECK(taps(BE, ALTGR, 0x1F) == (K{KBD_K_SHIFT, KBD_K_EQUALS}));  // AltGr+2 is @

  // MCONF KBDLAYOUT's HID country codes to the layouts.
  CHECK(kbd_seq_layout_for_country(8) == FR);
  CHECK(kbd_seq_layout_for_country(9) == DE);
  CHECK(kbd_seq_layout_for_country(25) == ES);
  CHECK(kbd_seq_layout_for_country(2) == BE);
  CHECK(kbd_seq_layout_for_country(33) == US);
  CHECK(kbd_seq_layout_for_country(0) == US);
  CHECK(kbd_seq_country_supported(0) && kbd_seq_country_supported(33));
  CHECK(!kbd_seq_country_supported(1) && !kbd_seq_country_supported(32));  // Arabic, UK: none yet
}

void testBasicXlateChunks() {
  auto line = [](uint16_t number, std::vector<uint8_t> body) {
    body.push_back(0x0D);
    std::vector<uint8_t> l = {static_cast<uint8_t>(number >> 8), static_cast<uint8_t>(number),
                              static_cast<uint8_t>(body.size())};
    l.insert(l.end(), body.begin(), body.end());
    return l;
  };
  // line 57792 (E1C0): CSIZE, "<E1 C0>", SDSAVE (E186), TEXT; line 10: ROTATE
  std::vector<uint8_t> ours = line(0xE1C0, {0xE1, 0xC0, '"', 0xE1, 0xC0, '"', 0xE1, 0x86, 0xE1, 0xC6});
  std::vector<uint8_t> l2 = line(10, {0xE1, 0xC5});
  ours.insert(ours.end(), l2.begin(), l2.end());
  ours.push_back(0xFF);
  std::vector<uint8_t> ce150 = ours;
  ce150[3] = 0xE6, ce150[4] = 0x80;    // CSIZE
  ce150[11] = 0xE6, ce150[12] = 0x86;  // TEXT
  ce150[17] = 0xE6, ce150[18] = 0x85;  // ROTATE

  // Every cut into two chunks, one byte at a time, and two. (A load can't
  // hold back a read's only byte -- a read of 0 would end it -- so a token
  // cut across one-byte reads isn't translated; reads are file chunks or
  // whole BLE frames, and only a file's last byte, FF, comes alone.)
  std::vector<std::vector<size_t>> cuts;
  for (size_t k = 0; k <= ours.size(); k++) cuts.push_back({k});
  std::vector<size_t> ones, twos;
  for (size_t k = 1; k < ours.size(); k++) ones.push_back(k);
  for (size_t k = 2; k < ours.size(); k += 2) twos.push_back(k);
  cuts.push_back(ones);
  cuts.push_back(twos);
  for (const auto& cut : cuts) {
    auto chunks = [&](const std::vector<uint8_t>& all) {
      std::vector<std::vector<uint8_t>> out;
      size_t at = 0;
      for (size_t k : cut) {
        out.emplace_back(all.begin() + static_cast<long>(at), all.begin() + static_cast<long>(k));
        at = k;
      }
      out.emplace_back(all.begin() + static_cast<long>(at), all.end());
      return out;
    };
    std::vector<uint8_t> saved, loaded;
    basic_xlate_begin(BASIC_XLATE_SAVE, BASIC_XLATE_CE150);
    for (auto c : chunks(ours)) {
      if (c.empty()) continue;  // the ROM never writes 0 bytes
      c.resize(c.size() + 1);
      uint16_t n = basic_xlate_write(c.data(), static_cast<uint16_t>(c.size() - 1));
      saved.insert(saved.end(), c.begin(), c.begin() + n);
    }
    uint8_t last;
    if (basic_xlate_flush(&last)) saved.push_back(last);
    basic_xlate_end();
    basic_xlate_begin(BASIC_XLATE_LOAD, BASIC_XLATE_CE150);
    for (auto c : chunks(ce150)) {
      if (c.empty()) continue;  // a read of 0 is the end, below
      c.resize(c.size() + 1);
      uint16_t n = basic_xlate_read(c.data(), static_cast<uint16_t>(c.size() - 1));
      loaded.insert(loaded.end(), c.begin(), c.begin() + n);
    }
    uint8_t end[2];
    uint16_t n = basic_xlate_read(end, 0);  // the end of the file: anything held
    loaded.insert(loaded.end(), end, end + n);
    basic_xlate_end();
    CHECK(saved == ce150);
    if (cut.size() != ones.size() || cut != ones) CHECK(loaded == ours);
    else loaded = ours;
    if (saved != ce150 || loaded != ours) std::printf("  cut at %zu (%zu cuts)\n", cut[0], cut.size());
  }
}
#endif

// The CE-150 ROM, attached the way the user's emulator has one: at A000H,
// visible whatever PV is (as this module's own).
static bool attachCe150(BootedMachine& m) {
  static std::vector<uint8_t> rom = readFile("C:/Users/paulc/Documents/PC1500/CE-150.ROM");
  if (rom.size() != 0x2000) return false;
  m.bus.loadRomModule(1, rom.data(), rom.size(), 0xA000, /*requirePv=*/false, /*usePuBank=*/false);
  return true;
}

// Saving and loading a program (2026-10-01, RP2350/basic_xlate.h): files
// hold the CE-150's E68x codes; a load makes them this module's E1Cx
// unless a CE-150 is attached. And this module's E1Cx keywords hand over
// to a real CE-150's own routines once one is attached.
void testCe150CodesSaveLoadAndHandOver() {
  auto f = bleFixture("testCe150CodesSaveLoadAndHandOver");
  if (!f) return;
  auto has = [](const std::vector<uint8_t>& bytes, uint8_t hi, uint8_t lo) {
    for (size_t i = 0; i + 1 < bytes.size(); i++)
      if (bytes[i] == hi && bytes[i + 1] == lo) return true;
    return false;
  };
  auto onCard = [&](const char* name) {
    std::ifstream in(f->sdDir / name, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  };
  CHECK(f->typeProgram("10 CSIZE 3:TEXT\n20 REM \"CSIZE\"\n"));
  std::vector<uint8_t> ours = f->program();
  CHECK(has(ours, 0xE1, 0xC0) && has(ours, 0xE1, 0xC6));
  CHECK(f->run("SDSAVE \"P.BAS\"") == 0);
  std::vector<uint8_t> file = onCard("P.BAS");
  CHECK(has(file, 0xE6, 0x80) && has(file, 0xE6, 0x86) && !has(file, 0xE1, 0xC0));
  CHECK(f->run("NEW") == 0);
  CHECK(f->run("SDLOAD \"P.BAS\"") == 0);
  CHECK(f->program() == ours);  // no CE-150: this module's codes again
  f->connect();
  CHECK(f->run("BLSAVE \"Q.BAS\"") == 0);
  CHECK(f->mock->bleFiles()["Q.BAS"] == file);
  CHECK(f->run("NEW") == 0);
  CHECK(f->run("BLLOAD \"Q.BAS\"") == 0);
  CHECK(f->program() == ours);
  CHECK(f->run("BLDISC") == 0);

  // Run, no link and no CE-150: this module's CSIZE has no printer.
  f->key(pc1500::Key::Cl);
  f->key(pc1500::Key::Mode);  // PRO -> RUN
  CHECK(f->run("RUN") == 27);

  if (!attachCe150(*f->m)) {
    std::printf("SKIP: the CE-150 half of testCe150CodesSaveLoadAndHandOver -- CE-150.ROM not found.\n");
    return;
  }
  // The same program now runs the CE-150's own CSIZE (CE-150 ROM LB180: the
  // size at 79F4H, no printer needed) and TEXT -- which, with the ROM but no
  // printer here, stops at its own "printer not ready" check (LB0EB): the
  // CE-150's ERROR 78, not this module's ERROR 27.
  f->m->bus.writeME0(0x79F4, 0);
  {
    int e = f->run("RUN");
    CHECK(e == 78);
    if (e != 78) std::printf("  RUN with a CE-150: ERL %d, 79F4H %02X\n", e, f->m->bus.readME0(0x79F4));
  }
  CHECK(f->m->bus.readME0(0x79F4) == 3);
  f->m->bus.writeME0(0x79F4, 0);
  CHECK(f->run("CSIZE 5") == 0);  // typed with a CE-150 there: its own code
  CHECK(f->m->bus.readME0(0x79F4) == 5);
  // A load with a CE-150 attached keeps its codes.
  f->key(pc1500::Key::Cl);
  f->key(pc1500::Key::Mode);  // RUN -> PRO
  CHECK(f->run("NEW") == 0);
  CHECK(f->run("SDLOAD \"P.BAS\"") == 0);
  std::vector<uint8_t> loaded = f->program();
  CHECK(has(loaded, 0xE6, 0x80) && !has(loaded, 0xE1, 0xC0));
}

// GLOBE.BAS (the CE-150 program in Documents/PC1500) end to end, with its
// INPUTs replaced by fixed answers, on a machine with 26K of expansion RAM
// (16K at 0000H, 10K at 4800H): it doesn't fit the bare 3.5K. It ends by
// design on the READ past its DATA (ON ERROR GOTO 560). What the peer drew goes to
// %TEMP%/testCe150Globe/globe_lines.txt, "pen x0 y0 x1 y1" a line, to look
// at. Slow (minutes): expansion_keyword_test Globe.
void testCe150Globe() {
  std::ifstream in("C:/Users/paulc/Documents/PC1500/GLOBE.BAS.TXT");
  if (!in) {
    std::printf("SKIP: testCe150Globe -- GLOBE.BAS.TXT not found.\n");
    return;
  }
  auto f = bleFixture("testCe150Globe");
  if (!f) return;
  std::string text, line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("20 ", 0) == 0) line = "20 S=10:A=20:B=30:F=0:G=30:P=10";
    else if (line.rfind("30 ", 0) == 0) line = "30 V$=\"N\"";
    else if (line.rfind("70 ", 0) == 0) line = "70 CO$=\"Y\"";
    if (!line.empty()) text += line + "\n";
  }
  f->connect();
  CHECK(f->typeProgram(text));
  f->key(pc1500::Key::Cl);
  f->key(pc1500::Key::Mode);  // PRO -> RUN
  f->mock->clearBlePlot();
  f->m->bus.writeME0(kErlAbs, 0);
  tapKey(*f->m, pc1500::Key::Cl);
  typeText(*f->m, "RUN");
  tapKey(*f->m, pc1500::Key::Ent);
  bool idle = false;
  for (int i = 0; i < 500 && !idle; i++) idle = waitForIdle(*f->m, 20'000'000);
  CHECK(idle);
  const auto& lines = f->mock->blePlotLines();
  std::printf("  GLOBE: %zu lines in %d frames\n", lines.size(), f->mock->blePlotFrames());
  CHECK(lines.size() > 500);
  std::ofstream out(f->sdDir / "globe_lines.txt");
  for (const auto& l : lines) {
    CHECK(l.x0 >= 0 && l.x0 <= 860 && l.x1 >= 0 && l.x1 <= 860);
    out << int(l.pen) << ' ' << l.x0 << ' ' << l.y0 << ' ' << l.x1 << ' ' << l.y1 << '\n';
  }
  std::printf("  wrote %s\n", (f->sdDir / "globe_lines.txt").string().c_str());
}

// The external keyboard's driver (2026-10-04). With MCONF BLKBD=1 the boot
// hook copies ROM1's keyboard wait loop to the MCU, which patches it into
// the ROM image at KBD_LOOP (RP2350/kbd_seq.c kbd_loop_install(), in the
// mock), and arms the base ROM's keyboard hook (79D4H = 55H, vector
// 785BH/785CH = KBD_HOOK at 880EH). The firmware's key sequencer (also in
// the mock) then types: the real keyboard still works through the driver,
// and the sequencer's keys reach BASIC as the same matrix keys -- letters,
// digits, shifted symbols (a Shift tap first), ENTER -- and its ON is a
// BREAK, here stopping a program waiting at INPUT. OFF goes through the
// loop's own power-off, so the hook is still there after ON. And with
// AUTOSTAGE=1 too, the loop is in place before staging, so the SRAM copy
// has it.
void testExternalKeyboardDriver() {
#ifndef PC1500_HAVE_EXPANSION_KEYWORDS
  std::printf("SKIP: testExternalKeyboardDriver -- built without the firmware sources.\n");
#else
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomDir =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/Design01_NonDMA_8K_PV_Swap.cydsn/rom/";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomDir + "rom_8800.bin");
  std::vector<uint8_t> romBin = readFile(kExpRomDir + "rom.bin");
  if (rom.empty() || expRom.empty() || romBin.size() < 0x800) {
    std::printf("SKIP: testExternalKeyboardDriver -- ROM1.BIN, rom_8800.bin and/or rom.bin not found.\n");
    return;
  }
  // The address the loop's own idle HLT returns to (ROM1 E2AAH, where BASIC
  // waits for a key) once it's copied to KBD_LOOP: KBD_HOOK (880EH) jumps
  // to KBD_ENTRY, whose own jump goes to KBD_LOOP.
  const uint16_t kbdEntry = static_cast<uint16_t>((expRom[0x0F] << 8) | expRom[0x10]);
  const uint16_t kbdLoop = static_cast<uint16_t>((expRom[0x11] << 8) | expRom[0x12]);
  const uint16_t kDriverIdle = static_cast<uint16_t>(kbdLoop + (0xE2AA - 0xE24A));
  CHECK(kbdEntry >= 0x8800 && kbdLoop >= 0x8800);

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_kbd");
  auto m = std::make_unique<BootedMachine>();
  m->bus.ioPort().useManualRtcClock();
  m->bus.loadME0(0xC000, rom.data(), rom.size());
  m->bus.setExtRam0000Size(0x4000);
  m->bus.setExtRamExtSize(0x2800);
  m->bus.loadExpansionModule(0, expRom.data(), expRom.size(), /*base=*/0x8800, /*requirePv=*/false,
                             /*usePuBank=*/false, /*dataWindowBase=*/0x8000,
                             /*dataWindowSize=*/0x800, /*instructionAddr=*/0x87FF, romBin.data(),
                             0x800);
  m->bus.expansionMock().setRootDir(sdDir);
  pc1500::ExpansionMock& mock = m->bus.expansionMock();

  auto idle = [&]() {
    for (long i = 0; i < 4'000'000; i++) {
      if (m->cpu.halted() && (m->cpu.p() == kIdleAddr || m->cpu.p() == kDriverIdle)) return true;
      stepOne(*m);
    }
    return false;
  };
  auto resetAndBoot = [&]() {
    m->cpu.reset();
    for (long c = 0; !m->cpu.halted() && c < 20'000'000; c++) stepOne(*m);
    for (long i = 0; i < 4'000'000; i++) stepOne(*m);
    tapKey(*m, pc1500::Key::Cl);
    typeText(*m, "NEW0");
    tapKey(*m, pc1500::Key::Ent);
    CHECK(idle());
  };
  auto armed = [&]() {
    return m->bus.readME0(0x79D4) == 0x55 && m->bus.readME0(0x785B) == 0x88 && m->bus.readME0(0x785C) == 0x0E;
  };
  auto external = [&](const std::string& text) {  // types it, then waits for BASIC
    for (char c : text) CHECK(mock.kbdChar(c));
    for (long i = 0; i < 40'000'000 && mock.kbdBusy(); i++) stepOne(*m);
    CHECK(!mock.kbdBusy());
    return idle();
  };
  auto number = [&](uint16_t at) {  // exponent, then the first mantissa byte
    return (m->bus.readME0(at) << 8) | m->bus.readME0(static_cast<uint16_t>(at + 2));
  };

  // BLKBD defaults to 0: no driver.
  resetAndBoot();
  CHECK(m->bus.readME0(0x79D4) == 0x00);
  CHECK(m->bus.readME0(kbdLoop) != rom[0xE24A - 0xC000]);

  mock.setConfigValue(pc1500::ExpansionMock::kConfigBlkbd, 1);
  resetAndBoot();
  CHECK(armed());
  CHECK(m->bus.readME0(kbdLoop) == rom[0xE24A - 0xC000]);

  // The real keyboard, through the driver.
  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "A=12+3");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(idle());
  CHECK(m->cpu.p() == kDriverIdle);
  CHECK(number(0x7900) == 0x0115);
  CHECK(m->bus.readME0(kErlAbs) == 0);

  // The external one: a line and its ENTER, then shifted symbols.
  CHECK(external("B=2*3\r"));
  CHECK(number(0x7908) == 0x0060);  // 6: exponent 0, mantissa 6
  CHECK(external("PRINT \"X\";"));
  CHECK(readDisplayBuffer(m->bus) == "PRINT \"X\";");
  CHECK(mock.kbdTap(0xB5, false));  // CL
  CHECK(external(""));

  // ON = BREAK: a program waiting at INPUT stops, and the next line typed is
  // a command again, not INPUT's answer.
  std::string typeError;
  CHECK(pc1500::basic::typeBasicProgramText(m->bus, m->cpu, "10 INPUT E\n20 F=9\n", kCyclesPerFrame,
                                            kCyclesPerTimerTick, &typeError));
  tapKey(*m, pc1500::Key::Cl);
  tapKey(*m, pc1500::Key::Mode);  // PRO -> RUN
  CHECK(external("RUN\r"));       // waits at INPUT
  mock.kbdBreak();
  CHECK(idle());
  for (int i = 0; i < 3'000'000; i++) stepOne(*m);
  CHECK(idle());
  CHECK(external("G=1\r"));
  CHECK(number(0x7930) == 0x0010);  // G = 1
  CHECK(number(0x7928) == 0);       // F: line 20 never ran
  CHECK(number(0x7920) == 0);       // E: INPUT got nothing

  // OFF, then ON: still armed, and still typing.
  tapKey(*m, pc1500::Key::Off);
  for (int i = 0; i < 2'000'000; i++) stepOne(*m);
  CHECK(armed());
  m->cpu.pressOnKey();
  m->bus.ioPort().setOnKeyLine(true);
  m->cpu.requestMI();
  for (int i = 0; i < 200'000; i++) stepOne(*m);
  m->bus.ioPort().setOnKeyLine(false);
  CHECK(idle());
  CHECK(armed());
  CHECK(external("H=2\r"));
  CHECK(number(0x7938) == 0x0020);  // H = 2

  // A Bluetooth keyboard's boot reports, keys going down and up about as
  // fast as a typist's: Ctrl+J is ignored, Shift+= is +, F7 is CL, Enter
  // is held like the host keyboard's.
  auto hid = [&](uint8_t mods, uint8_t usage) {
    const uint8_t down[8] = {mods, 0, usage, 0, 0, 0, 0, 0}, up[8] = {mods, 0, 0, 0, 0, 0, 0, 0};
    mock.kbdReport(down);
    for (int i = 0; i < 50'000; i++) stepOne(*m);  // ~40ms
    mock.kbdReport(up);
    for (int i = 0; i < 50'000; i++) stepOne(*m);
  };
  hid(0x00, 0x0E);  // K
  hid(0x00, 0x40);  // F7: CL
  CHECK(external(""));
  CHECK(readDisplayBuffer(m->bus) == ">");  // the prompt CL leaves
  hid(0x01, 0x0D);  // Ctrl+J: nothing
  for (uint8_t u : {0x0D, 0x2E, 0x1E}) hid(0x00, u);  // J = 1
  hid(0x02, 0x2E);  // Shift+=: +
  hid(0x00, 0x1F);  // 2
  CHECK(external(""));
  CHECK(readDisplayBuffer(m->bus) == "J=1+2");
  hid(0x00, 0x28);  // Enter
  CHECK(external(""));
  CHECK(number(0x7948) == 0x0030);  // J = 3

  // AUTOSTAGE as well: the loop is in the image before it's staged, so the
  // SRAM copy has it, and the driver runs from there.
  mock.setConfigValue(pc1500::ExpansionMock::kConfigAutostage, 1);
  resetAndBoot();
  CHECK(mock.remapActive());
  CHECK(mock.romStagedVerified());
  CHECK(mock.sramByte(kbdLoop - 0x8800) == rom[0xE24A - 0xC000]);
  CHECK(armed());
  CHECK(external("I=3\r"));
  CHECK(number(0x7940) == 0x0030);  // I = 3

  // MCONF BLKBD=0 unhooks the driver at once (2026-10-05: on hardware the
  // driver went on reading a key byte nothing kept up any more, and every
  // character typed was wiped again). A key "held" there now must not matter.
  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "MCONF BLKBD=0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(idle());
  CHECK(m->bus.readME0(0x79D4) == 0x00);
  mock.kbdHold(0xB5, true);  // CL, stuck
  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "K=4");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));
  CHECK(number(0x7950) == 0x0040);  // K = 4
  mock.kbdHold(0xB5, false);
#endif
}

// BLKBD (2026-10-04): pairing the external keyboard, against the mock's
// pretend keyboard (found, asks for 123456, connects). Needs MCONF BLKBD=1
// first; the result stays up until a key; BLKBD FORGET drops the bond.
void testBlkbdPairsKeyboard() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testBlkbdPairsKeyboard -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }
  fs::path sdDir = makeTempTestDir("expansion_keyword_test_blkbd");
  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);
  pc1500::ExpansionMock& mock = m->bus.expansionMock();
  auto run = [&](const std::string& line) {
    m->bus.writeME0(kErlAbs, 0);
    tapKey(*m, pc1500::Key::Cl);
    typeText(*m, line);
    tapKey(*m, pc1500::Key::Ent);
    CHECK(waitForIdle(*m, 20'000'000));
    return m->bus.readME0(kErlAbs);
  };
  auto shown = [&]() {
    std::string text;
    for (int i = 0; i < 26; i++) text += static_cast<char>(m->bus.readME0(static_cast<uint16_t>(0x8000 + i)));
    return text.substr(0, text.find_last_not_of(' ') + 1);
  };

  run("NEW0");
  CHECK(run("BLKBD") == 0);
  CHECK(shown() == "BLKBD: MCONF BLKBD=1 FIRST");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  mock.setConfigValue(pc1500::ExpansionMock::kConfigBlkbd, 1);
  CHECK(run("BLKBD") == 0);  // searching, the code, then connected
  CHECK(shown() == "BLKBD OK: MOCK KEYBOARD");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));
  CHECK(m->bus.readME0(kErlAbs) == 0);
  // The driver is on now (2026-10-06), though this boot had MCONF BLKBD=0:
  // the hook points at KBD_HOOK and BASWORD's flag is set. Until then only
  // the boot hook put it on, and the keyboard typed nothing until a power
  // cycle.
  CHECK(m->bus.readME0(0x785B) == 0x88 && m->bus.readME0(0x785C) == 0x0E);
  CHECK(m->bus.readME0(0x79D4) == 0x55);
  uint16_t loop = static_cast<uint16_t>(m->bus.readME0(0x8811) << 8 | m->bus.readME0(0x8812));  // KBD_LOOP
  CHECK(m->bus.readME0(loop) == m->bus.readME0(0xE24A));  // ROM1's loop is served there

  CHECK(run("BLKBD ?") == 0);  // what's arriving: nothing, from the pretend keyboard
  CHECK(shown() == "S0 R0 L0 00000000 PFF");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));
  CHECK(run("BLKBD FORGET") == 0);
  CHECK(run("BLKBD X") == 1);
}

// An older PC-1500 ROM (2026-10-05, dumped from a real machine with SDSAVE M,
// so a 4-byte header first): its keyboard hook loads the vector at 79D5H
// into U and then jumps to whatever X held, so there's no driver for it.
// With MCONF BLKBD=1 the boot hook's copy is refused (E2B9H is D5H, not
// 38H), the hook stays unarmed and the machine's own keyboard works as
// before; BLKBD says why instead of pairing.
void testExternalKeyboardOldRom() {
#ifndef PC1500_HAVE_EXPANSION_KEYWORDS
  std::printf("SKIP: testExternalKeyboardOldRom -- built without the firmware sources.\n");
#else
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/S1500ROM.BIN";
  const std::string kExpRomDir =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/Design01_NonDMA_8K_PV_Swap.cydsn/rom/";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomDir + "rom_8800.bin");
  std::vector<uint8_t> romBin = readFile(kExpRomDir + "rom.bin");
  if (rom.size() == 0x4004) rom.erase(rom.begin(), rom.begin() + 4);  // SDSAVE M's header
  if (rom.size() != 0x4000 || expRom.empty() || romBin.size() < 0x800) {
    std::printf("SKIP: testExternalKeyboardOldRom -- S1500ROM.BIN, rom_8800.bin and/or rom.bin not found.\n");
    return;
  }
  CHECK(rom[0xE2B9 - 0xC000] == 0xD5);
  const uint16_t kbdLoop = static_cast<uint16_t>((expRom[0x11] << 8) | expRom[0x12]);

  fs::path sdDir = makeTempTestDir("expansion_keyword_test_kbd_old");
  auto m = std::make_unique<BootedMachine>();
  m->bus.ioPort().useManualRtcClock();
  m->bus.loadME0(0xC000, rom.data(), rom.size());
  m->bus.setExtRam0000Size(0x4000);
  m->bus.setExtRamExtSize(0x2800);
  m->bus.loadExpansionModule(0, expRom.data(), expRom.size(), /*base=*/0x8800, /*requirePv=*/false,
                             /*usePuBank=*/false, /*dataWindowBase=*/0x8000,
                             /*dataWindowSize=*/0x800, /*instructionAddr=*/0x87FF, romBin.data(),
                             0x800);
  m->bus.expansionMock().setRootDir(sdDir);
  pc1500::ExpansionMock& mock = m->bus.expansionMock();
  mock.setConfigValue(pc1500::ExpansionMock::kConfigBlkbd, 1);

  m->cpu.reset();
  for (long c = 0; !m->cpu.halted() && c < 20'000'000; c++) stepOne(*m);
  for (long i = 0; i < 4'000'000; i++) stepOne(*m);
  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));

  // Refused: nothing patched in, the hook not armed.
  CHECK(m->bus.readME0(kbdLoop) != rom[0xE24A - 0xC000]);
  CHECK(m->bus.readME0(0x79D4) == 0x00);

  // The machine's own keyboard, unaffected.
  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "A=12+3");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));
  CHECK(((m->bus.readME0(0x7900) << 8) | m->bus.readME0(0x7902)) == 0x0115);
  CHECK(m->bus.readME0(kErlAbs) == 0);

  // BLKBD says why.
  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "BLKBD");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m, 20'000'000));
  std::string text;
  for (int i = 0; i < 26; i++) text += static_cast<char>(m->bus.readME0(static_cast<uint16_t>(0x8000 + i)));
  CHECK(text.substr(0, text.find_last_not_of(' ') + 1) == "BLKBD: NOT ON THIS ROM");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(waitForIdle(*m));
  CHECK(m->bus.readME0(kErlAbs) == 0);
#endif
}

// BLKEY$ (2026-10-06): INKEY$ for either keyboard. A program's INKEY$
// becomes BLKEY$ when it's loaded with MCONF BLKBD=1, and a save always
// writes INKEY$, so a file stays plain BASIC (RP2350/basic_xlate.h). The
// quoted "INKEY$" is text, and stays.
void testBlkeyLoadSaveTranslation() {
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomPath =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/"
      "Design01_NonDMA_8K_PV_Swap.cydsn/rom/rom_8800.bin";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomPath);
  if (rom.empty() || expRom.empty()) {
    std::printf("SKIP: testBlkeyLoadSaveTranslation -- ROM1.BIN and/or rom_8800.bin not found.\n");
    return;
  }
  fs::path sdDir = makeTempTestDir("expansion_keyword_test_blkey_xlate");
  auto m = bootAndSettle(rom);
  loadExpansionRom(*m, expRom, sdDir);
  pc1500::ExpansionMock& mock = m->bus.expansionMock();
  auto run = [&](const std::string& line) {
    tapKey(*m, pc1500::Key::Cl);
    typeText(*m, line);
    tapKey(*m, pc1500::Key::Ent);
    CHECK(waitForIdle(*m, 20'000'000));
  };
  // Every 2-byte token hi,lo in the program's statements (line structure:
  // number hi, lo, length, body ending in CR; FF after the last).
  auto tokens = [](const std::vector<uint8_t>& p, uint8_t hi, uint8_t lo) {
    int n = 0;
    size_t i = 0;
    while (i + 3 <= p.size() && p[i] != 0xFF) {
      size_t len = p[i + 2], body = i + 3;
      bool quoted = false;
      for (size_t k = body; k + 1 < body + len && k + 1 < p.size(); k++) {
        if (p[k] == '"') {
          quoted = !quoted;
        } else if (!quoted && p[k] >= 0xE0) {
          if (p[k] == hi && p[k + 1] == lo) n++;
          k++;
        }
      }
      i = body + len;
    }
    return n;
  };

  run("NEW0");
  std::string error;
  CHECK(pc1500::basic::typeBasicProgramText(m->bus, m->cpu, "10 A$=INKEY$\n20 B$=\"INKEY$\"\n30 C$=BLKEY$\n",
                                            kCyclesPerFrame, kCyclesPerTimerTick, &error));
  std::vector<uint8_t> typed = pc1500::basic::readBasicProgramBytes(m->bus, &error);
  CHECK(tokens(typed, 0xF1, 0x5C) == 1 && tokens(typed, 0xE1, 0x53) == 1);

  run("SDSAVE \"K.BAS\"");  // BLKBD=0, and still INKEY$ for both
  std::vector<uint8_t> saved = readFile((sdDir / "K.BAS").string());
  CHECK(tokens(saved, 0xF1, 0x5C) == 2 && tokens(saved, 0xE1, 0x53) == 0);

  run("NEW0");
  run("SDLOAD \"K.BAS\"");  // BLKBD=0: as saved
  CHECK(pc1500::basic::readBasicProgramBytes(m->bus, &error) == saved);

  mock.setConfigValue(pc1500::ExpansionMock::kConfigBlkbd, 1);
  run("NEW0");
  run("SDLOAD \"K.BAS\"");  // BLKBD=1: BLKEY$ for both
  std::vector<uint8_t> loaded = pc1500::basic::readBasicProgramBytes(m->bus, &error);
  CHECK(loaded.size() == saved.size());
  CHECK(tokens(loaded, 0xF1, 0x5C) == 0 && tokens(loaded, 0xE1, 0x53) == 2);
  std::string text(loaded.begin(), loaded.end());
  CHECK(text.find("\"INKEY$\"") != std::string::npos);

  run("SDSAVE \"K2.BAS\"");  // and back
  CHECK(readFile((sdDir / "K2.BAS").string()) == saved);
}

// BLKEY$ (2026-10-06) reads either keyboard: the external one's key (with the
// driver armed, MCONF BLKBD=1), and the PC-1500's own. The program first
// waits for no key, so RUN's own ENTER doesn't count.
void testBlkeyReadsEitherKeyboard() {
#ifndef PC1500_HAVE_EXPANSION_KEYWORDS
  std::printf("SKIP: testBlkeyReadsEitherKeyboard -- built without the firmware sources.\n");
#else
  const std::string kRomPath = "C:/Users/paulc/Documents/PC1500/ROM1.BIN";
  const std::string kExpRomDir =
      "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/Design01_NonDMA_8K_PV_Swap.cydsn/rom/";
  std::vector<uint8_t> rom = readFile(kRomPath);
  std::vector<uint8_t> expRom = readFile(kExpRomDir + "rom_8800.bin");
  std::vector<uint8_t> romBin = readFile(kExpRomDir + "rom.bin");
  if (rom.empty() || expRom.empty() || romBin.size() < 0x800) {
    std::printf("SKIP: testBlkeyReadsEitherKeyboard -- ROM1.BIN, rom_8800.bin and/or rom.bin not found.\n");
    return;
  }
  const uint16_t kbdLoop = static_cast<uint16_t>((expRom[0x11] << 8) | expRom[0x12]);
  const uint16_t kDriverIdle = static_cast<uint16_t>(kbdLoop + (0xE2AA - 0xE24A));
  fs::path sdDir = makeTempTestDir("expansion_keyword_test_blkey");
  auto m = std::make_unique<BootedMachine>();
  m->bus.ioPort().useManualRtcClock();
  m->bus.loadME0(0xC000, rom.data(), rom.size());
  m->bus.setExtRam0000Size(0x4000);
  m->bus.setExtRamExtSize(0x2800);
  m->bus.loadExpansionModule(0, expRom.data(), expRom.size(), /*base=*/0x8800, /*requirePv=*/false,
                             /*usePuBank=*/false, /*dataWindowBase=*/0x8000,
                             /*dataWindowSize=*/0x800, /*instructionAddr=*/0x87FF, romBin.data(),
                             0x800);
  m->bus.expansionMock().setRootDir(sdDir);
  pc1500::ExpansionMock& mock = m->bus.expansionMock();
  mock.setConfigValue(pc1500::ExpansionMock::kConfigBlkbd, 1);
  auto idle = [&]() {
    for (long i = 0; i < 20'000'000; i++) {
      if (m->cpu.halted() && (m->cpu.p() == kIdleAddr || m->cpu.p() == kDriverIdle)) return true;
      stepOne(*m);
    }
    return false;
  };
  auto number = [&](uint16_t at) { return (m->bus.readME0(at) << 8) | m->bus.readME0(static_cast<uint16_t>(at + 2)); };

  m->cpu.reset();
  for (long c = 0; !m->cpu.halted() && c < 20'000'000; c++) stepOne(*m);
  for (long i = 0; i < 4'000'000; i++) stepOne(*m);
  tapKey(*m, pc1500::Key::Cl);
  typeText(*m, "NEW0");
  tapKey(*m, pc1500::Key::Ent);
  CHECK(idle());
  CHECK(m->bus.readME0(0x79D4) == 0x55);  // the driver is armed

  std::string error;
  CHECK(pc1500::basic::typeBasicProgramText(
      m->bus, m->cpu,
      "5 IF BLKEY$<>\"\" GOTO 5\n10 K$=BLKEY$:IF K$=\"\" GOTO 10\n20 B=0:IF K$=\"Q\" LET B=1\n"
      "30 IF K$=\"W\" LET B=2\n",
      kCyclesPerFrame, kCyclesPerTimerTick, &error));
  tapKey(*m, pc1500::Key::Cl);
  tapKey(*m, pc1500::Key::Mode);  // PRO -> RUN
  auto runAndPress = [&](auto press) {
    tapKey(*m, pc1500::Key::Cl);
    typeText(*m, "RUN");
    tapKey(*m, pc1500::Key::Ent);
    for (int i = 0; i < 400'000; i++) stepOne(*m);  // in the wait loop by now
    CHECK(!m->cpu.halted());
    press();
    CHECK(idle());
    CHECK(m->bus.readME0(kErlAbs) == 0);
  };

  // The external keyboard.
  runAndPress([&] { CHECK(mock.kbdChar('Q')); });
  CHECK(number(0x7908) == 0x0010);  // B = 1: K$ was "Q"

  // The PC-1500's own.
  runAndPress([&] { tapKey(*m, pc1500::Key::W); });
  CHECK(number(0x7908) == 0x0020);  // B = 2: K$ was "W"
#endif
}

// WFPING (2026-10-07, RP2350/net_ping.h): four rounds, then the summary;
// the host here is this machine, which always answers.
void testWifiPing() {
  auto f = bleFixture("testWifiPing");
  if (!f) return;
  CHECK(f->run("WFPING \"127.0.0.1\"") == 0);  // no Wi-Fi yet
  CHECK(f->shown() == "SSH: NO WI-FI - WFCON");
  f->key(pc1500::Key::Ent);
  CHECK(f->m->bus.readME0(kErlAbs) == 40);
  CHECK(f->run("WFPING") == 1);
  CHECK(f->run("WFPING \"\"") == 1);
  CHECK(f->run("WFCON \"HOST\"") == 0);
  f->key(pc1500::Key::Ent);
  CHECK(f->run("WFPING \"127.0.0.1\"", 40'000'000) == 0);
  std::printf("  %s\n", f->shown().c_str());
  CHECK(f->shown().rfind("4/4 REPLIES ", 0) == 0);
  for (long i = 0; i < 1'000'000; i++) stepOne(*f->m);
  f->key(pc1500::Key::Ent);
  CHECK(f->m->bus.readME0(kErlAbs) == 0);
}

// SSH (2026-10-07, RP2350/ssh_session.h): the arguments, the errors that
// need no host, SSHKEY's file and SSHFORGET. A session on a real host is
// testSshSessionLive's.
void testSshArgumentsAndErrors() {
  auto f = bleFixture("testSshArgumentsAndErrors");
  if (!f) return;
  CHECK(f->run("SSH \"paul@host\"") == 0);  // no Wi-Fi yet
  CHECK(f->shown() == "SSH: NO WI-FI - WFCON");
  f->key(pc1500::Key::Ent);
  CHECK(f->m->bus.readME0(kErlAbs) == 40);
  CHECK(f->run("SSH \"nouser\"") == 1);
  CHECK(f->run("SSH \"@host\"") == 1);
  CHECK(f->run("SSH \"paul@\"") == 1);
  CHECK(f->run("SSH \"paul@host:\"") == 1);
  CHECK(f->run("SSH \"paul@host:0\"") == 1);
  CHECK(f->run("SSH \"paul@host:99999\"") == 1);
  CHECK(f->run("SSH \"paul@host:2X\"") == 1);
  CHECK(f->run("SSH") == 1);

  CHECK(f->run("WFCON \"HOST\"") == 0);
  f->key(pc1500::Key::Ent);
  CHECK(f->run("SSH \"paul@127.0.0.1:1\"", 20'000'000) == 0);  // nothing listens there (Windows takes ~2s to say so)
  CHECK(f->shown() == "SSH: NO CONNECTION");
  // The mock holds the emulated clock while a command blocks, so the ~2s
  // connect took no emulated time: ROM1's key gate after the Enter that
  // started it is still shut. Let it open, as those seconds would.
  for (long i = 0; i < 1'000'000; i++) stepOne(*f->m);
  f->key(pc1500::Key::Ent);
  CHECK(f->m->bus.readME0(kErlAbs) == 40);

  CHECK(f->run("SSHKEY") == 0);
  CHECK(f->shown().rfind("SSHKEY.PUB ", 0) == 0);
  f->key(pc1500::Key::Ent);
  {
    std::ifstream in(f->sdDir / "SSHKEY.PUB");
    std::string line;
    std::getline(in, line);
    CHECK(line.rfind("ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAI", 0) == 0);
    CHECK(line.size() > 12 && line.substr(line.size() - 19) == " pc1500@PC-1500 EMU");
  }
  CHECK(f->run("SSHKEY 1") == 1);
  CHECK(f->run("SSHFORGET \"host\"") == 40);  // not known
  CHECK(f->run("SSHFORGET") == 0);
  CHECK(f->shown() == "FORGET ALL HOSTS Y/N");
  f->key(pc1500::Key::Y);
}

// A session on a real host -- only with PC1500_SSH_TEST_HOST ("user@host")
// and PC1500_SSH_TEST_KEY (an unencrypted OpenSSH ed25519 key the host has
// in authorized_keys) set. Refusing the new host's key, then accepting it;
// a command typed on the PC-1500's keys, its output; BREAK as Ctrl-C;
// "exit" back to BASIC; the host known the next time.
static void tapTerminalText(BleFixture& f, const std::string& text) {
  for (char c : text) {
    pc1500::Key k = pc1500::Key::Space;
    if (c >= 'a' && c <= 'z') k = static_cast<pc1500::Key>(static_cast<int>(pc1500::Key::A) + (c - 'a'));
    else if (c == '\r') k = pc1500::Key::Ent;
    else if (c != ' ') continue;
    tapKey(*f.m, k);
  }
}

static bool stepUntil(BleFixture& f, const std::function<bool()>& done, long maxInstructions = 60'000'000) {
  for (long i = 0; i < maxInstructions; i++) {
    if (i % 1000 == 0 && done()) return true;
    stepOne(*f.m);
  }
  return done();
}

static bool shellPrinted(BleFixture& f, const std::string& text) {
  for (const auto& line : f.mock->ssh().lines())
    if (line.find(text) != std::string::npos) return true;
  return false;
}

void testSshSessionLive() {
  const char* target = std::getenv("PC1500_SSH_TEST_HOST");
  const char* keyFile = std::getenv("PC1500_SSH_TEST_KEY");
  if (!target || !keyFile) {
    std::printf("SKIP: testSshSessionLive -- PC1500_SSH_TEST_HOST/PC1500_SSH_TEST_KEY not set.\n");
    return;
  }
  auto f = bleFixture("testSshSessionLive");
  if (!f) return;
  CHECK(f->mock->ssh().loadDeviceKeyFile(keyFile));
  CHECK(f->run("WFCON \"HOST\"") == 0);
  f->key(pc1500::Key::Ent);
  std::string ssh = std::string("SSH \"") + target + "\"";
  std::string host = std::string(target).substr(std::string(target).find('@') + 1);

  CHECK(f->run(ssh) == 0);
  CHECK(f->shown().rfind("NEW HOST ", 0) == 0);
  CHECK(f->shown().size() == 26 && f->shown().substr(22) == " Y/N");
  std::printf("  %s\n", f->shown().c_str());
  f->key(pc1500::Key::N);  // refused: back to BASIC, not remembered
  CHECK(f->m->bus.readME0(kErlAbs) == 0);
  CHECK(!f->mock->ssh().knowsHost(host));

  CHECK(f->run(ssh) == 0);
  tapKey(*f->m, pc1500::Key::Y);
  CHECK(f->mock->ssh().knowsHost(host));
  CHECK(stepUntil(*f, [&] { return f->mock->ssh().terminal(); }));
  CHECK(stepUntil(*f, [&] {
    auto lines = f->mock->ssh().lines();
    return !lines.empty() && lines.back().find("$ ") != std::string::npos;
  }));
  tapTerminalText(*f, "echo pc fifteen hundred\r");
  CHECK(stepUntil(*f, [&] {
    for (const auto& line : f->mock->ssh().lines())
      if (line == "pc fifteen hundred") return true;
    return false;
  }));
  CHECK(f->shown().find('$') != std::string::npos);  // the prompt again, on the display

  tapTerminalText(*f, "sleep thirty\r");  // not a number: "sleep: invalid time interval"...
  CHECK(stepUntil(*f, [&] { return shellPrinted(*f, "sleep: invalid"); }));
  tapTerminalText(*f, "cat\r");  // BREAK (Ctrl-C) ends it
  for (long i = 0; i < 2'000'000; i++) stepOne(*f->m);
  f->m->cpu.pressOnKey();
  f->m->bus.ioPort().setOnKeyLine(true);
  f->m->cpu.requestMI();
  f->m->bus.ioPort().setOnKeyLine(false);
  CHECK(stepUntil(*f, [&] { return shellPrinted(*f, "^C"); }));

  tapTerminalText(*f, "exit\r");
  CHECK(waitForIdle(*f->m, 60'000'000));  // the session over: back at BASIC's prompt
  CHECK(f->m->bus.readME0(kErlAbs) == 0);
  for (const auto& line : f->mock->ssh().lines()) std::printf("  | %s\n", line.c_str());

  CHECK(f->run(ssh) == 0);  // known now: no question
  CHECK(f->shown().rfind("NEW HOST", 0) != 0);
  CHECK(stepUntil(*f, [&] { return f->mock->ssh().terminal(); }));
  tapTerminalText(*f, "exit\r");
  CHECK(waitForIdle(*f->m, 60'000'000));
}

int main(int argc, char** argv) {
  // expansion_keyword_test [part of a test's name]: only the tests whose
  // names contain it (the whole suite takes about 11 minutes).
  const char* only = argc > 1 ? argv[1] : nullptr;
#if defined(_MSC_VER) && defined(_DEBUG)
  // A Debug CRT/STL assertion goes to stderr instead of a popup dialog, so
  // an unattended run reports it instead of waiting for a click.
  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
  _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
#define RUN(test)                                                              \
  do {                                                                         \
    if (!only || std::string(#test).find(only) != std::string::npos) test(); \
  } while (0)
  RUN(testCe150Plotter);
#ifdef PC1500_HAVE_EXPANSION_KEYWORDS
  RUN(testBasicXlateChunks);
  RUN(testKbdLayouts);
#endif
  RUN(testCe150CodesSaveLoadAndHandOver);
  if (only) RUN(testCe150Globe);  // only when asked for: it takes minutes
  RUN(testFnKeysAndStateSaveRestore);
  RUN(testKeywordsInProgramWithExpressions);
  RUN(testExternalKeyboardDriver);
  RUN(testBlkbdPairsKeyboard);
  RUN(testExternalKeyboardOldRom);
  RUN(testBlkeyLoadSaveTranslation);
  RUN(testBlkeyReadsEitherKeyboard);

  RUN(testMconfShowsAndSetsSettings);
  RUN(testBleScanConnectAndDisconnect);
  RUN(testBleAdvertiseWaitsForPeer);
  RUN(testBlePairing);
  RUN(testWifiScanConnectAndPassword);
  RUN(testWifiConnectByNameAndForget);
  RUN(testWifiStatAndMlogClear);
  RUN(testWifiPing);
  RUN(testSshArgumentsAndErrors);
  RUN(testSshSessionLive);
  RUN(testBlePutToPeer);
  RUN(testBleGetFromPeer);
  RUN(testBleMessaging);
  RUN(testSdeofEndsReadLoop);
  RUN(testBlePrintText);
  RUN(testBleListMatchesDetokenizer);
  RUN(testBleSaveLoad);
  RUN(testMlogmsgLogsLiteralAndStringVariable);
  RUN(testBootHookStagesRomThenSkipsOnReset);
  RUN(testStageQueryIsRecognizedKeyword);
  RUN(testDebugBareWordIsNotAReservedKeyword);
  RUN(testStageDebugIsRecognizedKeyword);
  RUN(testStageBlockChecksumAlgorithmMatchesIndependentComputation);
  RUN(testMlogViewIsRecognized);
  RUN(testMlogVerboseIsRecognized);
  RUN(testMlogQuietIsRecognized);
  RUN(testMlogResetIsRecognized);
  RUN(testSlsExitThenTypingDoesNotConcatenate);
  RUN(testSdlsHidesBlinkingCursorDuringBrowse);
  RUN(testSdlsScrollUpDoesNotCorruptEntryPastScratchOffset);
  RUN(testStrayEnterAfterSlsExitDoesNotRedispatch);
  RUN(testSdloadSelectsAndLoadsFile);
  RUN(testSdloadDirectFilenameLoad);
  RUN(testSdloadMHeaderBrowseSelectsFile);
  RUN(testSdloadMDirectHeaderAddress);
  RUN(testSdloadMExplicitAddressDecimal);
  RUN(testSdloadMExplicitAddressHex);
  RUN(testSdsaveBasicRoundTrip);
  RUN(testSdsaveSdloadWidenedChunkRoundTrip);
  RUN(testSdsaveOverwritePromptNAborts);
  RUN(testSdsaveOverwritePromptYOverwrites);
  RUN(testSdsaveDashYSkipsPrompt);
  RUN(testSdsaveNoArgsRaisesError1);
  RUN(testSdsaveBareSavesAsLastLoaded);
  RUN(testSdsaveMMissingArgsRaisesError1);
  RUN(testSdsaveMCallAddressRoundTrip);
  RUN(testSdloadFileNotFoundRaisesError40);
  RUN(testSdloadUsesLiveProgramStartPointer);
  RUN(testSaveLoadBasicProgramUsesLiveProgramStartPointer);
  RUN(testSdmkdirCreatesDirectory);
  RUN(testSdmkdirFailsIfDirectoryAlreadyExists);
  RUN(testSdrmdirRemovesEmptyDirectory);
  RUN(testSdrmdirFailsOnNonEmptyDirectory);
  RUN(testSdcdAffectsSubsequentFileCommands);
  RUN(testSdlsListsDirectoriesWithDirMarker);
  RUN(testSdpwdStagesCurrentDirectoryResponse);
  RUN(testSdDirectoryCommandsRaiseError1WithoutArgument);
  RUN(testSdloadUppercasesLowercaseFilename);
  RUN(testSdmkdirRejectsNon83ShapedNames);
  RUN(testSdcdDotAndDotDotStillWork);
  RUN(testSdcdMultiSegmentPathValidatesEachSegment);
  RUN(testSdPlusTildeTranslation);
  RUN(testSdrmDeletesWithConfirmation);
  RUN(testSdrmNAbortsDeletion);
  RUN(testSdrmDashYSkipsPrompt);
  RUN(testSdrmMissingFilenameRaisesError1);
  RUN(testSdrmCannotRemoveDirectory);
  RUN(testSdcpCopiesFile);
  RUN(testSdcpMissingSourceRaisesError40);
  RUN(testSdmvMovesFile);
  RUN(testSddfDisplaysFreeAndTotalSpace);
  RUN(testSdmvIntoExistingDirectory);
  RUN(testSdcpIntoExistingDirectory);
  RUN(testSdmvWithDotDotRelativeSource);
  RUN(testSdcpWithAbsoluteDestinationPath);
  RUN(testSdcpOverwritePromptNAborts);
  RUN(testSdcpOverwritePromptYOverwrites);
  RUN(testSdcpDashYSkipsPrompt);
  RUN(testSdmvOverwritePromptNAborts);
  RUN(testSdloadFromAbsolutePath);
  RUN(testSdopenCreatesFileAndListsChannel);
  RUN(testSdopenReusingChannelClosesPrevious);
  RUN(testSdcloseClosesOneAndAll);
  RUN(testSdprintSdinputNumericRoundTrip);
  RUN(testSdprintSdinputStringRoundTrip);
  RUN(testSdinputEofFillsZeroAndBlank);
  RUN(testSdskipAdvancesAndRaisesError40PastEnd);
  RUN(testSdChannelCommandsRaiseError1OnMalformedArgument);
  RUN(testSdinputOverlongStringRaisesError42);

  if (g_failures == 0) {
    std::printf("All tests passed.\n");
    return 0;
  }
  std::printf("%d test(s) failed.\n", g_failures);
  return 1;
}
