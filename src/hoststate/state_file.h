// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
//
// File-level framing (magic + format version) around Bus::saveState/
// loadState -- see bus.h's own comment on that pair for exactly what's
// captured (RAM contents + ROM module config/data + extension-RAM sizes;
// deliberately not CPU registers or IoPortController/RTC state).
#pragma once

#include <cstdint>
#include <string>

#include "bus.h"

namespace pc1500host {

inline constexpr char kStateFileMagic[4] = {'P', 'C', '1', 'S'};
inline constexpr uint16_t kStateFileVersion = 1;

// Writes an 8-byte header (4-byte magic, u16 version, 2 reserved bytes)
// followed by bus.saveState(). Returns false with *error set if `path`
// can't be opened for writing.
bool saveStateFile(const pc1500::Bus& bus, const std::string& path, std::string* error);

// Validates the header (magic must match; formatVersion must be
// <= kStateFileVersion, so a state file from a newer build than this one
// understands is rejected rather than partially misread) before calling
// bus.loadState(). Returns false -- with *error set, and `bus` possibly
// partially modified, so callers should treat any false return as fatal
// to this restore attempt -- if the file is missing/unreadable, too
// short, has the wrong magic, or an unsupported version.
bool loadStateFile(pc1500::Bus& bus, const std::string& path, std::string* error);

}  // namespace pc1500host
