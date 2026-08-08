// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <cstdio>
#include <cstdint>
#include <fstream>
#include <string>

#include "bus.h"
#include "keyboard.h"
#include "state_file.h"

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
  pc1500::Keyboard kb;
  pc1500::Bus bus(kb);

  // Ext-RAM windows must be sized *before* writing into them --
  // writeME0 drops writes to an unmapped (size-0) window (see
  // Bus::isUnmapped), so this order matters.
  bus.setExtRam4800Size(0x2000);
  bus.setExtRam0000Size(0x4000);

  // RAM contents -- a few distinguishing bytes spread across the saved
  // range, including right at its very last byte (0x7FFF).
  bus.writeME0(0x0010, 0xAB);  // 0000H window (now sized)
  bus.writeME0(0x4010, 0xCD);  // built-in 2K RAM, never gated
  bus.writeME0(0x6000, 0xEF);  // 4800H window (now sized), offset 0x1800 < 0x2000
  bus.writeME0(0x7FFF, 0x99);  // system RAM, mirrors to 0x7BFF, never gated

  uint8_t module1Data[4] = {0x11, 0x22, 0x33, 0x44};
  bus.loadRomModule(module1Data, sizeof(module1Data), 0xA000, /*requirePv=*/false,
                     /*usePuBank=*/false);
  uint8_t module2Data[4] = {0x55, 0x66, 0x77, 0x88};
  bus.loadRomModule2(module2Data, sizeof(module2Data), 0x8000, /*requirePv=*/true,
                      /*usePuBank=*/true);

  std::string path = tempPath("pc1500emu_state_roundtrip_test.state");
  std::string err;
  CHECK(pc1500host::saveStateFile(bus, path, &err));
  CHECK(err.empty());

  pc1500::Keyboard kb2;
  pc1500::Bus bus2(kb2);
  err.clear();
  CHECK(pc1500host::loadStateFile(bus2, path, &err));
  CHECK(err.empty());

  CHECK(bus2.readME0(0x0010) == 0xAB);
  CHECK(bus2.readME0(0x4010) == 0xCD);
  CHECK(bus2.readME0(0x6000) == 0xEF);
  CHECK(bus2.readME0(0x7FFF) == 0x99);

  CHECK(bus2.extRam4800Size() == 0x2000);
  CHECK(bus2.extRam0000Size() == 0x4000);

  CHECK(bus2.romModuleLoaded());
  CHECK(bus2.romModule2Loaded());

  std::remove(path.c_str());
}

void testCorruptMagicRejected() {
  pc1500::Keyboard kb;
  pc1500::Bus bus(kb);
  bus.setExtRam4800Size(0x1000);

  std::string path = tempPath("pc1500emu_state_roundtrip_test_badmagic.state");
  std::string err;
  CHECK(pc1500host::saveStateFile(bus, path, &err));

  // Corrupt the magic bytes.
  {
    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(0);
    f.write("XXXX", 4);
  }

  pc1500::Keyboard kb2;
  pc1500::Bus bus2(kb2);
  err.clear();
  CHECK(!pc1500host::loadStateFile(bus2, path, &err));
  CHECK(!err.empty());
  // A fresh Bus's defaults must be untouched by the rejected load.
  CHECK(bus2.extRam4800Size() == 0);

  std::remove(path.c_str());
}

void testTruncatedFileRejected() {
  pc1500::Keyboard kb;
  pc1500::Bus bus(kb);
  bus.setExtRam0000Size(0x4000);

  std::string fullPath = tempPath("pc1500emu_state_roundtrip_test_full.state");
  std::string err;
  CHECK(pc1500host::saveStateFile(bus, fullPath, &err));

  // Truncate to just the header -- well short of a full RAM dump.
  std::string truncPath = tempPath("pc1500emu_state_roundtrip_test_trunc.state");
  {
    std::ifstream in(fullPath, std::ios::binary);
    std::ofstream out(truncPath, std::ios::binary);
    char buf[16];
    in.read(buf, sizeof(buf));
    out.write(buf, in.gcount());
  }

  pc1500::Keyboard kb2;
  pc1500::Bus bus2(kb2);
  err.clear();
  CHECK(!pc1500host::loadStateFile(bus2, truncPath, &err));
  CHECK(!err.empty());

  std::remove(fullPath.c_str());
  std::remove(truncPath.c_str());
}

}  // namespace

int main() {
  testRoundTrip();
  testCorruptMagicRejected();
  testTruncatedFileRejected();

  if (g_failures == 0) {
    std::printf("All tests passed.\n");
    return 0;
  }
  std::printf("%d check(s) failed.\n", g_failures);
  return 1;
}
