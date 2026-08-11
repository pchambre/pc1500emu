#include <cstdint>
#include <cstdio>

#include "bus.h"
#include "keyboard.h"

namespace {

int g_failures = 0;

// uPD1990AC RTC test helpers -- see Upd1990ac in bus.h. Command bits are
// latched on PC1 (STB, 0x02) rising edges with PC3-5 (0x08/0x10/0x20)
// holding C0/C1/C2; data is clocked on PC2 (CLK, 0x04) rising edges with
// PC0 (0x01) holding the bit. All three sequences below write the
// low-6-bits value twice (once at the old level, once at the new) purely
// so a human reading a trace sees an explicit "before/after" pair --
// IoPortController only reacts to the transition itself.
void rtcLatchCommand(pc1500::IoPortController& io, bool c0, bool c1, bool c2) {
  uint8_t levels = static_cast<uint8_t>((c0 ? 0x08 : 0) | (c1 ? 0x10 : 0) | (c2 ? 0x20 : 0));
  io.write(0x08, levels);
  io.write(0x08, static_cast<uint8_t>(levels | 0x02));  // STB rising edge
  io.write(0x08, levels);
}

// Feeds `value`'s 40 low bits in LSB-first, i.e. bit i is fed on the i-th
// clock. Since the shift register shifts right with new bits entering at
// bit 39, the last bit fed (value's bit 39) ends up at bit 39 and the
// first bit fed (value's bit 0) ends up at bit 0 after all 40 clocks --
// i.e. the register ends up equal to `value` itself. Must be called with
// Register Shift or Time Set already latched (see rtcLatchCommand).
void rtcShiftIn(pc1500::IoPortController& io, uint64_t value) {
  for (int i = 0; i < 40; i++) {
    uint8_t dataBit = static_cast<uint8_t>((value >> i) & 1);
    io.write(0x08, dataBit);
    io.write(0x08, static_cast<uint8_t>(dataBit | 0x04));  // CLK rising edge
  }
}

// Inverse of rtcShiftIn: samples DATA OUT (OPB bit 6) before each of 40
// clocks, so bit i of the result is the register's bit i at the time it
// was sampled -- reconstructing the pre-shift register value bit-for-bit.
uint64_t rtcShiftOut(pc1500::IoPortController& io) {
  uint64_t value = 0;
  for (int i = 0; i < 40; i++) {
    if (io.read(0x0F) & 0x40) value |= (static_cast<uint64_t>(1) << i);
    io.write(0x08, 0x04);  // CLK rising edge (DATA IN doesn't matter here)
    io.write(0x08, 0x00);
  }
  return value;
}

uint64_t rtcPackBcd40(int month, int dow, int day, int hour, int minute, int second) {
  auto bcdNibbles = [](int v) { return std::make_pair(v / 10, v % 10); };
  auto [dayTens, dayUnits] = bcdNibbles(day);
  auto [hourTens, hourUnits] = bcdNibbles(hour);
  auto [minTens, minUnits] = bcdNibbles(minute);
  auto [secTens, secUnits] = bcdNibbles(second);
  uint64_t reg = 0;
  reg |= static_cast<uint64_t>(secUnits) << 0;
  reg |= static_cast<uint64_t>(secTens) << 4;
  reg |= static_cast<uint64_t>(minUnits) << 8;
  reg |= static_cast<uint64_t>(minTens) << 12;
  reg |= static_cast<uint64_t>(hourUnits) << 16;
  reg |= static_cast<uint64_t>(hourTens) << 20;
  reg |= static_cast<uint64_t>(dayUnits) << 24;
  reg |= static_cast<uint64_t>(dayTens) << 28;
  reg |= static_cast<uint64_t>(dow) << 32;
  reg |= static_cast<uint64_t>(month) << 36;
  return reg;
}

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

void testOpcBit6ControlsBuzzerOn() {
  // PC6 (OPC bit 6) is the buzzer on/off control -- see
  // docs/pc1500_hardware_reference.md's I/O-PC pin table. buzzerOn()
  // is what main.cpp's audio-sample generation reads every cycle
  // (real BASIC BEEP works by the ROM toggling this bit at audio rate
  // in a timed software loop), so it must track OPC bit 6 exactly, in
  // both directions and independent of the register's other bits.
  pc1500::IoPortController io;
  CHECK(!io.buzzerOn());
  io.write(0x08, 0x40);
  CHECK(io.buzzerOn());
  io.write(0x08, 0xFF);
  CHECK(io.buzzerOn());
  io.write(0x08, 0xBF);  // every bit except PC6
  CHECK(!io.buzzerOn());
  io.write(0x08, 0x00);
  CHECK(!io.buzzerOn());
}

void testSerialTransmitSetsTdFlagAfterProgrammedDelay() {
  // Confirmed via CE-150 (printer/cassette interface) ROM disassembly:
  // it polls IF bit 3 (0x08) clear before writing the next byte to
  // register 0x06, i.e. writing 0x06 starts a transmission that clears
  // TD, and TD sets again once the transmission (start + 8 data + 2 stop
  // bits, at the rate F's low 3 bits select) finishes.
  pc1500::IoPortController io;
  io.write(0x07, 0x00);  // F divisor bits = 0 -> fastest rate (1 cycle/bit)
  io.write(0x06, 0xA5);  // start a transmission
  CHECK((io.read(0x0B) & 0x08) == 0x00);  // TD clear while busy
  io.advanceCycles(10);
  CHECK((io.read(0x0B) & 0x08) == 0x00);  // not done yet (11 bit periods needed)
  io.advanceCycles(1);
  CHECK((io.read(0x0B) & 0x08) == 0x08);  // TD set once the 11th bit period elapses
}

void testSerialTransmitDelayScalesWithClockDivisor() {
  pc1500::IoPortController io;
  io.write(0x07, 0x01);  // F divisor bits = 1 -> divisor 2 -> 22 cycles total
  io.write(0x06, 0x00);
  io.advanceCycles(21);
  CHECK((io.read(0x0B) & 0x08) == 0x00);
  io.advanceCycles(1);
  CHECK((io.read(0x0B) & 0x08) == 0x08);
}

void testSerialTransmitRegisterIsWriteOnly() {
  // Per the Service Manual's register table, 0x06 has no read column
  // (it's a trigger, not readable storage).
  pc1500::IoPortController io;
  io.write(0x06, 0x42);
  CHECK(io.read(0x06) == 0xFF);
}

void testSerialReceiveReadClearsRdFlag() {
  // U (register 0x05) is documented as "write: n/a" / "read: contents of
  // U, resets RD flag" -- no ROM code path exercising real reception has
  // been traced, so this only checks the read-clears-the-flag mechanics,
  // not that anything ever sets RD in the first place.
  pc1500::IoPortController io;
  io.write(0x0B, 0x04);  // force IF bit 2 (RD, per our best-guess bit assignment) set
  CHECK((io.read(0x0B) & 0x04) == 0x04);
  io.read(0x05);
  CHECK((io.read(0x0B) & 0x04) == 0x00);
  io.write(0x05, 0x99);  // write is documented as a no-op
  CHECK(io.read(0x05) == 0x00);
}

void testRtcTpRateSelectAndIfBitLatch() {
  // Confirmed via ROM1.BIN disassembly (E890-E8B4): BASIC BEEP's
  // repeat-gap wait issues this exact TP=64Hz command (C2=1,C1=0,C0=0)
  // before polling PB5/IF bit 1 -- see Upd1990ac's class comment. Default
  // rate is already 64Hz, so this test explicitly selects 256Hz instead,
  // to prove the command genuinely changes behavior rather than
  // coincidentally matching the default.
  pc1500::IoPortController io;
  io.testFreezeRtcClock();  // see its own comment -- must precede latchCommand
  rtcLatchCommand(io, /*c0=*/true, /*c1=*/false, /*c2=*/true);  // TP = 256 Hz

  CHECK((io.read(0x0B) & 0x02) == 0x00);  // IF bit 1 clear initially
  CHECK((io.read(0x0F) & 0x20) == 0x00);  // PB5 (TP) starts low

  double halfPeriod = 0.5 / 256.0;
  io.testAdvanceRtcSeconds(halfPeriod * 1.01);  // just over one half-period -> one rising edge
  CHECK((io.read(0x0B) & 0x02) == 0x02);  // IF bit 1 now latched
  CHECK((io.read(0x0F) & 0x20) == 0x20);  // PB5 mirrors TP's new (high) level

  io.write(0x0B, 0x00);  // ROM clears IF explicitly before re-polling
  io.testAdvanceRtcSeconds(halfPeriod * 1.01);  // one more half-period -> falling edge only
  CHECK((io.read(0x0B) & 0x02) == 0x00);  // falling edge must not re-set IF bit 1
  CHECK((io.read(0x0F) & 0x20) == 0x00);  // PB5 now low again
}

void testRtcTimeSetThenTimeReadRoundTrip() {
  // Exercises the full documented protocol end to end: Time Set shifts a
  // new value in and (on leaving Time Set) commits it as the chip's live
  // time; Time Read then snapshots that live time back into the shift
  // register for Register Shift to read back out. Day 15 is used
  // specifically so it's valid in every month (avoids a Feb-29 edge case),
  // since month/year aren't independently controlled here -- the chip
  // itself has no year field at all (see commitShiftRegisterToTime()).
  pc1500::IoPortController io;
  uint64_t setValue = rtcPackBcd40(/*month=*/5, /*dow=*/2, /*day=*/15, /*hour=*/10,
                                    /*minute=*/30, /*second=*/45);

  rtcLatchCommand(io, /*c0=*/false, /*c1=*/true, /*c2=*/false);  // Time Set & Counter Hold
  rtcShiftIn(io, setValue);
  rtcLatchCommand(io, /*c0=*/false, /*c1=*/false, /*c2=*/false);  // Register Hold -> commits

  rtcLatchCommand(io, /*c0=*/true, /*c1=*/true, /*c2=*/false);   // Time Read -> snapshot
  rtcLatchCommand(io, /*c0=*/true, /*c1=*/false, /*c2=*/false);  // Register Shift -> can clock out
  uint64_t readBack = rtcShiftOut(io);

  auto nibble = [readBack](int shift) { return static_cast<int>((readBack >> shift) & 0x0F); };
  CHECK(nibble(36) == 5);                        // month
  CHECK(nibble(28) * 10 + nibble(24) == 15);      // day
  CHECK(nibble(20) * 10 + nibble(16) == 10);      // hour
  CHECK(nibble(12) * 10 + nibble(8) == 30);       // minute
  CHECK(nibble(4) * 10 + nibble(0) == 45);        // second
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

void testExtensionRam4800WindowSizesGateMappedRange() {
  pc1500::Keyboard kb;
  pc1500::Bus bus(kb);
  // Off by default -- matches the no-module behavior every other test
  // above already assumes.
  CHECK(bus.extRam4800Size() == 0);
  CHECK(bus.readME0(0x4800) == 0xFF);
  bus.writeME0(0x4800, 0x42);
  CHECK(bus.readME0(0x4800) == 0xFF);  // write ignored while disabled

  // A partial (4K) module only maps the start of the window -- the rest
  // stays unmapped, matching a physically smaller module not filling its
  // whole socket.
  bus.setExtRam4800Size(0x1000);
  bus.writeME0(0x4800, 0x42);
  CHECK(bus.readME0(0x4800) == 0x42);
  bus.writeME0(0x57FF, 0x11);  // last byte of the 4K module (4800H + 1000H - 1)
  CHECK(bus.readME0(0x57FF) == 0x11);
  CHECK(bus.readME0(0x5800) == 0xFF);  // one past the 4K module -- still unmapped
  bus.writeME0(0x5800, 0x99);
  CHECK(bus.readME0(0x5800) == 0xFF);

  // Growing to the full 10K window maps the rest too, without disturbing
  // what was already there.
  bus.setExtRam4800Size(pc1500::Bus::kExtRam4800WindowSize);
  CHECK(bus.readME0(0x4800) == 0x42);  // preserved from the 4K-module write above
  bus.writeME0(0x6FFF, 0x99);          // last byte of the full 10K window
  CHECK(bus.readME0(0x6FFF) == 0x99);
  CHECK(bus.readME0(0x7000) == 0xFF);  // one past the window -- untouched, still its own default

  // Shrinking back to disabled doesn't clear the underlying bytes.
  bus.setExtRam4800Size(0);
  CHECK(bus.readME0(0x4800) == 0xFF);  // reads as unmapped again
  bus.setExtRam4800Size(pc1500::Bus::kExtRam4800WindowSize);
  CHECK(bus.readME0(0x4800) == 0x42);  // but the byte was preserved underneath
}

void testExtensionRam0000WindowSizeGatesMappedRange() {
  pc1500::Keyboard kb;
  pc1500::Bus bus(kb);
  CHECK(bus.extRam0000Size() == 0);
  CHECK(bus.readME0(0x0000) == 0xFF);
  bus.writeME0(0x0000, 0x77);
  CHECK(bus.readME0(0x0000) == 0xFF);  // write ignored while disabled

  bus.setExtRam0000Size(pc1500::Bus::kExtRam0000WindowSize);
  bus.writeME0(0x0000, 0x77);
  CHECK(bus.readME0(0x0000) == 0x77);
  bus.writeME0(0x3FFF, 0x88);  // last byte of the 16K window
  CHECK(bus.readME0(0x3FFF) == 0x88);
}

}  // namespace

int main() {
  testUnmappedRegionsReadHighAndIgnoreWrites();
  testMirroredRegionsAliasRealRam();
  testRomIsReadOnly();
  testRamRegionsAreReadWrite();
  testIoPortControllerDdaGatesOpaReadback();
  testOpcBit6ControlsBuzzerOn();
  testSerialTransmitSetsTdFlagAfterProgrammedDelay();
  testSerialTransmitDelayScalesWithClockDivisor();
  testSerialTransmitRegisterIsWriteOnly();
  testSerialReceiveReadClearsRdFlag();
  testRtcTpRateSelectAndIfBitLatch();
  testRtcTimeSetThenTimeReadRoundTrip();
  testMe1MirrorsIoPortControllerWhereAd12Ad13BothSet();
  testExtensionRam4800WindowSizesGateMappedRange();
  testExtensionRam0000WindowSizeGatesMappedRange();

  if (g_failures == 0) {
    std::printf("All tests passed.\n");
    return 0;
  }
  std::printf("%d check(s) failed.\n", g_failures);
  return 1;
}
