#include <cstdio>

#include "bus.h"
#include "keyboard.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                        \
      std::printf("FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__);       \
      g_failures++;                                                       \
    }                                                                     \
  } while (0)

void testUnmappedRegionsReadHighAndIgnoreWrites() {
  pc1500::Keyboard kb;
  pc1500::Bus bus(kb);
  // Option user memory (no module installed).
  CHECK(bus.readME0(0x0000) == 0xFF);
  bus.writeME0(0x0000, 0x42);
  CHECK(bus.readME0(0x0000) == 0xFF);  // write ignored

  // CE-150/153/158 region, not connected.
  CHECK(bus.readME0(0x9000) == 0xFF);
}

void testMirroredRegionsAliasRealRam() {
  pc1500::Keyboard kb;
  pc1500::Bus bus(kb);
  // 7C00H-7FFFH is a duplicate of 7800H-7BFFH (the 1K system RAM),
  // confirmed directly on real hardware.
  bus.writeME0(0x7C00, 0x33);
  CHECK(bus.readME0(0x7800) == 0x33);
  bus.writeME0(0x7FFF, 0x44);
  CHECK(bus.readME0(0x7BFF) == 0x44);

  // 7000H-77FFH is driven by a RAM chip smaller than its address window:
  // bits 9/10 (0200H/0400H) aren't decoded, so every address aliases the
  // one with those bits forced high (addr | 0600H). 7000H<->7600H,
  // 7100H<->7700H, 7200H<->7600H, and 7400H<->7600H are each confirmed
  // directly on real hardware.
  bus.writeME0(0x7000, 0x11);
  CHECK(bus.readME0(0x7600) == 0x11);
  bus.writeME0(0x71FF, 0x22);
  CHECK(bus.readME0(0x77FF) == 0x22);
  bus.writeME0(0x7400, 0x33);
  CHECK(bus.readME0(0x7600) == 0x33);
  bus.writeME0(0x7200, 0x55);
  CHECK(bus.readME0(0x7600) == 0x55);

  bus.writeME0(0x7A00, 0x00);
  bus.writeME0(0x7400, 0x77);
  CHECK(bus.readME0(0x7A00) == 0x00);  // unaffected -- outside this chip-select block
}

void testRomIsReadOnly() {
  pc1500::Keyboard kb;
  pc1500::Bus bus(kb);
  bus.writeME0(0xC000, 0x99);
  CHECK(bus.readME0(0xC000) == 0xFF);  // write to ROM region ignored (0xFF is the unwritten default)

  uint8_t data[] = {0x11, 0x22, 0x33};
  bus.loadME0(0xC000, data, sizeof(data));
  CHECK(bus.readME0(0xC000) == 0x11);
  CHECK(bus.readME0(0xC001) == 0x22);
  CHECK(bus.readME0(0xC002) == 0x33);
}

void testRamRegionsAreReadWrite() {
  pc1500::Keyboard kb;
  pc1500::Bus bus(kb);
  bus.writeME0(0x4000, 0xAB);  // standard user RAM
  CHECK(bus.readME0(0x4000) == 0xAB);
  bus.writeME0(0x7600, 0xCD);  // display buffer, chips 1&3
  CHECK(bus.readME0(0x7600) == 0xCD);
  bus.writeME0(0x7800, 0xEF);  // system RAM
  CHECK(bus.readME0(0x7800) == 0xEF);
}

void testIoPortControllerDdaGatesOpaReadback() {
  pc1500::IoPortController io;
  io.write(0x0C, 0xF0);  // DDA: high nibble output, low nibble input
  io.write(0x0E, 0xAB);  // OPA = 0xAB
  // Output bits (high nibble) reflect opa_; input bits (low nibble) read
  // back as 1 (undriven/pulled-up).
  CHECK(io.opaOutput() == 0xAF);
  CHECK(io.read(0x0E) == 0xAB);  // raw register readback is unaffected by DDA
}

void testMe1MirrorsIoPortControllerWhereAd12Ad13BothSet() {
  // CS0/CS1/CS2 tied to AD12/AD13/(fixed) -- confirmed on real hardware
  // that AD14/AD15 aren't decoded (F00AH/F00BH and B00AH/B00BH read back
  // identical, live values: F000H and B000H agree on bits 12-13, differing
  // only in bit 14). Addresses where bits 12-13 aren't both set stay
  // genuinely unmapped.
  pc1500::Keyboard kb;
  pc1500::Bus bus(kb);
  bus.writeME1(0xF00C, 0xFF);  // DDA = all outputs
  bus.writeME1(0xF00E, 0x55);  // OPA = 0x55
  CHECK(bus.readME1(0xF00E) == 0x55);
  CHECK(bus.readME1(0xB00E) == 0x55);  // same register, aliased address (bits 12-13 agree)
  bus.writeME1(0xB00D, 0xAA);          // write DDB via a different aliased address
  CHECK(bus.readME1(0xF00D) == 0xAA);  // visible through the "canonical" address too
  CHECK(bus.readME1(0x1234) == 0xFF);  // bits 12-13 not both set -- genuinely unmapped
  bus.writeME1(0x1234, 0x00);
  CHECK(bus.readME1(0x1234) == 0xFF);  // write outside the decode is a no-op
}

}  // namespace

int main() {
  testUnmappedRegionsReadHighAndIgnoreWrites();
  testMirroredRegionsAliasRealRam();
  testRomIsReadOnly();
  testRamRegionsAreReadWrite();
  testIoPortControllerDdaGatesOpaReadback();
  testMe1MirrorsIoPortControllerWhereAd12Ad13BothSet();

  if (g_failures == 0) {
    std::printf("All tests passed.\n");
    return 0;
  }
  std::printf("%d check(s) failed.\n", g_failures);
  return 1;
}
