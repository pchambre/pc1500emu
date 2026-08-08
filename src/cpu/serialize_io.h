// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
//
// Small little-endian read/write primitives shared by anything that
// serializes emulator state to a file (currently just Bus::saveState/
// loadState). Deliberately not a raw struct memcpy/fwrite -- explicit
// field-by-field writes with fixed-width types keep the on-disk format
// well-defined regardless of compiler padding/ABI choices. Lives under
// src/cpu/ only because that's an existing public include directory both
// lh5801cpu and pc1500bus consumers already see -- there's nothing
// CPU-specific in here.
#pragma once

#include <cstdint>
#include <cstddef>
#include <istream>
#include <ostream>

namespace pc1500state {

inline void writeU8(std::ostream& os, uint8_t v) { os.put(static_cast<char>(v)); }

inline void writeU16(std::ostream& os, uint16_t v) {
  writeU8(os, static_cast<uint8_t>(v & 0xFF));
  writeU8(os, static_cast<uint8_t>((v >> 8) & 0xFF));
}

inline void writeU32(std::ostream& os, uint32_t v) {
  writeU16(os, static_cast<uint16_t>(v & 0xFFFF));
  writeU16(os, static_cast<uint16_t>((v >> 16) & 0xFFFF));
}

inline void writeBool(std::ostream& os, bool v) { writeU8(os, v ? 1 : 0); }

inline void writeBytes(std::ostream& os, const uint8_t* data, size_t n) {
  os.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(n));
}

inline uint8_t readU8(std::istream& is) {
  char c = 0;
  is.get(c);
  return static_cast<uint8_t>(c);
}

inline uint16_t readU16(std::istream& is) {
  uint16_t lo = readU8(is);
  uint16_t hi = readU8(is);
  return static_cast<uint16_t>(lo | (hi << 8));
}

inline uint32_t readU32(std::istream& is) {
  uint32_t lo = readU16(is);
  uint32_t hi = readU16(is);
  return lo | (hi << 16);
}

inline bool readBool(std::istream& is) { return readU8(is) != 0; }

inline void readBytes(std::istream& is, uint8_t* data, size_t n) {
  is.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(n));
}

}  // namespace pc1500state
