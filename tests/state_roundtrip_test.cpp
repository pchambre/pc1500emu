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
#include "lh5801.h"
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
  lh5801::CPU cpu(bus);

  // Ext-RAM windows must be sized *before* writing into them --
  // writeME0 drops writes to an unmapped (size-0) window (see
  // Bus::isUnmapped), so this order matters.
  bus.setExtRamExtSize(0x2000);
  bus.setExtRam0000Size(0x4000);

  // RAM contents -- a few distinguishing bytes spread across the saved
  // range, including right at its very last byte (0x7FFF).
  bus.writeME0(0x0010, 0xAB);  // 0000H window (now sized)
  bus.writeME0(0x4010, 0xCD);  // built-in 2K RAM, never gated
  bus.writeME0(0x6000, 0xEF);  // 4800H window (now sized), offset 0x1800 < 0x2000
  bus.writeME0(0x7FFF, 0x99);  // system RAM, mirrors to 0x7BFF, never gated

  uint8_t module1Data[4] = {0x11, 0x22, 0x33, 0x44};
  bus.loadRomModule(0, module1Data, sizeof(module1Data), 0xA000, /*requirePv=*/false,
                     /*usePuBank=*/false);
  uint8_t module2Data[4] = {0x55, 0x66, 0x77, 0x88};
  bus.loadRomModule(1, module2Data, sizeof(module2Data), 0x8000, /*requirePv=*/true,
                     /*usePuBank=*/true);

  // An expansion module (writable data window + mock command processing) --
  // slot 2 doesn't collide with module1/module2's PV/base combinations
  // above, so it's live at the same time.
  uint8_t module3Data[2] = {0x55, 0xAA};
  bus.loadExpansionModule(2, module3Data, sizeof(module3Data), 0x9000, /*requirePv=*/false,
                           /*usePuBank=*/false, /*dataWindowBase=*/0x8800,
                           /*dataWindowSize=*/0x0100, /*instructionAddr=*/0x88FF);
  bus.writeME0(0x8800, 0x7E);  // distinguishing byte in the data window

  // Distinguishing CPU state -- this is the whole point of the redesign
  // that dropped cpu.reset() from the restore path: a resumed session
  // must land exactly on these values, not a freshly-reset CPU.
  cpu.setP(0x1234);
  cpu.setS(0x7A00);
  cpu.setX(0x1111);
  cpu.setY(0x2222);
  cpu.setU(0x3333);
  cpu.setHalted(true);
  cpu.requestMI();
  cpu.tickTimer();  // nudges timerCounter_ off its 0 fixed-point, per tickTimer()'s own comment

  std::string path = tempPath("pc1500emu_state_roundtrip_test.state");
  std::string err;
  CHECK(pc1500host::saveStateFile(cpu, bus, path, &err));
  CHECK(err.empty());

  pc1500::Keyboard kb2;
  pc1500::Bus bus2(kb2);
  lh5801::CPU cpu2(bus2);
  // loadState now rejects a config mismatch outright (see Bus::loadState's
  // own comment) -- bus2 must be configured to match `bus` *before* the
  // load, the same way main.cpp now applies AppConfig before attempting
  // one, rather than relying on the load itself to set these fields.
  bus2.setExtRamExtSize(0x2000);
  bus2.setExtRam0000Size(0x4000);
  err.clear();
  CHECK(pc1500host::loadStateFile(cpu2, bus2, path, &err));
  CHECK(err.empty());

  CHECK(bus2.readME0(0x0010) == 0xAB);
  CHECK(bus2.readME0(0x4010) == 0xCD);
  CHECK(bus2.readME0(0x6000) == 0xEF);
  CHECK(bus2.readME0(0x7FFF) == 0x99);

  CHECK(bus2.extRamExtSize() == 0x2000);
  CHECK(bus2.extRam0000Size() == 0x4000);

  CHECK(bus2.romModuleLoaded(0));
  CHECK(bus2.romModuleLoaded(1));
  CHECK(bus2.romModuleLoaded(2));
  CHECK(!bus2.romModuleLoaded(3));

  // Expansion module's data window survives the round trip, including the
  // live write made before saving -- not just the static ROM bytes.
  CHECK(bus2.readME0(0x9000) == 0x55);
  CHECK(bus2.readME0(0x8800) == 0x7E);
  CHECK(bus2.readME0(0x88FF) == 0xFF);  // untouched byte still at its power-up default

  CHECK(cpu2.p() == 0x1234);
  CHECK(cpu2.s() == 0x7A00);
  CHECK(cpu2.x() == 0x1111);
  CHECK(cpu2.y() == 0x2222);
  CHECK(cpu2.u() == 0x3333);
  CHECK(cpu2.halted() == true);
  CHECK(cpu2.timerCounter() == cpu.timerCounter());

  std::remove(path.c_str());
}

// CE-163 is mutually exclusive with the two extRam windows testRoundTrip
// already exercises (see Bus::setCe163Enabled's own comment), so it needs
// its own save/load pass: enabled flag, active bank, and -- the actual
// point of having two banks -- BOTH banks' contents, not just the one
// currently selected at save time.
void testCe163RoundTrip() {
  pc1500::Keyboard kb;
  pc1500::Bus bus(kb);
  lh5801::CPU cpu(bus);

  bus.setCe163Enabled(true);
  bus.writeME0(0x0010, 0xAA);   // bank 0
  bus.writeME0(0x3FFF, 0xBB);
  bus.writeME0(0x5801, 0x00);   // switch to bank 1 (odd address; value irrelevant)
  bus.writeME0(0x0010, 0xCC);   // bank 1
  bus.writeME0(0x3FFF, 0xDD);
  bus.writeME0(0x5801, 0x00);   // switch back to bank 1 again -- stay here for the save,
                                 // to confirm the *active* bank round-trips too, not just 0

  std::string path = tempPath("pc1500emu_state_roundtrip_test_ce163.state");
  std::string err;
  CHECK(pc1500host::saveStateFile(cpu, bus, path, &err));
  CHECK(err.empty());

  pc1500::Keyboard kb2;
  pc1500::Bus bus2(kb2);
  lh5801::CPU cpu2(bus2);
  // Must match bus's config before loading -- see testRoundTrip's own
  // comment on this same pattern.
  bus2.setCe163Enabled(true);
  err.clear();
  CHECK(pc1500host::loadStateFile(cpu2, bus2, path, &err));
  CHECK(err.empty());

  CHECK(bus2.ce163Enabled() == true);
  CHECK(bus2.ce163Bank() == 1);
  CHECK(bus2.readME0(0x0010) == 0xCC);  // bank 1, still selected after load
  CHECK(bus2.readME0(0x3FFF) == 0xDD);

  bus2.writeME0(0x5800, 0xFF);  // switch to bank 0 (even address; value irrelevant)
  CHECK(bus2.readME0(0x0010) == 0xAA);  // bank 0's contents survived the round trip too
  CHECK(bus2.readME0(0x3FFF) == 0xBB);

  std::remove(path.c_str());
}

// Version 5 additions: the machine-variant flag and CE-155's own enabled
// flag. CE-155 is mutually exclusive with everything testRoundTrip/
// testCe163RoundTrip already exercise (see Bus::setCe155Enabled's own
// comment), so it needs its own pass, same reasoning as CE-163's own
// dedicated test above. PC-1500A is the interesting case for the
// variant flag -- PC-1500 is Bus's own default, so a round trip through
// that alone wouldn't distinguish "saved as PC1500" from "field never
// read at all".
void testMachineVariantAndCe155RoundTrip() {
  pc1500::Keyboard kb;
  pc1500::Bus bus(kb);
  lh5801::CPU cpu(bus);

  bus.setMachineVariant(pc1500::Bus::MachineVariant::PC1500A);
  bus.setCe155Enabled(true);
  bus.writeME0(0x3800, 0x11);   // CE-155's isolated 2K
  bus.writeME0(0x5800, 0x22);   // expansion window, PC-1500A base

  std::string path = tempPath("pc1500emu_state_roundtrip_test_variant.state");
  std::string err;
  CHECK(pc1500host::saveStateFile(cpu, bus, path, &err));
  CHECK(err.empty());

  pc1500::Keyboard kb2;
  pc1500::Bus bus2(kb2);
  lh5801::CPU cpu2(bus2);
  // Must match bus's config before loading -- see testRoundTrip's own
  // comment on this same pattern.
  bus2.setMachineVariant(pc1500::Bus::MachineVariant::PC1500A);
  bus2.setCe155Enabled(true);
  err.clear();
  CHECK(pc1500host::loadStateFile(cpu2, bus2, path, &err));
  CHECK(err.empty());

  CHECK(bus2.machineVariant() == pc1500::Bus::MachineVariant::PC1500A);
  CHECK(bus2.ce155Enabled() == true);
  CHECK(bus2.readME0(0x3800) == 0x11);
  CHECK(bus2.readME0(0x5800) == 0x22);

  std::remove(path.c_str());
}

void testCorruptMagicRejected() {
  pc1500::Keyboard kb;
  pc1500::Bus bus(kb);
  lh5801::CPU cpu(bus);
  bus.setExtRamExtSize(0x1000);

  std::string path = tempPath("pc1500emu_state_roundtrip_test_badmagic.state");
  std::string err;
  CHECK(pc1500host::saveStateFile(cpu, bus, path, &err));

  // Corrupt the magic bytes.
  {
    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(0);
    f.write("XXXX", 4);
  }

  pc1500::Keyboard kb2;
  pc1500::Bus bus2(kb2);
  lh5801::CPU cpu2(bus2);
  err.clear();
  CHECK(!pc1500host::loadStateFile(cpu2, bus2, path, &err));
  CHECK(!err.empty());
  // A fresh Bus's defaults must be untouched by the rejected load.
  CHECK(bus2.extRamExtSize() == 0);

  std::remove(path.c_str());
}

void testTruncatedFileRejected() {
  pc1500::Keyboard kb;
  pc1500::Bus bus(kb);
  lh5801::CPU cpu(bus);
  bus.setExtRam0000Size(0x4000);

  std::string fullPath = tempPath("pc1500emu_state_roundtrip_test_full.state");
  std::string err;
  CHECK(pc1500host::saveStateFile(cpu, bus, fullPath, &err));

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
  lh5801::CPU cpu2(bus2);
  err.clear();
  CHECK(!pc1500host::loadStateFile(cpu2, bus2, truncPath, &err));
  CHECK(!err.empty());

  std::remove(fullPath.c_str());
  std::remove(truncPath.c_str());
}

}  // namespace

int main() {
  testRoundTrip();
  testCe163RoundTrip();
  testMachineVariantAndCe155RoundTrip();
  testCorruptMagicRejected();
  testTruncatedFileRejected();

  if (g_failures == 0) {
    std::printf("All tests passed.\n");
    return 0;
  }
  std::printf("%d check(s) failed.\n", g_failures);
  return 1;
}
