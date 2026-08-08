// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#include "app_config.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "json.hpp"

namespace pc1500host {

namespace {

using Json = nlohmann::json;

// Resolves `p` against `confDir` if it's relative; returns `p` unchanged
// if it's already absolute or empty.
std::string resolveRelative(const std::string& p, const std::filesystem::path& confDir) {
  if (p.empty()) return p;
  std::filesystem::path path(p);
  if (path.is_absolute()) return p;
  return (confDir / path).string();
}

}  // namespace

bool loadAppConfig(const std::string& path, AppConfig* out, std::string* error) {
  *out = AppConfig{};
  std::ifstream f(path, std::ios::binary);
  if (!f) return true;  // missing conf file -- not an error, just nothing configured yet

  std::ostringstream buf;
  buf << f.rdbuf();

  Json j;
  try {
    j = Json::parse(buf.str());
  } catch (const Json::parse_error& e) {
    if (error) *error = e.what();
    return false;
  }

  std::filesystem::path confDir = std::filesystem::path(path).parent_path();
  if (j.contains("romPath") && j["romPath"].is_string()) {
    out->romPath = resolveRelative(j["romPath"].get<std::string>(), confDir);
  }
  if (j.contains("stateFilePath") && j["stateFilePath"].is_string()) {
    out->stateFilePath = resolveRelative(j["stateFilePath"].get<std::string>(), confDir);
  }
  out->autoLoadOnStart = j.value("autoLoadOnStart", true);
  out->autoSaveOnExit = j.value("autoSaveOnExit", false);
  return true;
}

bool saveAppConfig(const AppConfig& config, const std::string& path, std::string* error) {
  Json j = Json::object();
  if (config.romPath) j["romPath"] = *config.romPath;
  if (config.stateFilePath) j["stateFilePath"] = *config.stateFilePath;
  j["autoLoadOnStart"] = config.autoLoadOnStart;
  j["autoSaveOnExit"] = config.autoSaveOnExit;

  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) {
    if (error) *error = "could not open '" + path + "' for writing";
    return false;
  }
  f << j.dump(2);
  if (!f) {
    if (error) *error = "write failed for '" + path + "'";
    return false;
  }
  return true;
}

std::optional<std::string> findDefaultConfFile(const std::string& exePath) {
  namespace fs = std::filesystem;

  std::error_code ec;
  fs::path cwdCandidate = fs::current_path(ec) / kDefaultConfFileName;
  if (!ec && fs::exists(cwdCandidate)) return fs::absolute(cwdCandidate).string();

  if (!exePath.empty()) {
    fs::path exeDir = fs::absolute(exePath, ec).parent_path();
    if (!ec) {
      fs::path exeCandidate = exeDir / kDefaultConfFileName;
      if (fs::exists(exeCandidate)) return fs::absolute(exeCandidate).string();
    }
  }

#if defined(_WIN32)
  const char* home = std::getenv("USERPROFILE");
#else
  const char* home = std::getenv("HOME");
#endif
  if (home != nullptr) {
    fs::path homeCandidate = fs::path(home) / kDefaultConfFileName;
    if (fs::exists(homeCandidate)) return fs::absolute(homeCandidate).string();
  }

  return std::nullopt;
}

}  // namespace pc1500host
