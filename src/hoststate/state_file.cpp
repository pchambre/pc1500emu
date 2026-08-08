// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#include "state_file.h"

#include <cstring>
#include <fstream>

#include "serialize_io.h"

namespace pc1500host {

bool saveStateFile(const pc1500::Bus& bus, const std::string& path, std::string* error) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) {
    if (error) *error = "could not open '" + path + "' for writing";
    return false;
  }
  f.write(kStateFileMagic, sizeof(kStateFileMagic));
  pc1500state::writeU16(f, kStateFileVersion);
  pc1500state::writeU16(f, 0);  // reserved
  bus.saveState(f);
  if (!f) {
    if (error) *error = "write failed for '" + path + "'";
    return false;
  }
  return true;
}

bool loadStateFile(pc1500::Bus& bus, const std::string& path, std::string* error) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    if (error) *error = "could not open '" + path + "' for reading";
    return false;
  }
  char magic[4];
  f.read(magic, sizeof(magic));
  if (!f || std::memcmp(magic, kStateFileMagic, sizeof(magic)) != 0) {
    if (error) *error = "'" + path + "' is not a pc1500emu state file (bad magic)";
    return false;
  }
  uint16_t version = pc1500state::readU16(f);
  pc1500state::readU16(f);  // reserved
  if (!f || version > kStateFileVersion) {
    if (error) {
      *error = "'" + path + "' has state-file format version " + std::to_string(version) +
                ", newer than this build understands (" + std::to_string(kStateFileVersion) + ")";
    }
    return false;
  }
  if (!bus.loadState(f)) {
    if (error) *error = "'" + path + "' is truncated or corrupt";
    return false;
  }
  return true;
}

}  // namespace pc1500host
