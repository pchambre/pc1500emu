// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
//
// The small JSON "conf file" passed via `--conf <path>` -- distinct from
// the binary state file (state_file.h): this just remembers which ROM/
// state-file paths and auto-load/auto-save preferences to use, so a
// shortcut like `pc1500emu --conf myconf.json` doesn't need those spelled
// out on the command line every time.
#pragma once

#include <optional>
#include <string>

namespace pc1500host {

struct AppConfig {
  // Resolved relative to the conf file's own directory (not the process's
  // current working directory) by loadAppConfig, so a conf file stays
  // portable if its containing directory moves.
  std::optional<std::string> romPath;
  std::optional<std::string> stateFilePath;
  bool autoLoadOnStart = true;
  bool autoSaveOnExit = false;
};

// A missing file at `path` is not an error -- returns true with *out left
// at its all-defaults construction. Returns false only if the file exists
// but isn't valid JSON, with *error set.
bool loadAppConfig(const std::string& path, AppConfig* out, std::string* error);

// Pretty-printed (2-space indent) for human readability/diffability.
// Returns false with *error set if `path` can't be opened for writing.
bool saveAppConfig(const AppConfig& config, const std::string& path, std::string* error);

// The filename findDefaultConfFile looks for, and the one any code that
// auto-creates a conf file (see main.cpp's persistActiveConf) should use.
inline constexpr const char* kDefaultConfFileName = "pc1500emu.json";

// Searches, in order, the current working directory, the directory
// containing `exePath` (typically main()'s argv[0] -- resolved to an
// absolute path first, since argv[0] may be relative depending on how the
// process was launched), and the user's home directory, for a file named
// kDefaultConfFileName. Returns the first match found (as an absolute
// path), or std::nullopt if none of the three locations has one -- not an
// error, just "nothing to auto-load".
std::optional<std::string> findDefaultConfFile(const std::string& exePath);

}  // namespace pc1500host
