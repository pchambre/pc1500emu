#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "bus.h"
#include "keyboard.h"
#include "lcd.h"
#include "lh5801.h"

namespace {

constexpr int kScale = 6;         // pixels per dot
// ImGui's default main menu bar height at default font size.
constexpr int kMenuBarHeight = 19;
constexpr int kMarginTop = 20;
constexpr int kMarginBottom = 20;
constexpr int kMarginLeft = 3 * kScale;
constexpr int kMarginRight = 3 * kScale;
constexpr int kWindowW = pc1500::Lcd::kColumns * kScale + kMarginLeft + kMarginRight;

// Fixed-segment status indicator row, shown above the dot matrix (see
// docs/pc1500_hardware_reference.md -- bytes 764EH/764FH, not part of the
// 156x7 graphic display). Laid out left to right per real-hardware
// confirmation from Paul.
constexpr int kIndicatorBarHeight = 18;
constexpr int kIndicatorFontPtSize = 11;
// A .ttc font collection; index picks a language face, but katakana glyphs
// don't differ meaningfully between the CJK variants for our purposes.
constexpr const char* kIndicatorFontPath = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc";
constexpr int kIndicatorFontFaceIndex = 0;

constexpr int kWindowH =
    pc1500::Lcd::kRows * kScale + kMarginTop + kMarginBottom + kIndicatorBarHeight + kMenuBarHeight;

// The fixed set of strings the indicator row can ever show -- rendered
// once into cached textures at startup rather than every frame.
constexpr const char* kBusy = "BUSY";
constexpr const char* kShift = "SHIFT";
constexpr const char* kKatakana = "\xE3\x82\xAB\xE3\x82\xBF";  // katakana ka-ta
constexpr const char* kSmall = "SMALL";
constexpr const char* kDeg = "DEG";
constexpr const char* kRad = "RAD";
constexpr const char* kGrad = "GRAD";
constexpr const char* kRun = "RUN";
constexpr const char* kPro = "PRO";
constexpr const char* kReserve = "RESERVE";
constexpr const char* kDef = "DEF";
constexpr const char* kOne = "I";
constexpr const char* kTwo = "II";
constexpr const char* kThree = "III";
constexpr const char* kAllIndicatorTexts[] = {
    kBusy, kShift, kKatakana, kSmall, kDeg, kRad, kGrad, kRun, kPro, kReserve, kDef, kOne, kTwo, kThree,
};
// I/II/III are a tight cluster of program-level indicators on real
// hardware (basically one space apart from each other -- confirmed by
// Paul), not spread evenly across the bar like the other indicators, so
// they're laid out separately from kIndicatorSlotCount below.
constexpr int kIndicatorSlotCount = 9;  // left-to-right positions for the other indicators

// Approximate: 2.6MHz crystal / 2 = 1.3MHz internal machine cycle (manual
// section 4-2-1). Not cycle-accurate frame pacing, just a reasonable bring
// -up budget so the emulator runs at roughly the real machine's speed.
constexpr int kCyclesPerSecond = 1300000;
constexpr int kFramesPerSecond = 60;
constexpr int kCyclesPerFrame = kCyclesPerSecond / kFramesPerSecond;

// Timer tick rate: approximated as machine-cycle/64, by analogy with the
// PC-2's documented crystal/128 divider (see lh5801::CPU::tickTimer's
// comment) -- not confirmed for the PC-1500's own divider depth.
constexpr int kCyclesPerTimerTick = 64;

// SDL keycode -> PC-1500 matrix key. Host physical key state maps directly
// to PC-1500 physical key state (ignoring host shift/modifiers) since the
// PC-1500's own Shift/Sml keys are themselves ordinary matrix keys --
// interpreting Shift+X combinations is the ROM's job, not ours. Where
// there's no natural 1:1 host key, a reasonable nearby substitute is
// picked (documented inline).
struct KeyMapping {
  SDL_Keycode keycode;
  pc1500::Key key;
};

// clang-format off
constexpr KeyMapping kKeyMap[] = {
    {SDLK_0, pc1500::Key::Digit0}, {SDLK_1, pc1500::Key::Digit1},
    {SDLK_2, pc1500::Key::Digit2}, {SDLK_3, pc1500::Key::Digit3},
    {SDLK_4, pc1500::Key::Digit4}, {SDLK_5, pc1500::Key::Digit5},
    {SDLK_6, pc1500::Key::Digit6}, {SDLK_7, pc1500::Key::Digit7},
    {SDLK_8, pc1500::Key::Digit8}, {SDLK_9, pc1500::Key::Digit9},
    {SDLK_KP_0, pc1500::Key::Digit0}, {SDLK_KP_1, pc1500::Key::Digit1},
    {SDLK_KP_2, pc1500::Key::Digit2}, {SDLK_KP_3, pc1500::Key::Digit3},
    {SDLK_KP_4, pc1500::Key::Digit4}, {SDLK_KP_5, pc1500::Key::Digit5},
    {SDLK_KP_6, pc1500::Key::Digit6}, {SDLK_KP_7, pc1500::Key::Digit7},
    {SDLK_KP_8, pc1500::Key::Digit8}, {SDLK_KP_9, pc1500::Key::Digit9},
    {SDLK_a, pc1500::Key::A}, {SDLK_b, pc1500::Key::B}, {SDLK_c, pc1500::Key::C},
    {SDLK_d, pc1500::Key::D}, {SDLK_e, pc1500::Key::E}, {SDLK_f, pc1500::Key::F},
    {SDLK_g, pc1500::Key::G}, {SDLK_h, pc1500::Key::H}, {SDLK_i, pc1500::Key::I},
    {SDLK_j, pc1500::Key::J}, {SDLK_k, pc1500::Key::K}, {SDLK_l, pc1500::Key::L},
    {SDLK_m, pc1500::Key::M}, {SDLK_n, pc1500::Key::N}, {SDLK_o, pc1500::Key::O},
    {SDLK_p, pc1500::Key::P}, {SDLK_q, pc1500::Key::Q}, {SDLK_r, pc1500::Key::R},
    {SDLK_s, pc1500::Key::S}, {SDLK_t, pc1500::Key::T}, {SDLK_u, pc1500::Key::U},
    {SDLK_v, pc1500::Key::V}, {SDLK_w, pc1500::Key::W}, {SDLK_x, pc1500::Key::X},
    {SDLK_y, pc1500::Key::Y}, {SDLK_z, pc1500::Key::Z},
    {SDLK_MINUS, pc1500::Key::Minus}, {SDLK_EQUALS, pc1500::Key::Equals},
    {SDLK_SLASH, pc1500::Key::Slash}, {SDLK_PERIOD, pc1500::Key::Period},
    {SDLK_LEFTBRACKET, pc1500::Key::LeftParen},   // nearest substitute for (
    {SDLK_RIGHTBRACKET, pc1500::Key::RightParen}, // nearest substitute for )
    {SDLK_KP_PLUS, pc1500::Key::Plus},
    {SDLK_KP_MULTIPLY, pc1500::Key::Asterisk},
    {SDLK_LEFT, pc1500::Key::Left}, {SDLK_RIGHT, pc1500::Key::Right},
    {SDLK_UP, pc1500::Key::Up}, {SDLK_DOWN, pc1500::Key::Down},
    // Deliberately no {SDLK_LSHIFT, ...} or {SDLK_RSHIFT, ...} entry --
    // both host Shift keys behave identically, as pure modifiers, never
    // mapped directly to any PC-1500 key. Holding PC-1500 Shift
    // continuously for the whole time a host Shift is down means the
    // ROM's key-scan sees "Shift alone" as its own key event *before*
    // whatever other key is being combined with it -- and, just like
    // the MODE/CL debounce we traced earlier, that latches a ~0.2s "a
    // key was just processed" window that swallows the very next key
    // event (the actual target key), which arrives well within normal
    // Shift+key typing speed. So host Shift (either one) is read only
    // as a modifier (event.key.keysym.mod) and only ever engages
    // PC-1500 Shift atomically together with a specific target key, via
    // kSymbolMap below. For cases kSymbolMap doesn't cover, Tab sends a
    // direct, standalone PC-1500 Shift keypress (real hardware's own
    // Shift is a tap-to-toggle key, not a hold, so this matches that
    // directly rather than needing the tap-sequence machinery below).
    {SDLK_TAB, pc1500::Key::Shift},
    {SDLK_BACKSPACE, pc1500::Key::Left},           // duplicates the left arrowhead
    {SDLK_RETURN, pc1500::Key::Ent},               // the *small* Ent key -- the
                                                    // main ENTER key's matrix
                                                    // position was never
                                                    // located (see hardware
                                                    // reference doc)
    {SDLK_SPACE, pc1500::Key::Space},
    {SDLK_F1, pc1500::Key::F1}, {SDLK_F2, pc1500::Key::F2},
    {SDLK_F3, pc1500::Key::F3}, {SDLK_F4, pc1500::Key::F4},
    {SDLK_F5, pc1500::Key::F5}, {SDLK_F6, pc1500::Key::F6},
    {SDLK_F7, pc1500::Key::Cl}, {SDLK_F8, pc1500::Key::Mode},
    {SDLK_F9, pc1500::Key::Def}, {SDLK_F11, pc1500::Key::Rcl},
};
// clang-format on

// F10 and F12 need host-modifier awareness (Shift/Ctrl), so they're
// handled specially rather than through the plain kKeyMap table:
//   F10          -> Sml
//   Shift+F10    -> the up/down rocker key
//   F12          -> On (not part of the 8x8 matrix -- wired straight to
//                  the CPU's BFI pin, so it's handled separately from
//                  every other key, same as on real hardware)
//   Shift+F12    -> Off
//   Ctrl+F12     -> host-only convenience key mimicking the real
//                  machine's recessed ALL RESET pinhole switch (also not
//                  part of the matrix) -- re-runs CPU::reset() without
//                  otherwise touching RAM.

// Punctuation/symbol passthrough: on the real PC-1500, typing these
// requires PC-1500 Shift with a specific matrix key (confirmed by Paul
// on real hardware) -- e.g. Shift+F6 always types '&' regardless of
// whether F6 has been separately programmed as a command shortcut, same
// as Shift+/ always types '?'. Pass Shift+F6 as PC-1500 Shift+F6 and
// plain F6 as plain F6, and let the ROM decide what to do -- don't try
// to be clever about current RUN/PRO mode here.
//
// Crucially, PC-1500 Shift is a *toggle*, not a hold: on real hardware
// you tap Shift (press and release), which lights the Shift indicator
// indefinitely (no timeout) until Shift or any other key is pressed,
// and that next keypress is shifted. Shift is never held down
// simultaneously with another key. Simulating a hold instead (as a
// modern keyboard's Shift works) breaks things: the ROM's key-scan
// would see "Shift alone" as its own key event the instant host Shift
// goes down, *before* the target key arrives, which latches the same
// ~0.2s "a key was just processed" debounce window we traced for
// MODE/CL -- and that window is well within normal Shift+key typing
// speed, so it swallows the target key's press entirely.
//
// So each symbol is fired as a queued, fire-and-forget sequence of
// PC-1500 key actions (see QueuedKeyAction/kSymbolActionQueue below),
// entirely decoupled from how long the *host* key is actually held:
//   1. Press PC-1500 Shift, hold for kTapFrames (long enough for one
//      ROM scan cycle to see it as its own event).
//   2. Release Shift, then wait *idle* (nothing pressed) for
//      kIdleFrames -- confirmed empirically that this is what actually
//      matters: the debounce counter only advances while nothing is
//      pressed, so it's the gap *after* release that has to clear the
//      ~0.2s (~260,000-cycle) window, not the hold duration. (Real
//      hardware needs no such delay at all -- Shift stays lit
//      indefinitely -- so this points at a real, still-unresolved bug
//      in our own debounce logic; this queue is a working workaround,
//      not a fix for that deeper issue.)
//   3. Press the target key, hold for kTapFrames, then release.
// Firing this as a queue (rather than tracking per-key held/released
// state) also fixes an earlier, more serious bug: since a normal human
// keypress is much shorter than the ~0.3s this sequence takes, tying it
// to "is the host key still held" meant the sequence was almost always
// cancelled partway through, before the target key was ever pressed --
// which is why symbols "mostly didn't work" before. Once queued, a
// press is committed and runs to completion regardless of when the
// host key is released; OS key-repeat while a host key is held is
// filtered out (event.key.repeat) so a long hold doesn't flood the
// queue with duplicate sequences.
//
// Each entry fires when the *host* types the target symbol (typically
// host Shift+key on a US layout). Most keys fall through to kKeyMap
// when host Shift isn't held (e.g. plain "1" -> Digit1); SDLK_COMMA and
// SDLK_SEMICOLON are the exception -- they type a *different* symbol
// unshifted (also via a PC-1500 Shift tap, just a different base key)
// rather than falling through, since the PC-1500 has no plain "," or
// ";" key of its own.
constexpr int kTapFrames = 4;    // ~67ms -- spans the ROM's ~25ms scan cadence
constexpr int kIdleFrames = 15;  // ~250ms -- clears the ~0.2s debounce window
struct SymbolMapping {
  SDL_Keycode keycode;
  pc1500::Key shiftedTarget;     // host Shift+keycode -> PC-1500 Shift-tap then this
  bool hasUnshiftedTarget;       // if true, plain keycode also -> PC-1500 Shift-tap then unshiftedTarget
  pc1500::Key unshiftedTarget;   // (only meaningful if hasUnshiftedTarget)
};
// clang-format off
constexpr SymbolMapping kSymbolMap[] = {
    {SDLK_1, pc1500::Key::F1, false, {}},              // !
    {SDLK_QUOTE, pc1500::Key::F2, false, {}},           // " (US layout: Shift+')
    {SDLK_QUOTEDBL, pc1500::Key::F2, true, pc1500::Key::F2},  // " (some layouts/setups
                                                               // report this keycode
                                                               // directly instead of
                                                               // SDLK_QUOTE+Shift)
    {SDLK_3, pc1500::Key::F3, false, {}},               // #
    {SDLK_4, pc1500::Key::F4, false, {}},               // $
    {SDLK_5, pc1500::Key::F5, false, {}},               // %
    {SDLK_7, pc1500::Key::F6, false, {}},               // &
    {SDLK_2, pc1500::Key::Equals, false, {}},           // @
    {SDLK_6, pc1500::Key::Space, false, {}},            // ^
    {SDLK_PERIOD, pc1500::Key::RightParen, false, {}},  // >
    {SDLK_SLASH, pc1500::Key::Slash, false, {}},        // ?
    {SDLK_COMMA, pc1500::Key::LeftParen, true, pc1500::Key::Minus},    // < (shifted) / , (plain)
    {SDLK_SEMICOLON, pc1500::Key::Asterisk, true, pc1500::Key::Plus}, // : (shifted) / ; (plain)
    // Insert/Delete have only one meaning regardless of host Shift (no
    // plain-key gesture exists for either), so both targets are the same
    // key -- see the comment where hasTarget is computed for why.
    {SDLK_INSERT, pc1500::Key::Right, true, pc1500::Key::Right},  // Insert -> Shift+Right
    {SDLK_DELETE, pc1500::Key::Left, true, pc1500::Key::Left},    // Delete -> Shift+Left
};
// clang-format on

// "(" and ")" are each their own dedicated, unshifted physical key on real
// PC-1500 hardware (IN6/PA3 and IN0/PA3 -- see docs/pc1500_hardware_reference.md's
// key matrix), same as kKeyMap's other direct mappings -- unlike
// kSymbolMap's entries above, sending them needs no PC-1500 Shift at all.
// But on a QWERTY host they're naturally typed as Shift+9/Shift+0, and
// SDLK_9/SDLK_0 are otherwise plain kKeyMap digit keys (Digit9/Digit0)
// regardless of host Shift -- so without this, Shift+9/Shift+0 just typed
// "9"/"0" instead of "("/")", with no way to reach the parens at all
// (confirmed as a real gap by Paul: the *only* working path was the
// unshifted `[`/`]` keys in kKeyMap below, which isn't how a QWERTY typist
// would ever guess to type them). Handled as its own small dispatch step,
// separate from kSymbolMap's PC-1500-Shift-tap-queue machinery, since
// there's no PC-1500 Shift involved -- just an instant, direct keypress of
// a different target key than kKeyMap's own unshifted entry for the same
// host keycode.
struct ShiftedDirectMapping {
  SDL_Keycode keycode;
  pc1500::Key shiftedTarget;
};
constexpr ShiftedDirectMapping kShiftedDirectMap[] = {
    {SDLK_9, pc1500::Key::LeftParen},   // Shift+9 -> (
    {SDLK_0, pc1500::Key::RightParen},  // Shift+0 -> )
};

// One step of a queued symbol-tap sequence (see kSymbolMap above): set a
// PC-1500 key's state, then wait `framesToWait` real frames before the
// next queued action runs. Processed one action at a time in the main
// loop, independent of host key state -- see the "fire-and-forget"
// rationale in the kSymbolMap comment.
struct QueuedKeyAction {
  pc1500::Key key;
  bool pressed;
  int framesToWait;
};

// ---- Scriptable command FIFO (/tmp/pc1500emu.cmd) ----
//
// Lets an external process (Claude, via Bash) drive a *running* emulator
// session directly -- type text, press named keys, peek/poke memory, dump
// the display -- instead of relaying everything through a human typing on
// the actual keyboard and reporting results back by hand. Query commands
// (peek/dump/status/display) write their result to a response file
// (kResponsePath) for the caller to read back afterward.
//
// Deliberately simple/textual (one command per line) and POSIX-only (this
// is a dev/debug tool, not a portability-sensitive feature).
constexpr const char* kCommandFifoPath = "/tmp/pc1500emu.cmd";
constexpr const char* kResponsePath = "/tmp/pc1500emu.response";

// Maps a plain typed character to a direct PC-1500 keypress (letters,
// digits, space) or a Shift-tap sequence (symbols that need PC-1500 Shift,
// mirroring kSymbolMap's mechanism above) -- returns false if there's no
// mapping at all, in which case the caller should use `key NAME` instead.
bool charToTapActions(char c, std::deque<QueuedKeyAction>* out) {
  pc1500::Key direct{};
  bool hasDirect = true;
  char upper = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
  if (upper >= 'A' && upper <= 'Z') {
    direct = static_cast<pc1500::Key>(static_cast<int>(pc1500::Key::A) + (upper - 'A'));
  } else if (c >= '0' && c <= '9') {
    direct = static_cast<pc1500::Key>(static_cast<int>(pc1500::Key::Digit0) + (c - '0'));
  } else if (c == ' ') {
    direct = pc1500::Key::Space;
  } else if (c == '.') {
    direct = pc1500::Key::Period;
  } else if (c == '/') {
    direct = pc1500::Key::Slash;
  } else if (c == '+') {
    direct = pc1500::Key::Plus;
  } else if (c == '-') {
    direct = pc1500::Key::Minus;
  } else if (c == '=') {
    direct = pc1500::Key::Equals;
  } else {
    hasDirect = false;
  }
  if (hasDirect) {
    out->push_back({direct, true, kTapFrames});
    out->push_back({direct, false, kIdleFrames});
    return true;
  }
  // Symbols needing a PC-1500 Shift-tap sequence (see kSymbolMap above) --
  // only the ones actually needed for BASIC program text so far.
  pc1500::Key shiftedTarget{};
  bool hasShifted = true;
  if (c == '"') {
    shiftedTarget = pc1500::Key::F2;
  } else if (c == ':') {
    shiftedTarget = pc1500::Key::Asterisk;
  } else if (c == '<') {
    shiftedTarget = pc1500::Key::LeftParen;
  } else if (c == '>') {
    shiftedTarget = pc1500::Key::RightParen;
  } else {
    hasShifted = false;
  }
  if (hasShifted) {
    out->push_back({pc1500::Key::Shift, true, kTapFrames});
    out->push_back({pc1500::Key::Shift, false, kIdleFrames});
    out->push_back({shiftedTarget, true, kTapFrames});
    out->push_back({shiftedTarget, false, kIdleFrames});
    return true;
  }
  return false;
}

// Maps a `key NAME` command's name (case-insensitive) to a PC-1500 key for
// a direct tap -- covers keys with no natural printable-character form
// (Enter, Cl, Mode, ...) plus a few redundant-but-harmless aliases.
bool nameToKey(std::string name, pc1500::Key* out) {
  for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  static const std::pair<const char*, pc1500::Key> kNames[] = {
      {"enter", pc1500::Key::Ent},   {"ent", pc1500::Key::Ent},
      {"cl", pc1500::Key::Cl},       {"mode", pc1500::Key::Mode},
      {"def", pc1500::Key::Def},     {"sml", pc1500::Key::Sml},
      {"rcl", pc1500::Key::Rcl},     {"shift", pc1500::Key::Shift},
      {"off", pc1500::Key::Off},     {"up", pc1500::Key::Up},
      {"down", pc1500::Key::Down},   {"left", pc1500::Key::Left},
      {"right", pc1500::Key::Right}, {"space", pc1500::Key::Space},
      {"f1", pc1500::Key::F1},       {"f2", pc1500::Key::F2},
      {"f3", pc1500::Key::F3},       {"f4", pc1500::Key::F4},
      {"f5", pc1500::Key::F5},       {"f6", pc1500::Key::F6},
  };
  for (const auto& [n, k] : kNames) {
    if (name == n) {
      *out = k;
      return true;
    }
  }
  return false;
}

std::vector<uint8_t> readFile(const char* path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// ---- ML/binary load-save: CLOAD M / CSAVE M semantics (PC-1500 Technical
// Reference Manual) -- a plain [addr, addr+len) byte range plus a filename,
// no BASIC-state awareness at all (that's BASIC load/save's job, see
// loadBasicProgram/saveBasicProgram below). The real CLOAD M/CSAVE M take
// an explicit address (and, for CSAVE M, length) argument for exactly this
// reason -- unlike a BASIC program, a hand-assembled ML routine has no
// self-describing bounds the machine could infer on its own.
bool loadBinary(pc1500::Bus& bus, uint16_t addr, const char* path, std::string* error) {
  std::vector<uint8_t> data = readFile(path);
  if (data.empty()) {
    *error = "Could not read file (or file is empty).";
    return false;
  }
  bus.loadME0(addr, data.data(), data.size());
  return true;
}

bool saveBinary(pc1500::Bus& bus, uint16_t addr, uint32_t len, const char* path, std::string* error) {
  if (len == 0) {
    *error = "Length must be greater than 0.";
    return false;
  }
  std::ofstream f(path, std::ios::binary);
  if (!f) {
    *error = "Could not open file for writing.";
    return false;
  }
  for (uint32_t i = 0; i < len; i++) {
    uint8_t b = bus.readME0(static_cast<uint16_t>(addr + i));
    f.put(static_cast<char>(b));
  }
  return true;
}

// ---- BASIC load-save: CLOAD / CSAVE semantics (filename only, no offsets)
// ----
//
// The BASIC program itself always starts at 40C5H on a bare PC-1500 (see
// docs/pc1500_hardware_reference.md's reserve-area note) and is a sequence
// of lines, each [2-byte line number][1-byte line size][line-size bytes of
// tokenized code][0DH], terminated by a single FFH byte after the last
// line (PC-1500 Technical Reference Manual section 5-3-5, "Structure of
// program", confirmed against its own worked example). That FFH byte is
// what CLOAD/CSAVE actually key off, *not* anything in the 4000H-40C4H
// reserve area -- an earlier version of our own hardware-reference doc
// assumed the reserve area held a live "pointer to the BASIC program",
// but on closer reading (PC1500_Technical_reference_manual.pdf section
// 5-3-6) that area is ROM/module bookkeeping and F1-F6 key-reassignment
// shortcuts, unrelated to CLOAD/CSAVE bounds -- see the correction in
// that doc.
//
// We walk the line structure (rather than just scanning for the first FFH
// byte) so an FFH that happens to occur *inside* a line's own tokenized
// content or a string literal can't be mistaken for the terminator -- each
// line's own size field tells us exactly how many content bytes to skip.
constexpr uint16_t kBasicProgramStart = 0x40C5;

// Returns the address of the terminating FFH byte (i.e. one past the last
// real program byte), or 0 if the structure runs off the end of the
// addressable range without finding one (a corrupt/never-initialized
// program area).
uint32_t findBasicProgramEnd(pc1500::Bus& bus) {
  uint32_t addr = kBasicProgramStart;
  while (addr <= 0xFFFF) {
    uint8_t hi = bus.readME0(static_cast<uint16_t>(addr));
    if (hi == 0xFF) return addr;  // end-of-program marker
    if (addr + 2 > 0xFFFF) break;
    uint8_t lineSize = bus.readME0(static_cast<uint16_t>(addr + 2));
    // lineSize already includes the trailing 0DH terminator byte (verified
    // against the manual's own worked example: line "10 PRINT A"'s size
    // byte is 04H, covering exactly F0 97 41 0D -- 3 content bytes plus
    // the CR, not just the content).
    addr += 3 + lineSize;  // line#(2) + size(1) + (content + CR)(lineSize)
  }
  return 0;
}

bool saveBasicProgram(pc1500::Bus& bus, const char* path, std::string* error) {
  uint32_t endAddr = findBasicProgramEnd(bus);
  if (endAddr == 0) {
    *error = "Could not find end of BASIC program (corrupt program area?).";
    return false;
  }
  std::ofstream f(path, std::ios::binary);
  if (!f) {
    *error = "Could not open file for writing.";
    return false;
  }
  for (uint32_t a = kBasicProgramStart; a <= endAddr; a++) {
    f.put(static_cast<char>(bus.readME0(static_cast<uint16_t>(a))));
  }
  return true;
}

// The ROM tracks the program's end address itself (not just the FFH byte
// in-place) at this fixed system-RAM location -- big-endian, value = the
// address of the FFH terminator itself (i.e. exactly what
// findBasicProgramEnd() returns). Found empirically: LIST/RUN both showed
// nothing for a program whose bytes were otherwise byte-for-byte correct
// (verified via direct memory dump) until this pointer was also updated to
// match -- writing raw program bytes into 40C5H+ alone isn't sufficient,
// this cached pointer has to agree or the ROM still thinks the program
// ends wherever it last did (e.g. right at 40C5H, if the program was
// cleared just before loading).
constexpr uint16_t kProgramEndPointerAddr = 0x7867;

bool loadBasicProgram(pc1500::Bus& bus, const char* path, std::string* error) {
  std::vector<uint8_t> data = readFile(path);
  if (data.empty()) {
    *error = "Could not read file (or file is empty).";
    return false;
  }
  if (data.back() != 0xFF) {
    *error = "File doesn't end with the BASIC end-of-program marker (FFH) -- not a saved BASIC program?";
    return false;
  }
  bus.loadME0(kBasicProgramStart, data.data(), data.size());
  uint32_t endAddr = kBasicProgramStart + data.size() - 1;
  bus.writeME0(kProgramEndPointerAddr, static_cast<uint8_t>(endAddr >> 8));
  bus.writeME0(kProgramEndPointerAddr + 1, static_cast<uint8_t>(endAddr & 0xFF));
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  pc1500::Keyboard keyboard;
  pc1500::Bus bus(keyboard);
  lh5801::CPU cpu(bus);
  pc1500::Lcd lcd;

  if (argc > 1) {
    std::vector<uint8_t> rom = readFile(argv[1]);
    if (rom.empty()) {
      std::fprintf(stderr, "pc1500emu: could not read ROM file '%s'\n", argv[1]);
      return 1;
    }
    bus.loadME0(0xC000, rom.data(), rom.size());
    std::printf("pc1500emu: loaded %zu-byte ROM from %s\n", rom.size(), argv[1]);
  } else {
    std::printf(
        "pc1500emu: no ROM given (usage: %s <rom.bin>) -- running with "
        "ROM area 0xFF-filled\n",
        argv[0]);
  }
  cpu.reset();

  // Known limitation: HLT sets a one-way halted flag. Real hardware wakes
  // from HLT via interrupt, but this core doesn't implement interrupt
  // delivery yet (only the SIE/RIE/IE flag bookkeeping), so a HLT'd
  // program will not resume in this emulator.
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    std::fprintf(stderr, "pc1500emu: SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }
  if (TTF_Init() != 0) {
    std::fprintf(stderr, "pc1500emu: TTF_Init failed: %s\n", TTF_GetError());
    return 1;
  }
  TTF_Font* indicatorFont = TTF_OpenFontIndex(kIndicatorFontPath, kIndicatorFontPtSize,
                                               kIndicatorFontFaceIndex);
  if (!indicatorFont) {
    std::fprintf(stderr, "pc1500emu: could not load indicator font '%s': %s\n",
                 kIndicatorFontPath, TTF_GetError());
    return 1;
  }
  SDL_Window* window = SDL_CreateWindow("pc1500emu", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                        kWindowW, kWindowH, SDL_WINDOW_SHOWN);
  SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
  Uint32 mainWindowID = SDL_GetWindowID(window);

  IMGUI_CHECKVERSION();
  ImGuiContext* mainImguiCtx = ImGui::CreateContext();
  ImGuiIO& imguiIo = ImGui::GetIO();
  imguiIo.IniFilename = nullptr;  // no persisted window-layout state -- menu bar only
  ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer2_Init(renderer);

  // Pre-render every indicator label once; each frame just picks which
  // (if any) cached texture to blit per slot, based on the live 764EH/
  // 764FH bits -- matches how the dot matrix itself is drawn (fixed
  // "lit" color, no per-frame text shaping cost).
  SDL_Color indicatorColor{0x20, 0x28, 0x18, 255};  // same "lit" tone as the dot matrix
  std::vector<std::pair<const char*, SDL_Texture*>> indicatorTextures;
  for (const char* text : kAllIndicatorTexts) {
    SDL_Surface* surf = TTF_RenderUTF8_Blended(indicatorFont, text, indicatorColor);
    if (!surf) {
      std::fprintf(stderr, "pc1500emu: TTF_RenderUTF8_Blended('%s') failed: %s\n", text,
                   TTF_GetError());
      return 1;
    }
    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf);
    SDL_FreeSurface(surf);
    indicatorTextures.emplace_back(text, tex);
  }
  auto indicatorTexture = [&](const char* text) -> SDL_Texture* {
    for (auto& [t, tex] : indicatorTextures) {
      if (t == text) return tex;  // pointer equality is fine -- all callers pass the kXxx constants
    }
    return nullptr;
  };

  auto readByte = [&](uint16_t addr) { return bus.readME0(addr); };

  // See kCommandFifoPath's comment (near QueuedKeyAction) for the overall
  // design. mkfifo() is a no-op (EEXIST) on repeated launches -- the FIFO
  // just persists on disk between runs, same file every time.
  mkfifo(kCommandFifoPath, 0666);
  int cmdFifoFd = open(kCommandFifoPath, O_RDONLY | O_NONBLOCK);
  if (cmdFifoFd < 0) {
    std::fprintf(stderr, "pc1500emu: could not open command FIFO '%s': %s\n", kCommandFifoPath,
                 strerror(errno));
  }
  std::string cmdBuf;

  bool running = true;
  int cyclesSinceTimerTick = 0;
  // Tracks which action F10/F12 actually triggered on press, so release
  // matches it even if Shift/Ctrl changed state while the key was held
  // (otherwise a modifier change mid-hold could release the wrong
  // PC-1500 key and leave the other one stuck "pressed" forever).
  bool f10IsRocker = false;
  // Pending symbol-tap sequences (see kSymbolMap/QueuedKeyAction above),
  // processed one action at a time regardless of host key state.
  std::deque<QueuedKeyAction> symbolActionQueue;
  int symbolActionFramesRemaining = 0;
  // Per kSymbolMap entry (by index): whether the *last* press of that
  // host key fell through to kKeyMap (no PC-1500-Shift meaning at the
  // time) rather than being symbol-queued, so release matches whichever
  // actually happened even if Shift's state changed in between.
  std::array<bool, std::size(kSymbolMap)> engagedViaKeyMap{};
  // Per kShiftedDirectMap entry: whether the *last* press of that host key
  // used the shifted-direct target (so release matches even if Shift's
  // state changed mid-hold) -- same pattern as engagedViaKeyMap/f10IsRocker.
  std::array<bool, std::size(kShiftedDirectMap)> shiftedKeyActive{};

  // Command FIFO dispatcher -- see kCommandFifoPath's comment. Parses one
  // line at a time; `type`/`key` append to the same queue kSymbolMap's
  // punctuation handling uses (so scripted and real typing interleave
  // naturally instead of racing); `peek`/`dump`/`status`/`display` write
  // their result to kResponsePath immediately (no queueing needed -- these
  // don't touch the keyboard at all).
  auto writeResponse = [&](const std::string& text) {
    std::ofstream f(kResponsePath, std::ios::trunc);
    f << text;
  };
  auto processCommand = [&](const std::string& line) {
    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;
    for (char& c : cmd) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (cmd == "type") {
      std::string text;
      std::getline(iss, text);
      size_t start = text.find_first_not_of(' ');
      if (start != std::string::npos) text = text.substr(start);
      for (char c : text) {
        if (!charToTapActions(c, &symbolActionQueue)) {
          std::fprintf(stderr, "pc1500emu: 'type' has no mapping for char '%c' -- use 'key NAME'\n", c);
        }
      }
    } else if (cmd == "key") {
      std::string name;
      iss >> name;
      pc1500::Key k;
      if (nameToKey(name, &k)) {
        symbolActionQueue.push_back({k, true, kTapFrames});
        symbolActionQueue.push_back({k, false, kIdleFrames});
      } else {
        std::fprintf(stderr, "pc1500emu: 'key' has no mapping for name '%s'\n", name.c_str());
      }
    } else if (cmd == "peek") {
      long addr = 0;
      iss >> std::hex >> addr;
      writeResponse(std::to_string(bus.readME0(static_cast<uint16_t>(addr))));
    } else if (cmd == "poke") {
      long addr = 0, val = 0;
      iss >> std::hex >> addr >> val;
      bus.writeME0(static_cast<uint16_t>(addr), static_cast<uint8_t>(val));
    } else if (cmd == "dump") {
      long start = 0, end = 0;
      iss >> std::hex >> start >> end;
      std::ostringstream out;
      for (long a = start; a <= end; a += 16) {
        out << std::hex << std::uppercase;
        out.width(4);
        out.fill('0');
        out << a << ": ";
        for (long b = a; b < a + 16 && b <= end; b++) {
          out.width(2);
          out.fill('0');
          out << static_cast<int>(bus.readME0(static_cast<uint16_t>(b))) << " ";
        }
        out << "\n";
      }
      writeResponse(out.str());
    } else if (cmd == "status") {
      std::ostringstream out;
      out << std::hex << std::uppercase;
      out << "P=" << cpu.p() << " A=" << static_cast<int>(cpu.a()) << " X=" << cpu.x()
          << " Y=" << cpu.y() << " U=" << cpu.u() << " S=" << cpu.s() << "\n";
      out << std::dec;
      out << "halted=" << cpu.halted() << " bf=" << cpu.bf() << " disp=" << cpu.disp()
          << " pu=" << cpu.pu() << " pv=" << cpu.pv() << "\n";
      uint8_t ind1 = bus.readME0(0x764E), ind2 = bus.readME0(0x764F);
      out << "ind1(764E)=" << std::hex << static_cast<int>(ind1) << " ind2(764F)="
          << static_cast<int>(ind2) << std::dec
          << " [busy=" << ((ind1 & 0x01) != 0) << " shift=" << ((ind1 & 0x02) != 0)
          << " small=" << ((ind1 & 0x08) != 0) << " def=" << ((ind1 & 0x80) != 0)
          << " run=" << ((ind2 & 0x40) != 0) << " pro=" << ((ind2 & 0x20) != 0)
          << " reserve=" << ((ind2 & 0x10) != 0) << "]\n";
      writeResponse(out.str());
    } else if (cmd == "display") {
      std::ostringstream out;
      for (int row = 0; row < pc1500::Lcd::kRows; row++) {
        for (int col = 0; col < pc1500::Lcd::kColumns; col++) {
          out << (lcd.dot(col, row, cpu.disp(), readByte) ? '#' : '.');
        }
        out << "\n";
      }
      writeResponse(out.str());
    } else if (cmd == "savebasic") {
      std::string path;
      iss >> path;
      std::string error;
      bool ok = saveBasicProgram(bus, path.c_str(), &error);
      writeResponse(ok ? "OK" : ("ERROR: " + error));
    } else if (cmd == "loadbasic") {
      std::string path;
      iss >> path;
      std::string error;
      bool ok = loadBasicProgram(bus, path.c_str(), &error);
      writeResponse(ok ? "OK" : ("ERROR: " + error));
    } else if (cmd == "savebinary") {
      long addr = 0, len = 0;
      std::string path;
      iss >> std::hex >> addr >> len >> path;
      std::string error;
      bool ok = saveBinary(bus, static_cast<uint16_t>(addr), static_cast<uint32_t>(len),
                            path.c_str(), &error);
      writeResponse(ok ? "OK" : ("ERROR: " + error));
    } else if (cmd == "loadbinary") {
      long addr = 0;
      std::string path;
      iss >> std::hex >> addr >> path;
      std::string error;
      bool ok = loadBinary(bus, static_cast<uint16_t>(addr), path.c_str(), &error);
      writeResponse(ok ? "OK" : ("ERROR: " + error));
    } else if (cmd == "call") {
      long addr = 0;
      iss >> std::hex >> addr;
      cpu.setP(static_cast<uint16_t>(addr));
    } else if (cmd == "run") {
      long cycles = 0;
      iss >> std::dec >> cycles;
      long ticks = 0;
      for (long i = 0; i < cycles;) {
        int c = cpu.step();
        int used = (c > 0) ? c : 1;
        i += used;
        cyclesSinceTimerTick += used;
        bus.advanceCycles(used);
        while (cyclesSinceTimerTick >= kCyclesPerTimerTick) {
          cpu.tickTimer();
          cyclesSinceTimerTick -= kCyclesPerTimerTick;
          ticks++;
        }
      }
    } else if (!cmd.empty()) {
      std::fprintf(stderr, "pc1500emu: unknown command '%s'\n", cmd.c_str());
    }
  };

  // File menu dialog state. The dialog form gets its own separate OS
  // window (SDL_Window/SDL_Renderer/ImGuiContext), created on demand --
  // the main emulator window is too small to comfortably fit it (Paul's
  // report). Dear ImGui's SDL2 backend already self-filters events by the
  // window it was bound to at init (see ImGui_ImplSDL2_GetViewportForWindowID
  // in imgui_impl_sdl2.cpp), so routing every SDL event through *both*
  // contexts' ImGui_ImplSDL2_ProcessEvent (switching ImGui::SetCurrentContext
  // first) is safe -- whichever context wasn't bound to that event's window
  // just ignores it.
  enum class ActiveDialog { None, LoadBasic, SaveBasic, LoadBinary, SaveBinary };
  ActiveDialog activeDialog = ActiveDialog::None;
  char filenameBuf[512] = "";
  char addrBuf[8] = "4600";
  char lenBuf[8] = "100";
  char callAddrBuf[8] = "4000";
  bool callAddrTouched = false;  // stops callAddrBuf auto-tracking addrBuf once the user edits it directly
  bool autoCallOnLoad = false;
  std::string dialogError;
  SDL_Window* dialogWindow = nullptr;
  SDL_Renderer* dialogRenderer = nullptr;
  ImGuiContext* dialogImguiCtx = nullptr;

  auto closeDialogWindow = [&]() {
    if (!dialogWindow) return;
    ImGui::SetCurrentContext(dialogImguiCtx);
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext(dialogImguiCtx);
    ImGui::SetCurrentContext(mainImguiCtx);
    SDL_DestroyRenderer(dialogRenderer);
    SDL_DestroyWindow(dialogWindow);
    dialogWindow = nullptr;
    dialogRenderer = nullptr;
    dialogImguiCtx = nullptr;
  };
  auto openDialogWindow = [&](const char* title) {
    closeDialogWindow();  // in case a different dialog was already open
    dialogWindow = SDL_CreateWindow(title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 440,
                                     260, SDL_WINDOW_SHOWN);
    dialogRenderer = SDL_CreateRenderer(dialogWindow, -1, SDL_RENDERER_ACCELERATED);
    dialogImguiCtx = ImGui::CreateContext();
    ImGui::SetCurrentContext(dialogImguiCtx);
    ImGui::GetIO().IniFilename = nullptr;
    ImGui_ImplSDL2_InitForSDLRenderer(dialogWindow, dialogRenderer);
    ImGui_ImplSDLRenderer2_Init(dialogRenderer);
    ImGui::SetCurrentContext(mainImguiCtx);
  };
  // Temporary: logs every real key event with a precise timestamp, so a
  // captured typing sample can be replayed against the C++ core exactly
  // as typed (see /tmp/pc1500emu_keycapture.log).
  std::ofstream keyCaptureLog("/tmp/pc1500emu_keycapture.log", std::ios::app);
  auto captureStart = std::chrono::steady_clock::now();
  while (running) {
    if (cmdFifoFd >= 0) {
      char buf[4096];
      ssize_t n;
      while ((n = read(cmdFifoFd, buf, sizeof(buf))) > 0) {
        cmdBuf.append(buf, static_cast<size_t>(n));
      }
      size_t nl;
      while ((nl = cmdBuf.find('\n')) != std::string::npos) {
        std::string line = cmdBuf.substr(0, nl);
        cmdBuf.erase(0, nl + 1);
        processCommand(line);
      }
    }

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      ImGui::SetCurrentContext(mainImguiCtx);
      ImGui_ImplSDL2_ProcessEvent(&event);
      if (dialogImguiCtx) {
        ImGui::SetCurrentContext(dialogImguiCtx);
        ImGui_ImplSDL2_ProcessEvent(&event);
        ImGui::SetCurrentContext(mainImguiCtx);
      }
      if (event.type == SDL_QUIT) {
        running = false;
      } else if (dialogWindow && event.type == SDL_WINDOWEVENT &&
                 event.window.windowID == SDL_GetWindowID(dialogWindow) &&
                 event.window.event == SDL_WINDOWEVENT_CLOSE) {
        activeDialog = ActiveDialog::None;
        closeDialogWindow();
      } else if ((event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) &&
                 event.key.windowID != mainWindowID) {
        // Belongs to the dialog window (or something else) -- its own
        // ImGui context already processed it above; never feed it to the
        // emulated keyboard regardless of focus/capture state.
      } else if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
        bool pressed = (event.type == SDL_KEYDOWN);
        SDL_Keycode kc = event.key.keysym.sym;
        Uint16 mod = event.key.keysym.mod;
        bool shiftHeld = (mod & KMOD_SHIFT) != 0;
        bool ctrlHeld = (mod & KMOD_CTRL) != 0;
        {
          auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now() - captureStart)
                               .count();
          keyCaptureLog << elapsedUs << ',' << kc << ',' << (pressed ? 1 : 0) << ','
                        << (event.key.repeat ? 1 : 0) << ',' << mod << '\n';
          keyCaptureLog.flush();
        }
        if (kc == SDLK_F12) {
          if (pressed && ctrlHeld) {
            cpu.reset();
          } else if (pressed && shiftHeld) {
            bus.setKeyState(pc1500::Key::Off, true);
          } else if (pressed) {
            cpu.pressOnKey();
            bus.ioPort().setOnKeyLine(true);
          } else {
            // Unconditionally release both -- whichever was actually
            // pressed gets released; releasing the other is a no-op.
            bus.setKeyState(pc1500::Key::Off, false);
            bus.ioPort().setOnKeyLine(false);
          }
        } else if (kc == SDLK_F10) {
          if (pressed) f10IsRocker = shiftHeld;
          bus.setKeyState(f10IsRocker ? pc1500::Key::UpDownRocker : pc1500::Key::Sml, pressed);
        } else {
          bool handledByShiftedDirectMap = false;
          for (size_t i = 0; i < std::size(kShiftedDirectMap); i++) {
            const ShiftedDirectMapping& sdm = kShiftedDirectMap[i];
            if (sdm.keycode != kc) continue;
            if (pressed) {
              if (event.key.repeat) {
                handledByShiftedDirectMap = shiftedKeyActive[i];
                break;
              }
              if (shiftHeld) {
                bus.setKeyState(sdm.shiftedTarget, true);
                shiftedKeyActive[i] = true;
                handledByShiftedDirectMap = true;
              } else {
                shiftedKeyActive[i] = false;  // falls through to kKeyMap below
              }
            } else if (shiftedKeyActive[i]) {
              bus.setKeyState(sdm.shiftedTarget, false);
              shiftedKeyActive[i] = false;
              handledByShiftedDirectMap = true;
            }
            break;
          }
          if (handledByShiftedDirectMap) {
            // Handled above -- skip kSymbolMap/kKeyMap entirely for this
            // event.
          } else {
          bool handledBySymbolMap = false;
          for (size_t i = 0; i < std::size(kSymbolMap); i++) {
            const SymbolMapping& sm = kSymbolMap[i];
            if (sm.keycode != kc) continue;
            if (pressed) {
              if (event.key.repeat) {
                // Don't flood the queue with duplicate sequences while
                // the host key is held past the OS's key-repeat delay.
                handledBySymbolMap = true;
                break;
              }
              // Some of these keycodes are *shared*: on QWERTY, SDLK_1 is
              // plain "1" alone, or "!" when host Shift is also held --
              // shiftHeld must gate the choice, and plain "1" needs to
              // fall through to kKeyMap (there's no PC-1500-Shift meaning
              // for it at all). Others (Insert/Delete, or a layout like
              // AZERTY reporting SDLK_QUOTEDBL directly for ") have only
              // one meaning regardless of host Shift -- those are encoded
              // by giving hasUnshiftedTarget=true with unshiftedTarget set
              // to the *same* key as shiftedTarget, so they always queue
              // the tap sequence without ever depending on shiftHeld.
              bool hasTarget = true;
              pc1500::Key target = pc1500::Key::Space;
              if (shiftHeld) {
                target = sm.shiftedTarget;
              } else if (sm.hasUnshiftedTarget) {
                target = sm.unshiftedTarget;
              } else {
                hasTarget = false;
              }
              if (hasTarget) {
                // Fire-and-forget: queue the whole tap sequence now: it
                // runs to completion regardless of how long the host
                // key ends up being held, or when it's released.
                symbolActionQueue.push_back({pc1500::Key::Shift, true, kTapFrames});
                symbolActionQueue.push_back({pc1500::Key::Shift, false, kIdleFrames});
                symbolActionQueue.push_back({target, true, kTapFrames});
                symbolActionQueue.push_back({target, false, 0});
                engagedViaKeyMap[i] = false;
                handledBySymbolMap = true;
              } else {
                // No PC-1500-Shift meaning right now (e.g. plain "1")
                // -- falls through to kKeyMap, remembered so release
                // matches even if Shift's state changes meanwhile.
                engagedViaKeyMap[i] = true;
              }
            } else if (!engagedViaKeyMap[i]) {
              // This host key's last press was symbol-queued, not
              // passed through to kKeyMap -- the queue above already
              // handles releasing the target key on its own schedule,
              // so there's nothing to do here.
              handledBySymbolMap = true;
            }
            // else: last press fell through to kKeyMap, so let release
            // fall through too (handledBySymbolMap stays false).
            break;
          }
          if (!handledBySymbolMap) {
            for (const KeyMapping& m : kKeyMap) {
              if (m.keycode == kc) {
                bus.setKeyState(m.key, pressed);
                break;
              }
            }
          }
          }
        }
      }
    }

    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    if (ImGui::BeginMainMenuBar()) {
      if (ImGui::BeginMenu("File")) {
        auto startDialog = [&](ActiveDialog which, const char* title) {
          activeDialog = which;
          filenameBuf[0] = '\0';
          dialogError.clear();
          callAddrTouched = false;
          std::strncpy(callAddrBuf, addrBuf, sizeof(callAddrBuf));
          openDialogWindow(title);
        };
        if (ImGui::MenuItem("Load BASIC...")) startDialog(ActiveDialog::LoadBasic, "Load BASIC");
        if (ImGui::MenuItem("Save BASIC...")) startDialog(ActiveDialog::SaveBasic, "Save BASIC");
        ImGui::Separator();
        if (ImGui::MenuItem("Load Binary...")) startDialog(ActiveDialog::LoadBinary, "Load Binary");
        if (ImGui::MenuItem("Save Binary...")) startDialog(ActiveDialog::SaveBinary, "Save Binary");
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu("Settings")) {
        if (ImGui::BeginMenu("Extension RAM (4800H)")) {
          size_t cur = bus.extRam4800Size();
          // Real 1982 hardware options were 4K/8K; 10K (the window's full
          // physical span) wasn't a real period-correct module, but is
          // easy to emulate and physically possible with modern RAM.
          if (ImGui::MenuItem("None", nullptr, cur == 0)) bus.setExtRam4800Size(0);
          if (ImGui::MenuItem("4K", nullptr, cur == 0x1000)) bus.setExtRam4800Size(0x1000);
          if (ImGui::MenuItem("8K", nullptr, cur == 0x2000)) bus.setExtRam4800Size(0x2000);
          if (ImGui::MenuItem("10K (full window)", nullptr,
                               cur == pc1500::Bus::kExtRam4800WindowSize)) {
            bus.setExtRam4800Size(pc1500::Bus::kExtRam4800WindowSize);
          }
          ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Extension RAM (0000H)")) {
          size_t cur = bus.extRam0000Size();
          // Not a real 1982-era option at all (nothing plugged in there
          // back then), but physically possible now.
          if (ImGui::MenuItem("None", nullptr, cur == 0)) bus.setExtRam0000Size(0);
          if (ImGui::MenuItem("16K (full window)", nullptr,
                               cur == pc1500::Bus::kExtRam0000WindowSize)) {
            bus.setExtRam0000Size(pc1500::Bus::kExtRam0000WindowSize);
          }
          ImGui::EndMenu();
        }
        ImGui::EndMenu();
      }
      ImGui::EndMainMenuBar();
    }

    ImGui::Render();

    // The dialog form renders in its own OS window/context (see
    // openDialogWindow's comment) -- a fully separate NewFrame/Render/
    // Present cycle, bound to dialogRenderer instead of the main window's
    // renderer.
    if (dialogWindow) {
      ImGui::SetCurrentContext(dialogImguiCtx);
      ImGui_ImplSDLRenderer2_NewFrame();
      ImGui_ImplSDL2_NewFrame();
      ImGui::NewFrame();

      const char* actionLabel = "";
      bool isBinary = false;
      bool isLoad = false;
      switch (activeDialog) {
        case ActiveDialog::LoadBasic:
          actionLabel = "Load";
          isLoad = true;
          break;
        case ActiveDialog::SaveBasic:
          actionLabel = "Save";
          break;
        case ActiveDialog::LoadBinary:
          actionLabel = "Load";
          isBinary = true;
          isLoad = true;
          break;
        case ActiveDialog::SaveBinary:
          actionLabel = "Save";
          isBinary = true;
          break;
        case ActiveDialog::None:
          break;
      }
      ImGuiIO& dialogIo = ImGui::GetIO();
      ImGui::SetNextWindowPos(ImVec2(0, 0));
      ImGui::SetNextWindowSize(dialogIo.DisplaySize);
      bool closeRequested = false;
      if (ImGui::Begin("##dialog", nullptr,
                        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse)) {
        ImGui::InputText("Filename", filenameBuf, sizeof(filenameBuf));
        if (isBinary) {
          bool addrChanged = ImGui::InputText("Address (hex)", addrBuf, sizeof(addrBuf),
                                               ImGuiInputTextFlags_CharsHexadecimal);
          if (addrChanged && !callAddrTouched) {
            std::strncpy(callAddrBuf, addrBuf, sizeof(callAddrBuf));
          }
          if (!isLoad) {
            ImGui::InputText("Length (hex)", lenBuf, sizeof(lenBuf),
                              ImGuiInputTextFlags_CharsHexadecimal);
          } else {
            ImGui::Checkbox("Call after load", &autoCallOnLoad);
            if (autoCallOnLoad) {
              // Defaults to the load address (kept in sync until the user
              // edits this field directly -- see callAddrTouched), but can
              // be set independently: e.g. load a relocatable blob at one
              // address and jump into an entry point partway through it.
              if (ImGui::InputText("Call address (hex)", callAddrBuf, sizeof(callAddrBuf),
                                    ImGuiInputTextFlags_CharsHexadecimal)) {
                callAddrTouched = true;
              }
            }
          }
        }
        if (!dialogError.empty()) {
          ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", dialogError.c_str());
        }
        bool doAction = ImGui::Button(actionLabel);
        ImGui::SameLine();
        bool doCancel = ImGui::Button("Cancel");
        if (doAction) {
          bool ok = false;
          switch (activeDialog) {
            case ActiveDialog::LoadBinary: {
              uint16_t addr = static_cast<uint16_t>(strtol(addrBuf, nullptr, 16));
              ok = loadBinary(bus, addr, filenameBuf, &dialogError);
              if (ok && autoCallOnLoad) {
                uint16_t callAddr = static_cast<uint16_t>(strtol(callAddrBuf, nullptr, 16));
                cpu.setP(callAddr);
              }
              break;
            }
            case ActiveDialog::SaveBinary: {
              uint16_t addr = static_cast<uint16_t>(strtol(addrBuf, nullptr, 16));
              uint32_t len = static_cast<uint32_t>(strtoul(lenBuf, nullptr, 16));
              ok = saveBinary(bus, addr, len, filenameBuf, &dialogError);
              break;
            }
            case ActiveDialog::LoadBasic:
              ok = loadBasicProgram(bus, filenameBuf, &dialogError);
              break;
            case ActiveDialog::SaveBasic:
              ok = saveBasicProgram(bus, filenameBuf, &dialogError);
              break;
            case ActiveDialog::None:
              break;
          }
          if (ok) closeRequested = true;
        }
        if (doCancel) closeRequested = true;
      }
      ImGui::End();

      ImGui::Render();
      SDL_SetRenderDrawColor(dialogRenderer, 0x30, 0x30, 0x30, 255);
      SDL_RenderClear(dialogRenderer);
      ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), dialogRenderer);
      SDL_RenderPresent(dialogRenderer);
      ImGui::SetCurrentContext(mainImguiCtx);

      if (closeRequested) {
        activeDialog = ActiveDialog::None;
        closeDialogWindow();
      }
    }

    // Process one step of the pending symbol-tap queue per frame,
    // independent of host key state (see kSymbolMap/QueuedKeyAction).
    if (!symbolActionQueue.empty() && --symbolActionFramesRemaining <= 0) {
      QueuedKeyAction action = symbolActionQueue.front();
      symbolActionQueue.pop_front();
      bus.setKeyState(action.key, action.pressed);
      symbolActionFramesRemaining = action.framesToWait;
    }

    // step() checks for pending interrupts even while halted (and clears
    // halted_ if one dispatches), so keep calling it either way rather
    // than skipping entirely -- that's the only way HLT can ever resume.
    // Deliberately no early-exit on cpu.halted(): breaking out here would
    // stop the timer from advancing too, so a halted CPU would only ever
    // see its timer interrupt move forward by one cycle per rendered
    // frame (~9 minutes of real time to accumulate one interrupt period,
    // instead of ~25ms) -- exactly the kind of thing that makes keyboard
    // input look almost entirely unresponsive.
    int cyclesRun = 0;
    while (cyclesRun < kCyclesPerFrame) {
      int c = cpu.step();
      int used = (c > 0) ? c : 1;
      cyclesRun += used;
      cyclesSinceTimerTick += used;
      bus.advanceCycles(used);
      while (cyclesSinceTimerTick >= kCyclesPerTimerTick) {
        cpu.tickTimer();
        cyclesSinceTimerTick -= kCyclesPerTimerTick;
      }
    }

    SDL_SetRenderDrawColor(renderer, 0xC8, 0xD8, 0xC0, 255);  // unlit LCD background
    SDL_RenderClear(renderer);
    SDL_SetRenderDrawColor(renderer, 0x20, 0x28, 0x18, 255);  // lit dot
    for (int col = 0; col < pc1500::Lcd::kColumns; col++) {
      for (int row = 0; row < pc1500::Lcd::kRows; row++) {
        if (lcd.dot(col, row, cpu.disp(), readByte)) {
          SDL_Rect r{kMarginLeft + col * kScale,
                     kMenuBarHeight + kIndicatorBarHeight + kMarginTop + row * kScale, kScale - 1,
                     kScale - 1};
          SDL_RenderFillRect(renderer, &r);
        }
      }
    }

    // Fixed-segment status indicators (764EH/764FH) -- left to right:
    // BUSY, SHIFT, katakana, SMALL, DEG/RAD/GRAD, RUN, PRO, RESERVE, DEF,
    // then I/II/III as a tight cluster (see kIndicatorSlotCount's comment).
    uint8_t ind1 = bus.readME0(0x764E);
    uint8_t ind2 = bus.readME0(0x764F);
    bool de = ind2 & 0x01, g = ind2 & 0x02, rad = ind2 & 0x04;
    const char* angleMode = nullptr;
    if (de && g) angleMode = kDeg;
    else if (g) angleMode = kGrad;
    else if (rad) angleMode = kRad;
    const char* slotText[kIndicatorSlotCount] = {
        (ind1 & 0x01) ? kBusy : nullptr,      // Busy
        (ind1 & 0x02) ? kShift : nullptr,     // Shift
        (ind1 & 0x04) ? kKatakana : nullptr,  // Japanese
        (ind1 & 0x08) ? kSmall : nullptr,     // Small
        angleMode,                            // Deg/Rad/Grad
        (ind2 & 0x40) ? kRun : nullptr,        // Run
        (ind2 & 0x20) ? kPro : nullptr,        // Pro
        (ind2 & 0x10) ? kReserve : nullptr,    // Reserve
        (ind1 & 0x80) ? kDef : nullptr,        // Def
    };
    int slotWidth = kWindowW / (kIndicatorSlotCount + 1);  // +1 reserves the last slot for I/II/III
    for (int slot = 0; slot < kIndicatorSlotCount; slot++) {
      if (!slotText[slot]) continue;
      SDL_Texture* tex = indicatorTexture(slotText[slot]);
      if (!tex) continue;
      int texW, texH;
      SDL_QueryTexture(tex, nullptr, nullptr, &texW, &texH);
      SDL_Rect dst{slot * slotWidth + (slotWidth - texW) / 2,
                   kMenuBarHeight + (kIndicatorBarHeight - texH) / 2, texW, texH};
      SDL_RenderCopy(renderer, tex, nullptr, &dst);
    }

    // I/II/III: tight cluster, one space apart, centered in the last slot.
    const char* romanTexts[3] = {(ind1 & 0x40) ? kOne : nullptr, (ind1 & 0x20) ? kTwo : nullptr,
                                  (ind1 & 0x10) ? kThree : nullptr};
    int spaceW = 0, spaceH = 0;
    TTF_SizeUTF8(indicatorFont, " ", &spaceW, &spaceH);
    int romanTotalW = 0;
    for (const char* t : romanTexts) {
      if (!t) continue;
      int w, h;
      SDL_QueryTexture(indicatorTexture(t), nullptr, nullptr, &w, &h);
      romanTotalW += (romanTotalW > 0 ? spaceW : 0) + w;
    }
    int romanX = kIndicatorSlotCount * slotWidth + (slotWidth - romanTotalW) / 2;
    for (const char* t : romanTexts) {
      if (!t) continue;
      SDL_Texture* tex = indicatorTexture(t);
      int texW, texH;
      SDL_QueryTexture(tex, nullptr, nullptr, &texW, &texH);
      SDL_Rect dst{romanX, kMenuBarHeight + (kIndicatorBarHeight - texH) / 2, texW, texH};
      SDL_RenderCopy(renderer, tex, nullptr, &dst);
      romanX += texW + spaceW;
    }

    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);

    SDL_RenderPresent(renderer);

    SDL_Delay(1000 / kFramesPerSecond);
  }

  closeDialogWindow();
  if (cmdFifoFd >= 0) close(cmdFifoFd);

  for (auto& [text, tex] : indicatorTextures) {
    (void)text;
    SDL_DestroyTexture(tex);
  }
  ImGui_ImplSDLRenderer2_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();
  TTF_CloseFont(indicatorFont);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  TTF_Quit();
  SDL_Quit();
  return 0;
}
