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
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "basic_text.h"
#include "bus.h"
#include "keyboard.h"
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

// extRam0000Bytes/extRamExtBytes/variant let a test approximate a
// RAM-expanded machine (e.g. a CE-155-shaped 10K config on a PC-1500) --
// must be set before cpu.reset() so BASIC's own boot-time RAM scan sees
// them, matching AppConfig::isPC1500A/extRamExtBytes's own documented
// ordering requirement (src/hoststate/app_config.h).
std::unique_ptr<BootedMachine> bootAndSettle(
    const std::vector<uint8_t>& rom, size_t extRam0000Bytes = 0, size_t extRamExtBytes = 0,
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
bool waitForIdle(BootedMachine& m, long maxInstructions = 2'000'000) {
  for (long i = 0; i < maxInstructions; i++) {
    if (m.cpu.p() == kIdleAddr && m.cpu.halted()) return true;
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

  uint8_t status = bus.expansionMock().processCommand(pc1500::ExpansionMock::kCommandMakeSdDir, window);
  CHECK(status == pc1500::ExpansionMock::kStatusError);
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

  uint8_t status = bus.expansionMock().processCommand(pc1500::ExpansionMock::kCommandRemoveSdDir, window);
  CHECK(status == pc1500::ExpansionMock::kStatusError);
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

  auto m = bootAndSettle(rom, /*extRam0000Bytes=*/16384, /*extRamExtBytes=*/6144);
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

}  // namespace

int main() {
  testSlsExitThenTypingDoesNotConcatenate();
  testSdlsHidesBlinkingCursorDuringBrowse();
  testStrayEnterAfterSlsExitDoesNotRedispatch();
  testSdloadSelectsAndLoadsFile();
  testSdloadDirectFilenameLoad();
  testSdloadMHeaderBrowseSelectsFile();
  testSdloadMDirectHeaderAddress();
  testSdloadMExplicitAddressDecimal();
  testSdloadMExplicitAddressHex();
  testSdsaveBasicRoundTrip();
  testSdsaveOverwritePromptNAborts();
  testSdsaveOverwritePromptYOverwrites();
  testSdsaveDashYSkipsPrompt();
  testSdsaveNoArgsRaisesError1();
  testSdsaveMMissingArgsRaisesError1();
  testSdsaveMCallAddressRoundTrip();
  testSdloadFileNotFoundRaisesError40();
  testSdloadUsesLiveProgramStartPointer();
  testSaveLoadBasicProgramUsesLiveProgramStartPointer();
  testSdmkdirCreatesDirectory();
  testSdmkdirFailsIfDirectoryAlreadyExists();
  testSdrmdirRemovesEmptyDirectory();
  testSdrmdirFailsOnNonEmptyDirectory();
  testSdcdAffectsSubsequentFileCommands();
  testSdlsListsDirectoriesWithDirMarker();
  testSdpwdStagesCurrentDirectoryResponse();
  testSdDirectoryCommandsRaiseError1WithoutArgument();
  testSdloadUppercasesLowercaseFilename();
  testSdmkdirRejectsNon83ShapedNames();
  testSdcdDotAndDotDotStillWork();
  testSdcdMultiSegmentPathValidatesEachSegment();
  testSdPlusTildeTranslation();
  testSdrmDeletesWithConfirmation();
  testSdrmNAbortsDeletion();
  testSdrmDashYSkipsPrompt();
  testSdrmMissingFilenameRaisesError1();
  testSdrmCannotRemoveDirectory();
  testSdcpCopiesFile();
  testSdcpMissingSourceRaisesError40();
  testSdmvMovesFile();
  testSddfDisplaysFreeAndTotalSpace();
  testSdmvIntoExistingDirectory();
  testSdcpIntoExistingDirectory();
  testSdmvWithDotDotRelativeSource();
  testSdcpWithAbsoluteDestinationPath();
  testSdcpOverwritePromptNAborts();
  testSdcpOverwritePromptYOverwrites();
  testSdcpDashYSkipsPrompt();
  testSdmvOverwritePromptNAborts();
  testSdloadFromAbsolutePath();
  testSdopenCreatesFileAndListsChannel();
  testSdopenReusingChannelClosesPrevious();
  testSdcloseClosesOneAndAll();
  testSdprintSdinputNumericRoundTrip();
  testSdprintSdinputStringRoundTrip();
  testSdinputEofFillsZeroAndBlank();
  testSdskipAdvancesAndRaisesError40PastEnd();
  testSdChannelCommandsRaiseError1OnMalformedArgument();
  testSdinputOverlongStringRaisesError42();

  if (g_failures == 0) {
    std::printf("All tests passed.\n");
    return 0;
  }
  std::printf("%d test(s) failed.\n", g_failures);
  return 1;
}
