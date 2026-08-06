// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#pragma once

#include <cstdint>
#include <deque>
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

// kBasicProgramStart is BASIC's fixed program-storage origin (PC-1500
// Technical Reference Manual section 5-3-5's own worked example stores "10
// PRINT A" / "20 END" starting here) -- not configurable, not detected at
// runtime, just where the ROM always looks. kProgramEndPointerAddr is the
// ROM's own cached copy of the program's end address (big-endian, value =
// the address of the terminating 0xFFH byte); writing new program bytes at
// kBasicProgramStart isn't enough on its own, this pointer has to agree or
// LIST/RUN still act on the *old* end address.
constexpr uint16_t kBasicProgramStart = 0x40C5;
constexpr uint16_t kProgramEndPointerAddr = 0x7867;

// Returns the address of the terminating 0xFFH byte (i.e. one past the
// last real program byte), or 0 if the structure runs off the end of the
// addressable range without finding one (a corrupt/never-initialized
// program area).
uint32_t findBasicProgramEnd(pc1500::Bus& bus);

// Reads the current BASIC program's raw tokenized bytes (kBasicProgramStart
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
// advances cpu/bus cycles directly so a whole listing loads in well under a
// second of host time regardless of how many characters it "types".
bool typeBasicProgramText(pc1500::Bus& bus, lh5801::CPU& cpu, const std::string& text,
                           int cyclesPerFrame, int cyclesPerTimerTick, std::string* error);

}  // namespace pc1500::basic
