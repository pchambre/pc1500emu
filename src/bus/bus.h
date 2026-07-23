#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "keyboard.h"
#include "lh5801.h"

namespace pc1500 {

// LH5810/LH5811 I/O port controller, mapped into ME1 at F000H-F00FH (see
// docs/pc1500_hardware_reference.md). Register selected by the low 4 bits
// of the address (RS3-RS0). Only the registers actually used on a stock
// PC-1500 are modeled with real behavior (DDA/OPA for keyboard column
// strobe, OPB for the ON-key mirror on PB7); the rest are plain
// read/write storage with no further side effects yet.
class IoPortController {
 public:
  uint8_t read(uint8_t reg) const;
  void write(uint8_t reg, uint8_t value);

  // PA0-PA7 output value, gated by DDA (bits set to input read back as 1,
  // matching an undriven/pulled-up line) -- this is what the keyboard's
  // column strobe sees.
  uint8_t opaOutput() const;

  // PB7 is hardwired as the ON-key input regardless of DDB (see
  // docs/pc1500_hardware_reference.md) -- live level, not a latch, so it
  // tracks the physical key state directly.
  void setOnKeyLine(bool pressed) { onKeyLine_ = pressed; }

 private:
  uint8_t dda_ = 0;
  uint8_t ddb_ = 0;
  uint8_t opa_ = 0;
  uint8_t opb_ = 0;
  uint8_t opc_ = 0;
  uint8_t f_ = 0;
  uint8_t g_ = 0;
  uint8_t msk_ = 0;
  uint8_t if_ = 0;
  bool onKeyLine_ = false;
};

// PC-1500 memory map (see docs/pc1500_hardware_reference.md). ME0 holds
// ROM/RAM/display-buffer; ME1 holds only the I/O port controller. Keyboard
// row-sensing (IN0-IN7) is direct CPU input, not through ME0/ME1 at all --
// exposed here via readInputPort() since it's the same "what the CPU sees
// when it asks the bus" role as ME0/ME1.
class Bus : public lh5801::MemoryBus {
 public:
  explicit Bus(Keyboard& keyboard) : keyboard_(keyboard) {}

  uint8_t readME0(uint16_t addr) override;
  void writeME0(uint16_t addr, uint8_t value) override;
  uint8_t readME1(uint16_t addr) override;
  void writeME1(uint16_t addr, uint8_t value) override;
  uint8_t readInputPort() override;

  // Loads `size` bytes at `addr` into ME0 (e.g. a real PC-1500 ROM dump at
  // 0xC000, once the caller has one -- none is bundled here, for obvious
  // licensing reasons). Bytes beyond the target region's bounds are not
  // written.
  void loadME0(uint16_t addr, const uint8_t* data, size_t size);

  IoPortController& ioPort() { return io_; }

 private:
  static bool isRom(uint16_t addr) { return addr >= 0xC000; }
  static bool isUnmapped(uint16_t addr) {
    return (addr <= 0x3FFF) ||                    // option user memory (no module)
           (addr >= 0x4800 && addr <= 0x6FFF) ||   // module RAM (no module installed)
           (addr >= 0x8000 && addr <= 0xBFFF);     // CE-150/153/158 (not connected)
  }
  // 7C00H-7FFFH is a duplicate of 7800H-7BFFH (the 1K system RAM) -- a
  // half-decoded chip-select block, confirmed directly on real hardware.
  //
  // The PC-2 memory map (TRS-80 Microcomputer News, March 1983, p.26)
  // claims all of 7000H-75FFH duplicates 7600H-7BFFH, but real-hardware
  // testing shows that's overstated: 7000H/7100H do mirror 7600H/7700H,
  // but 7400H does NOT mirror 7A00H (they hold independently different
  // values). The actual mirrored range is just 7000H-71FFH -> 7600H-77FFH
  // (512 bytes, matching the display chip-select block); 7200H-75FFH is
  // real, independent RAM. 4000H-47FFH (the 2K user RAM) is NOT mirrored
  // into 4800H-4FFFH either.
  static uint16_t effectiveAddr(uint16_t addr) {
    if (addr >= 0x7000 && addr <= 0x71FF) return static_cast<uint16_t>(addr + 0x0600);
    if (addr >= 0x7C00 && addr <= 0x7FFF) return static_cast<uint16_t>(addr - 0x0400);
    return addr;
  }

  std::array<uint8_t, 65536> me0_{};
  IoPortController io_;
  Keyboard& keyboard_;
};

}  // namespace pc1500
