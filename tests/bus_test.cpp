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

  // Inhibited/unused region.
  CHECK(bus.readME0(0x7000) == 0xFF);
  // CE-150/153/158 region, not connected.
  CHECK(bus.readME0(0x9000) == 0xFF);
}

void testRomIsReadOnly() {
  pc1500::Keyboard kb;
  pc1500::Bus bus(kb);
  bus.writeME0(0xC000, 0x99);
  CHECK(bus.readME0(0xC000) == 0x00);  // write to ROM region ignored

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

void testMe1OnlyMapsIoPortControllerRange() {
  pc1500::Keyboard kb;
  pc1500::Bus bus(kb);
  bus.writeME1(0xF00C, 0xFF);  // DDA = all outputs
  bus.writeME1(0xF00E, 0x55);  // OPA = 0x55
  CHECK(bus.readME1(0xF00E) == 0x55);
  CHECK(bus.readME1(0x1234) == 0xFF);  // nothing else mapped in ME1
  bus.writeME1(0x1234, 0x00);          // write outside F000-F00F is a no-op
  CHECK(bus.readME1(0x1234) == 0xFF);
}

}  // namespace

int main() {
  testUnmappedRegionsReadHighAndIgnoreWrites();
  testRomIsReadOnly();
  testRamRegionsAreReadWrite();
  testIoPortControllerDdaGatesOpaReadback();
  testMe1OnlyMapsIoPortControllerRange();

  if (g_failures == 0) {
    std::printf("All tests passed.\n");
    return 0;
  }
  std::printf("%d check(s) failed.\n", g_failures);
  return 1;
}
