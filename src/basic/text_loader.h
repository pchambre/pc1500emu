// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "bus.h"
#include "keyboard.h"
#include "lh5801.h"

// Keystroke-driven BASIC program loading/saving, shared between the live
// host app (src/host/main.cpp, which also uses charToTapActions/
// QueuedKeyAction/kTapFrames/kIdleFrames for its own interactive typing
// paths -- see `using` re-exports there) and tests that need to drive the
// ROM's own line editor without a GUI (e.g. basic_load_roundtrip_test.cpp).
namespace pc1500::basic {

// A tapped keypress's timing, expressed in emulator *frames* (not cycles)
// since that's the unit the ROM's own scan cadence is naturally measured
// in -- see typeBasicProgramText's cyclesPerFrame parameter for how a
// caller converts this to actual CPU cycles to step.
constexpr int kTapFrames = 4;   // ~67ms -- spans the ROM's ~25ms scan cadence
constexpr int kIdleFrames = 4;  // ~67ms -- empirically bisected minimum (2026-08-02) that
                                 // still reliably registers BASWORD keywords; 3 fails
                                 // reliably. See [[pc1500_keyword_table_mechanism]] in memory.

// One step of a queued keypress sequence: set a PC-1500 key's state, then
// wait `framesToWait` frames before the next queued action runs.
struct QueuedKeyAction {
  pc1500::Key key;
  bool pressed;
  int framesToWait;
};

// Maps one input character to the PC-1500 keypress sequence that types it
// (a direct tap for keys with a 1:1 physical key, or a Shift-tap sequence
// for shifted symbols). Appends to `out` and returns true on a recognized
// character; returns false (leaving `out` unchanged) for anything with no
// PC-1500 keystroke mapping.
//
// Deliberately case-folds 'a'-'z' to the same physical key as 'A'-'Z' --
// there is only one physical key per letter on the PC-1500, and which case
// it types is a ROM-side keyboard mode (see the SML/Small status bit),
// not a separate keystroke. Callers that need genuine lowercase output
// (e.g. typeBasicProgramText below) drive the SML key themselves around
// calls to this function; charToTapActions has no SML awareness of its
// own.
bool charToTapActions(char c, std::deque<QueuedKeyAction>* out);

// Wraps charToTapActions with the SML (lowercase-input toggle) awareness
// every text-sending path needs -- charToTapActions itself case-folds
// 'a'-'z' to the same physical key as 'A'-'Z' (see its own comment), so
// producing genuine lowercase output means tapping SML before a lowercase
// letter whenever not already in lowercase mode, and again before an
// uppercase letter whenever currently in lowercase mode. This was
// previously duplicated independently in two places (main.cpp's `type`
// FIFO command and typeBasicProgramText below) and missing entirely from
// several others (the typeline* FIFO command family) -- one shared
// implementation now, used everywhere text gets typed.
//
// Only actual letters ('a'-'z'/'A'-'Z') drive the toggle -- confirmed on
// real hardware (and by driving the live emulator's own SDL_TEXTINPUT
// keyboard path, which has no SML logic at all and therefore can't have
// this class of bug) that a space, digit, or punctuation character has no
// case-dependent representation, so a real/live user never touches SML
// for one. The first version of this class toggled for *any* character
// where `(c is a-z) != smlActive_`, which fired on every space/digit/
// punctuation mid-lowercase-run too (toggling off, typing it, then
// toggling back on before the next lowercase letter) -- harmless-looking
// but confirmed to make the ROM drop the very character being typed that
// way (e.g. every space in "rem hello world" typed with SML active).
// Restricting the toggle to letters alone matches how a live user
// actually behaves and fixes this.
//
// Seeds its initial state from the machine's *live* SML status bit
// (764EH bit 3 -- same one `status`'s own small= field reads) rather than
// assuming a fixed starting state, so it self-corrects instead of
// drifting out of sync if SML was toggled by something else (an
// interleaved `key sml` command, a real keypress, or simply not being the
// first text sent in a session) since the caller last checked.
class SmlAwareTyper {
 public:
  explicit SmlAwareTyper(pc1500::Bus& bus) : smlActive_((bus.readME0(0x764E) & 0x08) != 0) {}

  // Appends this character's SML toggle (if it's a letter whose case
  // doesn't match the tracked state) followed by its own tap actions to
  // `out`. Returns false (matching charToTapActions's own convention;
  // `out` is left with only the SML toggle, if any, appended) if `c` has
  // no keystroke mapping.
  bool typeChar(char c, std::deque<QueuedKeyAction>* out) {
    bool isLetter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    if (isLetter) {
      bool wantLower = c >= 'a' && c <= 'z';
      if (wantLower != smlActive_) {
        out->push_back({pc1500::Key::Sml, true, kTapFrames});
        out->push_back({pc1500::Key::Sml, false, kIdleFrames});
        smlActive_ = wantLower;
      }
    }
    return charToTapActions(c, out);
  }

  // Explicitly forces SML to `active`, appending the toggle tap to `out`
  // if a change was actually needed -- for a caller that needs to leave
  // the machine in a known state (e.g. back in uppercase mode) without
  // that being driven by the next character typed.
  void setActive(bool active, std::deque<QueuedKeyAction>* out) {
    if (active == smlActive_) return;
    out->push_back({pc1500::Key::Sml, true, kTapFrames});
    out->push_back({pc1500::Key::Sml, false, kIdleFrames});
    smlActive_ = active;
  }

 private:
  bool smlActive_;
};

// kBasicProgramStart is BASIC's program-storage origin on a bare 2KB-RAM
// machine only (PC-1500 Technical Reference Manual section 5-3-5's own
// worked example stores "10 PRINT A" / "20 END" starting here). It is
// NOT a fixed ROM constant in general -- the ROM's own boot sequence
// computes this origin from whatever RAM is actually installed and
// records it live at BASIC_PROGRAM_START_HI/LO_ABS (0x7865/0x7866,
// 2-byte BE; see basicProgramStart() below), and that value shifts
// whenever the installed RAM shape differs from bare-machine default --
// e.g. 0x38C5 with an isolated 2K at 3800H (CE-155), or 0x00C5 with 16K
// of extension RAM at 0000H (confirmed live: this emulator's 0000H
// window is left-aligned from address 0, so contiguous free RAM now
// starts there instead of at 4000H, and the reserve area's own 189-byte
// offset shifts down with it). Code that reads/writes the *live* program
// area (loading, saving, appending lines, etc.) must call
// basicProgramStart(bus) instead of using this constant directly, or it
// will silently operate on the wrong addresses whenever RAM differs from
// bare-machine default -- this constant exists only as the bare-machine
// reference value (e.g. for the disassembler's static BASPROG symbol,
// which has no live Bus to query).
constexpr uint16_t kBasicProgramStart = 0x40C5;
constexpr uint16_t kProgramEndPointerAddr = 0x7867;

// Reads BASIC's live program-storage origin from RAM (7865H/7866H,
// 2-byte BE -- the ROM's own BASIC_PROGRAM_START_HI/LO_ABS) as computed
// and recorded by the ROM's own boot sequence for whatever RAM is
// currently installed. See kBasicProgramStart's own comment for why this
// must be used instead of that constant for any live program-area
// read/write.
uint16_t basicProgramStart(pc1500::Bus& bus);

// Returns the address of the terminating 0xFFH byte (i.e. one past the
// last real program byte), or 0 if the structure runs off the end of the
// addressable range without finding one (a corrupt/never-initialized
// program area).
uint32_t findBasicProgramEnd(pc1500::Bus& bus);

// Reads the current BASIC program's raw tokenized bytes (basicProgramStart()
// through the trailing 0xFFH, inclusive). Returns an empty vector and sets
// *error if the program area looks corrupt.
std::vector<uint8_t> readBasicProgramBytes(pc1500::Bus& bus, std::string* error);

bool saveBasicProgram(pc1500::Bus& bus, const char* path, std::string* error);
bool saveBasicTextFile(pc1500::Bus& bus, const char* path, std::string* error);
bool loadBasicProgram(pc1500::Bus& bus, const char* path, std::string* error);

// Loads `text` (one BASIC program line per input line, e.g. a listing
// pasted from a magazine transcription, or a saved .bas file) by driving
// the ROM's own PRO-mode line editor via simulated keystrokes -- this is
// authoritative rather than a reimplementation: the ROM itself tokenizes
// each line exactly as it would for a human typing it in.
//
// Tracks a persistent SML/Small-mode toggle across the *entire* call (not
// per-line or per-character) since SML is a sticky lowercase-input mode on
// real hardware that stays in effect across Enter -- confirmed empirically
// (and see the PB3/F00FH hardware-gating fix in bus.cpp's IoPortController
// ::read, without which the ROM's own SML toggle code never actually ran).
// Presses SML before a lowercase letter whenever not already in lowercase
// mode, and again before anything else whenever currently in lowercase
// mode, leaving the machine back in uppercase mode when the whole load
// finishes.
//
// `cyclesPerFrame`/`cyclesPerTimerTick` let the caller supply its own
// timing constants (main.cpp's kCyclesPerFrame/kCyclesPerTimerTick) without
// this file needing to depend on them -- not tied to real GUI frames,
// advances cpu/bus cycles directly so a whole listing loads well under a
// second of host time for a short program. A real-world listing with many
// long lines needing the multi-pass technique (see typeLongLine's own
// comment in text_loader.cpp) can take several seconds, though, since each
// pass involves its own LIST/navigate/type/Enter sequence -- `onProgress`,
// if given, is called once per source line processed (and once per
// continuation pass within a single long line) purely so a caller driving
// a GUI can repaint/pump its own event loop periodically during a long
// call, without this function needing to know anything about GUIs,
// threading, or how much work is left.
bool typeBasicProgramText(pc1500::Bus& bus, lh5801::CPU& cpu, const std::string& text,
                           int cyclesPerFrame, int cyclesPerTimerTick, std::string* error,
                           std::function<void()> onProgress = nullptr);

}  // namespace pc1500::basic
