// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
//
// File-level framing (magic + format version) around CPU::saveState/
// loadState and Bus::saveState/loadState -- see each's own comment for
// exactly what's captured. Deliberately includes full CPU register/flag/
// interrupt-latch state (format version 2; version 1 didn't; version 3
// widened Bus::saveState/loadState from two ROM module slots to four;
// version 4 added the CE-163 module's enabled flag, active bank, and 32K
// backing store) -- restoring a session is meant to resume exactly where
// OFF left the machine, the same way real hardware's OFF/ON cycle just
// halts and wakes the CPU in place rather than resetting it, so the
// caller must NOT call cpu.reset() after a successful loadStateFile.
#pragma once

#include <cstdint>
#include <string>

#include "bus.h"
#include "lh5801.h"

namespace pc1500host {

inline constexpr char kStateFileMagic[4] = {'P', 'C', '1', 'S'};
inline constexpr uint16_t kStateFileVersion = 4;

// Writes an 8-byte header (4-byte magic, u16 version, 2 reserved bytes),
// then cpu.saveState(), then bus.saveState(). Returns false with *error
// set if `path` can't be opened for writing.
bool saveStateFile(const lh5801::CPU& cpu, const pc1500::Bus& bus, const std::string& path,
                    std::string* error);

// Validates the header (magic must match; formatVersion must equal
// kStateFileVersion exactly -- this is early-stage software with no
// installed base to keep old state files compatible with, so a version
// mismatch in either direction is simply rejected rather than partially
// misread) before calling cpu.loadState()/bus.loadState(). Returns false
// -- with *error set, and cpu/bus possibly partially modified, so callers
// should treat any false return as fatal to this restore attempt -- if
// the file is missing/unreadable, too short, has the wrong magic, or a
// different version. On success, the caller must NOT follow up with
// cpu.reset() -- see the file header comment.
bool loadStateFile(lh5801::CPU& cpu, pc1500::Bus& bus, const std::string& path,
                    std::string* error);

}  // namespace pc1500host
