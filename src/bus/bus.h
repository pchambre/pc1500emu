// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <iosfwd>
#include <vector>

#include "keyboard.h"
#include "lh5801.h"

namespace pc1500 {

// NEC uPD1990AC real-time clock chip, wired to the LH5811 I/O port
// controller's PC0-PC5 (control) and PB5/PB6 (status input) pins -- see
// docs/pc1500_hardware_reference.md and Documents/PC1500/UPD1990AC.pdf
// (the real datasheet). A 40-bit BCD shift register holds
// month/day-of-week/day/hour/minute/second; C0-C2 (latched by STB) select
// one of Register Hold/Shift/Time-Set/Time-Read (C2=0) or a TP output rate
// of 64/256/2048 Hz (C2=1) -- two independently-latched command groups,
// per the datasheet's own wording.
//
// Confirmed wired exactly this way via direct ROM1.BIN disassembly: BASIC
// BEEP's repeat-gap wait (E890-E8B4) issues value 0x20 through the shared
// "set OPC low 6 bits + pulse STB" helper at E573 -- i.e. C2=1,C1=0,C0=0 on
// PC5/PC4/PC3 -- which is exactly the datasheet's "TP=64Hz Set" command,
// before polling PB5 (TP's live level) and IF bit 1 (latched from TP's
// rising edge, see IoPortController::advanceRealTime()). That's what
// resolved the mystery of what BEEP's gap timer actually depends on.
//
// TP is driven by real elapsed wall-clock time (advanceRealTime()), not
// CPU cycles: this chip has its own independent 32.768kHz crystal (per the
// Service Manual's block diagram), so its rate is correct regardless of
// how fast the emulated CPU itself is running -- same reasoning as
// main.cpp's audio sample accumulator, just for a different signal.
class Upd1990ac {
 public:
  // Forward every OPC write here with its six control-line levels
  // (PC0=DATA IN, PC1=STB, PC2=CLK, PC3=C0, PC4=C1, PC5=C2 -- confirmed via
  // the E573 ROM helper toggling bit 1 for STB pulses). Real behavior only
  // happens on STB/CLK rising edges, detected internally, so callers don't
  // need to track state themselves.
  void setControlPins(bool dataIn, bool stb, bool clk, bool c0, bool c1, bool c2);

  // PB5 (TP) / PB6 (DATA OUT) live levels -- see IoPortController::read()'s
  // OPB case, which reads these unconditionally regardless of DDB (same
  // treatment as PB7/onKeyLine_, since these are fixed hardwired
  // connections on a stock PC-1500, not general-purpose I/O).
  bool tp() const { return tpLevel_; }
  bool dataOut() const;

  // Advances TP's phase by elapsedSeconds of real time. Returns true if at
  // least one rising edge occurred since the last call (multiple edges
  // within one call collapse to a single "true" -- IF is a level flag, not
  // a counter, so software polling it can't tell the difference anyway).
  //
  // Returns false unconditionally until the ROM has issued at least one
  // TP-rate-select command (see latchCommand) -- confirmed necessary by a
  // real regression: without this gate, TP started toggling from process
  // launch using a guessed default rate, which spuriously set IF bit 1
  // during the boot ROM's own "NEW0?:CHECK" prompt sequence (well before
  // BEEP or anything else ever issues a real TP command), skipping past a
  // prompt that real hardware correctly stops at. The datasheet doesn't
  // document TP's power-on-reset mux state, but real hardware clearly
  // doesn't have this problem despite the RTC's divider presumably having
  // been free-running for years off a backup battery -- so a raw,
  // ungated TP-to-IF-bit-1 mirror can't be the whole story; not letting
  // TP mean anything before it's actually configured is the simplest fix
  // that resolves the regression while keeping BEEP working (it always
  // configures TP=64Hz before ever polling).
  bool advanceRealTime(double elapsedSeconds);

  // Explicitly (re-)syncs this chip's notion of "now" to the host's
  // current wall-clock time -- the same effect BASIC's own TIME command
  // has (see commitShiftRegisterToTime()), without needing the CPU to
  // actually run any ROM code to get there. Already the default at
  // construction (timeOffset_ starts at 0, see its own comment), so
  // this mostly exists to make that intent explicit and give a caller
  // (see main.cpp) an obvious hook to call at startup, rather than
  // relying on nobody having reset timeOffset_ away from its default in
  // the meantime.
  void syncToHostClock() { timeOffset_ = std::chrono::seconds{0}; }

 private:
  // Per the datasheet's command table: Group 0 (C2=0) selects one of these
  // four register-control modes; Group 1 (C2=1, handled separately via
  // tpRateHz_) selects the TP output rate instead.
  enum class Mode { RegisterHold, RegisterShift, TimeSet, TimeRead };

  void latchCommand(bool c0, bool c1, bool c2);
  uint64_t liveTimeAsBcd40() const;
  void commitShiftRegisterToTime();

  bool prevStb_ = false;
  bool prevClk_ = false;
  bool dataIn_ = false;

  Mode mode_ = Mode::RegisterHold;
  int tpRateHz_ = 64;
  bool tpConfigured_ = false;  // see advanceRealTime()
  // 40 bits used: bits [3:0]=units-of-seconds ... bits [39:36]=month (see
  // liveTimeAsBcd40()). Shifts right on each CLK edge while mode_ is
  // RegisterShift or TimeSet (the two modes the datasheet lists as
  // "DATA OUT=[LSB]"); bit 0 is what falls out to DATA OUT (Fig. 1: "DATA
  // ... appears on DATA OUT terminal from LSB of Second"), and the new bit
  // from DATA IN enters at bit 39.
  uint64_t shiftRegister_ = 0;

  bool tpLevel_ = false;
  double tpPhaseSeconds_ = 0.0;

  // now() + timeOffset_ is this chip's notion of "now". Starts at 0 (i.e.
  // matches host wall-clock time) rather than an arbitrary epoch, so
  // BASIC's TIME$/DATE$ show something sensible even before any Time-Set
  // command -- like a real battery-backed clock that happens to already
  // read correctly. Time-Set commits a new value here (see
  // commitShiftRegisterToTime()); the chip has no year field at all (per
  // the datasheet's 40-bit layout), so the host's current year is always
  // used for that part.
  std::chrono::seconds timeOffset_{0};
};

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
  // tracks the physical key state directly. On a rising edge, also
  // latches IF bit 0x02 (kTpFlagBit -- confirmed shared with the RTC's TP
  // edge, see its own comment below) -- PB7 is configured as an LH5811
  // interrupt input, and this is the flag real hardware sets so the CPU's
  // MI handler notices. Confirmed by tracing backward from the literal
  // "BREAK IN" string in ROM1.BIN's message table to find what actually
  // triggers it: setting only this bit, or only requesting MI without it,
  // was each independently confirmed insufficient -- a running BASIC
  // program only actually breaks with both together.
  void setOnKeyLine(bool pressed) {
    if (pressed && !onKeyLine_) if_ |= kTpFlagBit;
    onKeyLine_ = pressed;
  }

  // OPC bit 6 (PC6) is the buzzer on/off control -- there's no separate
  // tone/frequency register on real hardware; BASIC's BEEP works by
  // having the ROM toggle this bit in a timed software loop, and the
  // resulting square wave *is* the tone. Emulating it is therefore just
  // exposing the bit's live level for the host audio callback to sample
  // directly (see main.cpp) -- std::atomic since that callback runs on
  // SDL's own audio thread, not the emulator's main thread that calls
  // write().
  bool buzzerOn() const { return buzzerOn_.load(std::memory_order_relaxed); }

  // Drives serial-transmit timing (see write()'s 0x06 case): call once per
  // emulated cycle batch, same as Bus::advanceCycles (which forwards to
  // this). Counts down txCyclesRemaining_ and sets the TD flag (IF bit 3)
  // when a transmission completes.
  void advanceCycles(int cycles);

  // Drives the RTC's TP output (see Upd1990ac and rtc_ below): call once
  // per rendered frame, same as Bus::advanceRealTime (which forwards to
  // this), with real elapsed wall-clock seconds. Latches IF bit 1 on each
  // TP rising edge -- see rtc_'s class comment for why BASIC's BEEP
  // actually depends on this.
  void advanceRealTime(double elapsedSeconds);

  // Forwards to the RTC -- see Upd1990ac::syncToHostClock().
  void syncRtcToHostClock() { rtc_.syncToHostClock(); }

 private:
  uint8_t dda_ = 0;
  uint8_t ddb_ = 0;
  uint8_t opa_ = 0;
  uint8_t opb_ = 0;
  uint8_t opc_ = 0;
  uint8_t f_ = 0;
  uint8_t g_ = 0;
  uint8_t msk_ = 0;
  // mutable: reading U (register 0x05) clears the RD flag here as a real
  // hardware side effect (see below) even though read() is logically const
  // from the CPU's point of view (a memory-mapped register read, not a
  // write instruction).
  mutable uint8_t if_ = 0;
  bool onKeyLine_ = false;
  std::atomic<bool> buzzerOn_{false};

  // LH5811 serial transmission (RS=0110, "send data" trigger register --
  // called L in the PC-2 Service Manual's I/O port controller block
  // diagram) and reception (RS=0101, U register). Confirmed real usage via
  // CE-150 (printer/cassette interface) ROM disassembly: it polls IF bit 3
  // (0x08) clear before writing a new byte to 0x06, and the byte written
  // there is never itself readable back (register is write-only/trigger,
  // per the Service Manual's register table) -- i.e. bit 3 is TD
  // (transmit done), set by hardware when a transmission finishes, and
  // implicitly clear while one is in progress. Confirmed BASIC's BEEP does
  // NOT use this path at all (it bit-bangs OPC directly and waits on IF
  // bit 1, which tracks something else entirely -- see docs/pc1500_hardware_reference.md).
  //
  // RD (receive done) bit position is *not* confirmed against any ROM
  // disassembly (no PC-1500 ROM code path exercising CLOAD-style serial
  // reception has been traced yet) -- bit 2 here is a best guess based on
  // typical IRQ/PB7/RD/TD bit ordering, not a verified hardware fact.
  static constexpr uint8_t kTdFlagBit = 0x08;
  static constexpr uint8_t kRdFlagBit = 0x04;  // unconfirmed -- see above

  // Transmission format is start bit + 8 data bits + 2 stop bits (11 bit
  // periods total, per the Service Manual), each bit period lasting
  // divisor() cycles of the CPU's own clock (the "basic clock" the divisor
  // list is stated relative to). The 3-bit divisor selector packed into F
  // bits 2-0 and this ascending-rate ordering are also not confirmed
  // against a real bit-encoding table (none survived OCR from either
  // manual) -- best guess from the Service Manual's listed rate order,
  // refinable later against real CE-150 ROM timing if it matters.
  uint8_t transmitClockDivisor() const {
    static constexpr int kDivisors[8] = {1, 2, 128, 256, 512, 1024, 2048, 4096};
    return kDivisors[f_ & 0x07];
  }

  uint8_t serialTxByte_ = 0;
  uint8_t serialRxByte_ = 0;
  int txCyclesRemaining_ = 0;

  // IF bit 1: latched from the RTC's TP rising edge -- see
  // advanceRealTime() and Upd1990ac's class comment. Unlike TD/RD (bits
  // 3/2), this one *is* ROM-confirmed (BASIC BEEP's repeat-gap wait polls
  // exactly this bit after commanding TP=64Hz). Also confirmed shared
  // with PB7 (see setOnKeyLine()) -- rather than a dedicated bit per
  // possible LH5811 interrupt-input pin, this looks like one shared
  // "external event" flag, with software expected to disambiguate the
  // actual cause afterward by checking each candidate pin's own live
  // level (e.g. onKeyLine_ for "was this ON/BREAK").
  static constexpr uint8_t kTpFlagBit = 0x02;
  Upd1990ac rtc_;
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
  //
  // The F1-F6 reserve-key area (4008H-40C4H) is the one documented
  // exception: no ROM code path ever initializes it either (confirmed via
  // instruction tracing -- the assignment-lookup loop at CEC6h just scans
  // past 40C4H into whatever garbage follows, since it never finds the
  // 00H "end of assignments" terminator the manual requires), yet real
  // hardware reads 0 there whenever no keys are assigned. That 189-byte
  // structure must already be a valid (all-zero, i.e. "nothing assigned
  // yet") 00H-terminated list before the ROM ever touches it -- there is
  // no real-hardware state equivalent to our synthetic all-0xFF default
  // for this specific range, so it's seeded to 0 explicitly here.
  explicit Bus(Keyboard& keyboard) : keyboard_(keyboard) {
    me0_.fill(0xFF);
    std::fill(me0_.begin() + 0x4008, me0_.begin() + 0x40C5, 0x00);
  }

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

  // CE-150/153/158-style plug-in ROM module at 0x8000-0xBFFF, selected by
  // the CPU's PV flip-flop (and, for modules whose ROM is bigger than the
  // 16K window, banked by PU) -- see PU/PV's own class comment in
  // lh5801.h. PV/PU only affect this region; everything else (SPU/RPU/
  // SPV/RPV's other historical uses, if any) is out of scope here.
  //
  // `data` is the module's raw ROM dump, `base` is where it appears in
  // the address space, `requirePv` is the PV level the CPU must have set
  // for the module to respond at all (CE-150 defaults to PV=0/low,
  // CE-158 to PV=1/high -- i.e. the two module *families* are
  // distinguished by which PV level they answer to), and `usePuBank`
  // splits `data` into two equal halves (PU=0 selects the first,
  // PU=1 the second) for modules whose real ROM exceeds the 16K window
  // (CE-158) -- false for modules that fit in one 16K bank regardless of
  // PU (CE-150).
  struct RomModule {
    std::vector<uint8_t> data;
    uint16_t base = 0xA000;
    bool requirePv = false;
    bool usePuBank = false;

    bool tryRead(uint16_t addr, bool pv, bool pu, uint8_t& out) const {
      if (data.empty() || pv != requirePv) return false;
      size_t bankSize = usePuBank ? data.size() / 2 : data.size();
      if (bankSize == 0 || addr < base || addr >= base + bankSize) return false;
      size_t bank = (usePuBank && pu) ? 1 : 0;
      size_t offset = bank * bankSize + (addr - base);
      if (offset >= data.size()) return false;
      out = data[offset];
      return true;
    }
  };
  void loadRomModule(const uint8_t* data, size_t size, uint16_t base, bool requirePv,
                      bool usePuBank) {
    module_.data.assign(data, data + size);
    module_.base = base;
    module_.requirePv = requirePv;
    module_.usePuBank = usePuBank;
  }
  void unloadRomModule() { module_ = RomModule{}; }
  bool romModuleLoaded() const { return !module_.data.empty(); }

  // Second, independent module slot -- e.g. testing a candidate PSoC-style
  // card concurrently with a real CE-158 dump, one per PV level, both
  // answering in 0x8000-0xBFFF at once. Same semantics as the primary slot
  // above, just a second instance so the two don't clobber each other.
  void loadRomModule2(const uint8_t* data, size_t size, uint16_t base, bool requirePv,
                       bool usePuBank) {
    module2_.data.assign(data, data + size);
    module2_.base = base;
    module2_.requirePv = requirePv;
    module2_.usePuBank = usePuBank;
  }
  void unloadRomModule2() { module2_ = RomModule{}; }
  bool romModule2Loaded() const { return !module2_.data.empty(); }

  // Forwarded from the CPU's own SPU/RPU/SPV/RPV opcode handlers (see
  // lh5801.cpp) -- Bus is the single source of truth for the *current*
  // pin level a module ROM read sees, since the CPU has no other reason
  // to know the bus exists on this side of the interface.
  void setPv(bool v) override { pv_ = v; }
  void setPu(bool v) override { pu_ = v; }

  // Emulated module RAM, two independent regions/windows -- see
  // docs/pc1500_hardware_reference.md's memory map. Both off (size 0) by
  // default, matching stock hardware with no module installed. Size is in
  // bytes, backed starting at the window's low address; the rest of the
  // window (if size is less than the window's full span) stays unmapped,
  // matching a physically smaller module not filling its whole socket.
  // Changing the size doesn't clear underlying bytes, so shrinking and
  // growing back preserves whatever was there.
  //
  // 4800H-6FFFH window (2K module RAM slot, up to 10K span): real 1982
  // hardware options were 4K or 8K here; 10K (the window's full physical
  // span) wasn't a real period-correct module but is easy to emulate and
  // physically possible with denser modern RAM.
  static constexpr size_t kExtRam4800WindowSize = 0x2800;  // 10K
  void setExtRam4800Size(size_t bytes) { extRam4800Size_ = bytes; }
  size_t extRam4800Size() const { return extRam4800Size_; }

  // 0000H-3FFFH window (option user memory slot, up to 16K span): not a
  // real 1982-era option at all (nothing plugged in there back then), but
  // physically possible now and worth emulating for headroom.
  static constexpr size_t kExtRam0000WindowSize = 0x4000;  // 16K
  void setExtRam0000Size(size_t bytes) { extRam0000Size_ = bytes; }
  size_t extRam0000Size() const { return extRam0000Size_; }

  // Wraps Keyboard::setKeyState. The actual release is deferred by
  // kMinimumHoldCycles (see applyRelease) rather than applied immediately,
  // so a host keypress briefer than one ROM timer-interrupt period can't
  // go all the way down and back up between two keyboard scans without
  // any scan ever seeing it.
  //
  // History, worth keeping in mind if key responsiveness ever looks wrong
  // again: this used to *also* force-clear the ROM's key-dispatch gate
  // (RAM flag at 7B0EH bit 0) the instant a non-cursor release left
  // nothing held, because that flag's own ~8-timer-period software
  // countdown didn't match observed real-hardware responsiveness (typing
  // several keys a second with no perceptible delay). That was
  // compensating for a real bug, not a genuine hardware quirk: the CPU
  // timer was modeled as a linear counter instead of the real 9-bit
  // *polynomial* one (see lh5801_timer_table.h; fixed 2026-08-02). With
  // the correct polynomial sequence, the ROM's own countdown clears the
  // gate on its own within a few thousand cycles -- the force-clear was
  // removed once this was confirmed, and confirmed *necessary* to remove:
  // it had been silently breaking BASWORD's own keyboard-hook timing
  // (see [[pc1500_keyword_table_mechanism]] in memory for the full story
  // -- `BASWORD +"name";"..."` only started actually registering new
  // keywords once this force-clear was gone).
  void setKeyState(Key key, bool pressed);

  // Debug-only: exposes cursor-key repeat-timing state directly, for
  // testing the double-press fix without the confound of normal ROM
  // execution also touching 7B0EH during a `run`.
  bool debugCursorKeyHeld() const { return cursorKeyHeld_; }
  int debugCursorRepeatCycles() const { return cursorRepeatCycles_; }
  bool debugCursorRepeatFired() const { return cursorRepeatFired_; }

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

  // Forwards to the I/O port controller's RTC -- see
  // IoPortController::advanceRealTime(). Call once per rendered frame with
  // real elapsed wall-clock seconds (main.cpp already computes this for
  // its audio sample accumulator).
  void advanceRealTime(double elapsedSeconds) { io_.advanceRealTime(elapsedSeconds); }

  // Session state save/load -- deliberately narrow scope: RAM contents
  // (me0_[0x0000,0x8000) only -- covers both extension-RAM windows, the
  // built-in 2K RAM, and the LCD buffer/system RAM/their mirrors, all
  // backed by the same array), extension-RAM window sizes, and both ROM
  // module slots (raw bytes + base/requirePv/usePuBank -- module ROM isn't
  // stored in me0_ at all, see RomModule::tryRead). Deliberately excludes
  // 0x8000H-0xFFFFH (module-ROM range isn't backed by me0_; the base
  // system ROM at 0xC000H-0xFFFFH is always reloaded fresh from the
  // command-line/conf-file ROM path, not duplicated into the state file),
  // CPU registers (restore instead relies on a normal cpu.reset() cold
  // boot, matching how a real PC-1500 resumes after a power cycle -- RAM
  // is battery-backed but the CPU always resets), and IoPortController/RTC
  // state (left to the ROM's own boot-time reinitialization, same
  // reasoning). See src/hoststate/state_file.h for the file-level
  // magic/version header this is embedded in.
  void saveState(std::ostream& os) const;
  bool loadState(std::istream& is);

 private:
  // Applies a release that setKeyState deferred: updates Keyboard, and (for
  // non-cursor keys) the same 7B0EH-bit-0 clear setKeyState's comment
  // describes, now that the release is actually taking effect.
  void applyRelease(Key key);


  static bool isRom(uint16_t addr) { return addr >= 0xC000; }
  bool isUnmapped(uint16_t addr) const {
    bool in0000Window = addr <= 0x3FFF;
    bool in4800Window = addr >= 0x4800 && addr <= 0x6FFF;
    return (in0000Window && addr >= extRam0000Size_) ||       // beyond configured 0000H module RAM
           (in4800Window && (addr - 0x4800) >= extRam4800Size_) ||  // beyond configured 4800H module RAM
           (addr >= 0x8000 && addr <= 0xBFFF);                // CE-150/153/158 (not connected)
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
  size_t extRam4800Size_ = 0;
  size_t extRam0000Size_ = 0;
  RomModule module_;
  RomModule module2_;
  bool pv_ = false;  // matches CPU::reset()'s own pv_/pu_ default
  bool pu_ = false;

  // Cursor-key rollover state (see advanceCycles).
  bool cursorKeyHeld_ = false;
  Key heldCursorKey_ = Key::Left;
  int cursorRepeatCycles_ = 0;
  // Whether this hold has already fired its first repeat -- real keyboard
  // auto-repeat has two distinct timings (a longer initial delay, then a
  // faster ongoing rate once repeating has started); this distinguishes
  // which threshold (kCursorInitialDelayCycles vs kCursorRepeatCycles)
  // applies. Reset alongside cursorRepeatCycles_ on every fresh press.
  bool cursorRepeatFired_ = false;
  // ~1s at 1.3MHz: Paul's own hardware observation ("close to one second
  // held down to start repeating") -- confirmed distinct from the ongoing
  // repeat rate below, which an earlier tuning pass had wrongly applied
  // to the initial delay too (making the first repeat fire ~6x too soon).
  static constexpr int kCursorInitialDelayCycles = 1300000;
  // ~154ms at 1.3MHz: Paul's own "~5/sec" estimate, refined after
  // side-by-side comparison to hardware (initial 200ms guess ran ~30%
  // slower than the real repeat rate). This is the *ongoing* rate once
  // repeating has already started -- see kCursorInitialDelayCycles above
  // for the (much longer) delay before the first repeat.
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
