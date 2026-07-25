#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

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
  // me0_ defaults to 0xFF, not 0x00: real SRAM commonly powers up with a
  // consistent non-zero bias rather than all-zero, and the system ROM's
  // MODE-key handler (gated on 79FFH != 0, confirmed via a real-hardware
  // PEEK immediately after ALL RESET + CL with fresh batteries -- no
  // code path anywhere in the ROM ever writes that address) relies on
  // this undocumented hardware behavior. See docs/pc1500_hardware_reference.md.
  explicit Bus(Keyboard& keyboard) : keyboard_(keyboard) { me0_.fill(0xFF); }

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

  // Wraps Keyboard::setKeyState with two hardware-behavior fixes confirmed
  // on real PC-1500 hardware (extensive testing this session):
  //
  // 1. Releasing a non-cursor key, with nothing else held, makes the
  //    machine ready for the *next* keypress immediately -- no perceptible
  //    delay, even typing several keys a second. The ROM's own
  //    key-dispatch state machine (RAM flag at 7B0EH bit 0, traced in
  //    detail via disassembly) only clears that flag after an
  //    ~8-timer-period software countdown, which doesn't match observed
  //    hardware behavior for ordinary keys; cursor keys (used for
  //    typematic-style rollover while held, confirmed at ~5/sec -- see
  //    advanceCycles) are the one case that countdown does seem to
  //    legitimately govern. We couldn't find or reproduce whatever the
  //    real fast-path mechanism is (possibly hardware-level, per the PC-2
  //    manual's vague "key input routine" MPU function), so this forces
  //    the observed outcome directly: clear 7B0EH bit 0 the moment a
  //    non-cursor release leaves nothing held.
  //
  // 2. The actual release is deferred by kMinimumHoldCycles (see
  //    applyRelease) rather than applied immediately, so a host keypress
  //    briefer than one ROM timer-interrupt period can't go all the way
  //    down and back up between two keyboard scans without any scan ever
  //    seeing it.
  void setKeyState(Key key, bool pressed);

  // Drives cursor-key rollover and deferred key releases (see
  // setKeyState): call once per emulated cycle batch (see main.cpp's frame
  // loop) with the number of cycles just executed.
  //
  // Rollover: while a cursor key is held, the ROM's dispatch never re-fires
  // on its own (traced directly: holding Left for 1.5M cycles touches
  // 7B0EH exactly once, at the initial press), so this clears bit 0
  // periodically at the ~5/sec rate confirmed on real hardware, letting
  // the ROM's own dispatch logic re-trigger a repeat.
  void advanceCycles(int cycles);

 private:
  // Applies a release that setKeyState deferred: updates Keyboard, and (for
  // non-cursor keys) the same 7B0EH-bit-0 clear setKeyState's comment
  // describes, now that the release is actually taking effect.
  void applyRelease(Key key);


  static bool isRom(uint16_t addr) { return addr >= 0xC000; }
  static bool isUnmapped(uint16_t addr) {
    return (addr <= 0x3FFF) ||                    // option user memory (no module)
           (addr >= 0x4800 && addr <= 0x6FFF) ||   // module RAM (no module installed)
           (addr >= 0x8000 && addr <= 0xBFFF);     // CE-150/153/158 (not connected)
  }
  // 7C00H-7FFFH is a duplicate of 7800H-7BFFH (the 1K system RAM) -- a
  // half-decoded chip-select block, confirmed directly on real hardware.
  //
  // 7000H-77FFH is driven by a RAM chip smaller than its address window:
  // address bits 9 and 10 (0200H/0400H) simply aren't decoded, so every
  // address in this range aliases the one with those two bits forced high
  // (i.e. ORed with 0600H). Confirmed directly on real hardware for
  // 7000H<->7600H, 7100H<->7700H, and 7400H<->7600H, and for 7400H NOT
  // aliasing 7A00H (bit 11 differs, outside this window). This supersedes
  // an earlier, narrower model (7000H-71FFH only, based on the PC-2 memory
  // map in TRS-80 Microcomputer News, March 1983, p.26, which called the
  // whole 7000H-75FFH range a duplicate of 7600H-7BFFH -- closer to
  // correct than our first, narrower correction, just off on the exact
  // mechanism). 4000H-47FFH (the 2K user RAM) is NOT mirrored into
  // 4800H-4FFFH.
  static uint16_t effectiveAddr(uint16_t addr) {
    if (addr >= 0x7000 && addr <= 0x77FF) return static_cast<uint16_t>(addr | 0x0600);
    if (addr >= 0x7C00 && addr <= 0x7FFF) return static_cast<uint16_t>(addr - 0x0400);
    return addr;
  }

  std::array<uint8_t, 65536> me0_{};
  IoPortController io_;
  Keyboard& keyboard_;

  // Cursor-key rollover state (see advanceCycles).
  bool cursorKeyHeld_ = false;
  Key heldCursorKey_ = Key::Left;
  int cursorRepeatCycles_ = 0;
  // ~154ms at 1.3MHz: Paul's own "~5/sec" estimate, refined after
  // side-by-side comparison to hardware (initial 200ms guess ran ~30%
  // slower than the real repeat rate).
  static constexpr int kCursorRepeatCycles = 200000;

  // Deferred-release state (see setKeyState/advanceCycles/applyRelease).
  struct PendingRelease {
    Key key;
    int cyclesRemaining;
  };
  std::vector<PendingRelease> pendingReleases_;
  // Comfortably more than one ~32768-cycle timer period (512 ticks x 64
  // cycles/tick), so a release is never applied before at least one full
  // keyboard-scan period has had a chance to see the key down.
  static constexpr int kMinimumHoldCycles = 40000;
};

}  // namespace pc1500
