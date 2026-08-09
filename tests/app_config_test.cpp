// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "app_config.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      g_failures++;                                                 \
    }                                                                \
  } while (0)

std::string tempPath(const char* name) {
#if defined(_WIN32)
  char dir[MAX_PATH]{};
  DWORD n = GetTempPathA(static_cast<DWORD>(sizeof(dir)), dir);
  std::string path = (n > 0 && n < sizeof(dir)) ? std::string(dir, n) : std::string("C:\\Windows\\Temp\\");
  if (!path.empty() && path.back() != '\\') path += '\\';
  return path + name;
#else
  return std::string("/tmp/") + name;
#endif
}

void testRoundTrip() {
  pc1500host::AppConfig config;
  config.romPath = "C:/roms/rom1.bin";
  config.stateFilePath = "C:/saves/session1.state";
  config.autoLoadOnStart = false;
  config.autoSaveOnExit = true;
  config.showStatusPanel = true;
  config.extRam4800Bytes = 0x2000;
  config.extRam0000Bytes = 0x4000;

  std::string path = tempPath("pc1500emu_app_config_test.json");
  std::string err;
  CHECK(pc1500host::saveAppConfig(config, path, &err));
  CHECK(err.empty());

  pc1500host::AppConfig loaded;
  err.clear();
  CHECK(pc1500host::loadAppConfig(path, &loaded, &err));
  CHECK(err.empty());

  CHECK(loaded.romPath.has_value());
  if (loaded.romPath) CHECK(*loaded.romPath == "C:/roms/rom1.bin");
  CHECK(loaded.stateFilePath.has_value());
  if (loaded.stateFilePath) CHECK(*loaded.stateFilePath == "C:/saves/session1.state");
  CHECK(loaded.autoLoadOnStart == false);
  CHECK(loaded.autoSaveOnExit == true);
  CHECK(loaded.showStatusPanel == true);
  CHECK(loaded.extRam4800Bytes == 0x2000);
  CHECK(loaded.extRam0000Bytes == 0x4000);

  std::remove(path.c_str());
}

void testEmptyConfigRoundTrip() {
  pc1500host::AppConfig config;  // all defaults, both optionals unset

  std::string path = tempPath("pc1500emu_app_config_test_empty.json");
  std::string err;
  CHECK(pc1500host::saveAppConfig(config, path, &err));

  pc1500host::AppConfig loaded;
  err.clear();
  CHECK(pc1500host::loadAppConfig(path, &loaded, &err));
  CHECK(!loaded.romPath.has_value());
  CHECK(!loaded.stateFilePath.has_value());
  CHECK(loaded.autoLoadOnStart == true);   // AppConfig{}'s own default
  CHECK(loaded.autoSaveOnExit == false);
  CHECK(loaded.showStatusPanel == false);
  CHECK(loaded.extRam4800Bytes == 0);
  CHECK(loaded.extRam0000Bytes == 0);

  std::remove(path.c_str());
}

void testMissingFileReturnsDefaults() {
  pc1500host::AppConfig loaded;
  loaded.autoLoadOnStart = false;  // deliberately non-default, to confirm it gets reset
  std::string err = "unset";
  CHECK(pc1500host::loadAppConfig(tempPath("pc1500emu_app_config_test_nonexistent.json"), &loaded,
                                   &err));
  CHECK(err == "unset");  // loadAppConfig shouldn't touch *error on the not-an-error path
  CHECK(!loaded.romPath.has_value());
  CHECK(loaded.autoLoadOnStart == true);  // back to AppConfig{}'s default
}

// Simulates a conf file written by an older build, before showStatusPanel
// (or any future setting) existed: loading it must not fail, and the
// missing key must fall back to AppConfig{}'s own default rather than
// some arbitrary/zeroed value -- the whole point of using nlohmann::json's
// j.value(key, default) instead of j[key] in loadAppConfig.
void testOldConfigMissingNewFieldUsesDefault() {
  std::string path = tempPath("pc1500emu_app_config_test_old.json");
  {
    std::ofstream f(path, std::ios::binary);
    f << R"({"romPath": "C:/roms/rom1.bin", "autoLoadOnStart": false})";
  }
  pc1500host::AppConfig loaded;
  std::string err;
  CHECK(pc1500host::loadAppConfig(path, &loaded, &err));
  CHECK(err.empty());
  CHECK(loaded.romPath.has_value());
  CHECK(loaded.autoLoadOnStart == false);        // present in the old file
  CHECK(loaded.autoSaveOnExit == false);         // absent -- AppConfig{}'s default
  CHECK(loaded.showStatusPanel == false);        // absent -- AppConfig{}'s default
  CHECK(loaded.extRam4800Bytes == 0);            // absent -- AppConfig{}'s default
  CHECK(loaded.extRam0000Bytes == 0);            // absent -- AppConfig{}'s default
  std::remove(path.c_str());
}

void testInvalidJsonRejected() {
  std::string path = tempPath("pc1500emu_app_config_test_invalid.json");
  {
    std::ofstream f(path, std::ios::binary);
    f << "{ this is not valid json";
  }
  pc1500host::AppConfig loaded;
  std::string err;
  CHECK(!pc1500host::loadAppConfig(path, &loaded, &err));
  CHECK(!err.empty());
  std::remove(path.c_str());
}

// --- findDefaultConfFile: CWD -> exe-dir -> home-dir search order ---

namespace fs = std::filesystem;

fs::path makeEmptyTempDir(const char* name) {
  fs::path p(tempPath(name));
  fs::remove_all(p);
  fs::create_directories(p);
  return p;
}

void writeMarkerConfFile(const fs::path& dir) {
  std::ofstream f(dir / pc1500host::kDefaultConfFileName, std::ios::binary);
  f << "{}";
}

void setHomeEnv(const fs::path& dir) {
#if defined(_WIN32)
  _putenv_s("USERPROFILE", dir.string().c_str());
#else
  setenv("HOME", dir.string().c_str(), 1);
#endif
}

// Every test below sets up all three search locations as distinct empty
// temp dirs, populates only the one(s) relevant to that test, points HOME
// at the home-dir candidate, and always restores CWD before returning
// (even though nothing here should throw) so later tests -- and whatever
// ctest runs next -- aren't left in a temp directory.
struct SearchLocations {
  fs::path cwdDir, exeDir, homeDir;
  fs::path originalCwd;

  SearchLocations()
      : cwdDir(makeEmptyTempDir("pc1500emu_conf_search_cwd")),
        exeDir(makeEmptyTempDir("pc1500emu_conf_search_exe")),
        homeDir(makeEmptyTempDir("pc1500emu_conf_search_home")),
        originalCwd(fs::current_path()) {
    fs::current_path(cwdDir);
    setHomeEnv(homeDir);
  }
  ~SearchLocations() { fs::current_path(originalCwd); }

  std::string exePath() const { return (exeDir / "pc1500emu.exe").string(); }
};

void testFindsInCwdFirst() {
  SearchLocations loc;
  writeMarkerConfFile(loc.cwdDir);
  writeMarkerConfFile(loc.exeDir);  // present too -- CWD must still win
  writeMarkerConfFile(loc.homeDir);

  auto found = pc1500host::findDefaultConfFile(loc.exePath());
  CHECK(found.has_value());
  if (found) CHECK(fs::path(*found) == loc.cwdDir / pc1500host::kDefaultConfFileName);
}

void testFallsBackToExeDir() {
  SearchLocations loc;
  writeMarkerConfFile(loc.exeDir);
  writeMarkerConfFile(loc.homeDir);  // present too -- exe-dir must still win over home

  auto found = pc1500host::findDefaultConfFile(loc.exePath());
  CHECK(found.has_value());
  if (found) CHECK(fs::path(*found) == loc.exeDir / pc1500host::kDefaultConfFileName);
}

void testFallsBackToHomeDir() {
  SearchLocations loc;
  writeMarkerConfFile(loc.homeDir);

  auto found = pc1500host::findDefaultConfFile(loc.exePath());
  CHECK(found.has_value());
  if (found) CHECK(fs::path(*found) == loc.homeDir / pc1500host::kDefaultConfFileName);
}

void testFindsNothing() {
  SearchLocations loc;
  auto found = pc1500host::findDefaultConfFile(loc.exePath());
  CHECK(!found.has_value());
}

}  // namespace

int main() {
  testRoundTrip();
  testEmptyConfigRoundTrip();
  testMissingFileReturnsDefaults();
  testOldConfigMissingNewFieldUsesDefault();
  testInvalidJsonRejected();
  testFindsInCwdFirst();
  testFallsBackToExeDir();
  testFallsBackToHomeDir();
  testFindsNothing();

  if (g_failures == 0) {
    std::printf("All tests passed.\n");
    return 0;
  }
  std::printf("%d check(s) failed.\n", g_failures);
  return 1;
}
