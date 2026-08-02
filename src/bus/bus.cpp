// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#include "bus.h"

namespace pc1500 {

namespace {
uint8_t toBcd(int v) { return static_cast<uint8_t>(((v / 10) << 4) | (v % 10)); }
}  // namespace

void Upd1990ac::setControlPins(bool dataIn, bool stb, bool clk, bool c0, bool c1, bool c2) {
  dataIn_ = dataIn;

  if (stb && !prevStb_) latchCommand(c0, c1, c2);
  prevStb_ = stb;

  if (clk && !prevClk_ && (mode_ == Mode::RegisterShift || mode_ == Mode::TimeSet)) {
    shiftRegister_ = (shiftRegister_ >> 1) |
                      (static_cast<uint64_t>(dataIn_ ? 1 : 0) << 39);
  }
  prevClk_ = clk;
}

void Upd1990ac::latchCommand(bool c0, bool c1, bool c2) {
  int sel = (c1 ? 2 : 0) | (c0 ? 1 : 0);
  if (!c2) {
    Mode oldMode = mode_;
    switch (sel) {
      case 0: mode_ = Mode::RegisterHold; break;
      case 1: mode_ = Mode::RegisterShift; break;
      case 2: mode_ = Mode::TimeSet; break;
      case 3:
        mode_ = Mode::TimeRead;
        // "PS" snapshot per the datasheet's block diagram: Time Read
        // copies the live running clock into the shift register: it's
        // Register Shift (or continuing to sit in Time Read, which the
        // datasheet doesn't distinguish for this purpose) that then
        // actually walks it out over DATA OUT via CLK pulses.
        shiftRegister_ = liveTimeAsBcd40();
        break;
      default: break;
    }
    // Time Set & Counter Hold pauses the live clock while new data is
    // shifted in; committing on exit (rather than bit-by-bit as each CLK
    // arrives) is externally indistinguishable to anything that only reads
    // the result afterward, and far simpler.
    if (oldMode == Mode::TimeSet && mode_ != Mode::TimeSet) commitShiftRegisterToTime();
  } else {
    switch (sel) {
      case 0: tpRateHz_ = 64; tpConfigured_ = true; break;
      case 1: tpRateHz_ = 256; tpConfigured_ = true; break;
      case 2: tpRateHz_ = 2048; tpConfigured_ = true; break;
      default: break;  // TEST MODE -- not modeled, treated as a no-op
    }
  }
}

bool Upd1990ac::dataOut() const {
  if (mode_ == Mode::RegisterShift || mode_ == Mode::TimeSet) return (shiftRegister_ & 1) != 0;
  // Register Hold / Time Read: DATA OUT is a fixed status waveform (1 Hz /
  // 0.5 Hz respectively per the datasheet's command table), not register
  // content -- computed on demand rather than tracked as separate phase
  // state, since nothing in the real protocol depends on catching a
  // specific edge of it.
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
  if (mode_ == Mode::TimeRead) return ((ms / 1000) % 2) != 0;  // 0.5 Hz
  return ((ms / 500) % 2) != 0;                                // 1 Hz
}

bool Upd1990ac::advanceRealTime(double elapsedSeconds) {
  if (!tpConfigured_) return false;
  double halfPeriod = 0.5 / tpRateHz_;
  tpPhaseSeconds_ += elapsedSeconds;
  bool risingEdge = false;
  while (tpPhaseSeconds_ >= halfPeriod) {
    tpPhaseSeconds_ -= halfPeriod;
    tpLevel_ = !tpLevel_;
    if (tpLevel_) risingEdge = true;
  }
  return risingEdge;
}

uint64_t Upd1990ac::liveTimeAsBcd40() const {
  auto now = std::chrono::system_clock::now() + timeOffset_;
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tmv{};
  localtime_r(&t, &tmv);

  uint8_t month = static_cast<uint8_t>(tmv.tm_mon + 1);  // 1-12, binary/hex -- not BCD
  uint8_t dow = static_cast<uint8_t>(tmv.tm_wday);        // 0-6, Sunday=0
  uint8_t day = toBcd(tmv.tm_mday);
  uint8_t hour = toBcd(tmv.tm_hour);
  uint8_t minute = toBcd(tmv.tm_min);
  uint8_t second = toBcd(tmv.tm_sec);

  uint64_t reg = 0;
  reg |= static_cast<uint64_t>(second & 0x0F) << 0;
  reg |= static_cast<uint64_t>((second >> 4) & 0x0F) << 4;
  reg |= static_cast<uint64_t>(minute & 0x0F) << 8;
  reg |= static_cast<uint64_t>((minute >> 4) & 0x0F) << 12;
  reg |= static_cast<uint64_t>(hour & 0x0F) << 16;
  reg |= static_cast<uint64_t>((hour >> 4) & 0x0F) << 20;
  reg |= static_cast<uint64_t>(day & 0x0F) << 24;
  reg |= static_cast<uint64_t>((day >> 4) & 0x0F) << 28;
  reg |= static_cast<uint64_t>(dow & 0x0F) << 32;
  reg |= static_cast<uint64_t>(month & 0x0F) << 36;
  return reg;
}

void Upd1990ac::commitShiftRegisterToTime() {
  auto nibble = [this](int shift) { return static_cast<int>((shiftRegister_ >> shift) & 0x0F); };
  int second = nibble(4) * 10 + nibble(0);
  int minute = nibble(12) * 10 + nibble(8);
  int hour = nibble(20) * 10 + nibble(16);
  int day = nibble(28) * 10 + nibble(24);
  int month = nibble(36);  // 1-12, binary/hex

  auto now = std::chrono::system_clock::now();
  std::time_t nowT = std::chrono::system_clock::to_time_t(now);
  std::tm tmv{};
  localtime_r(&nowT, &tmv);  // seeds tm_year -- the chip has no year field at all
  tmv.tm_mon = month - 1;
  tmv.tm_mday = day;
  tmv.tm_hour = hour;
  tmv.tm_min = minute;
  tmv.tm_sec = second;
  std::time_t newT = std::mktime(&tmv);
  auto newTimePoint = std::chrono::system_clock::from_time_t(newT);
  timeOffset_ = std::chrono::duration_cast<std::chrono::seconds>(newTimePoint - now);
}

uint8_t IoPortController::read(uint8_t reg) const {
  switch (reg & 0x0F) {
    case 0x05:
      if_ &= static_cast<uint8_t>(~kRdFlagBit);
      return serialRxByte_;
    case 0x07: return f_;
    case 0x08: return opc_;
    case 0x09: return g_;
    case 0x0A: return msk_;
    case 0x0B: return if_;
    case 0x0C: return dda_;
    case 0x0D: return ddb_;
    case 0x0E: return opa_;
    case 0x0F: {
      // Bits 5/6 are hardwired to the RTC's TP/DATA OUT pins (see rtc_ and
      // docs/pc1500_hardware_reference.md), read unconditionally like PB7
      // regardless of DDB -- these aren't general-purpose I/O on a stock
      // PC-1500.
      uint8_t v = static_cast<uint8_t>(opb_ & 0x1F);
      v = static_cast<uint8_t>(v | (rtc_.tp() ? 0x20 : 0x00));
      v = static_cast<uint8_t>(v | (rtc_.dataOut() ? 0x40 : 0x00));
      v = static_cast<uint8_t>(v | (onKeyLine_ ? 0x80 : 0x00));
      return v;
    }
    case 0x06: return 0xFF;  // write-only trigger register, per Service Manual register table
    default: return 0xFF;  // divider reset (0100): not modeled
  }
}

void IoPortController::write(uint8_t reg, uint8_t value) {
  switch (reg & 0x0F) {
    case 0x06:
      serialTxByte_ = value;
      if_ &= static_cast<uint8_t>(~kTdFlagBit);
      txCyclesRemaining_ = 11 * transmitClockDivisor();
      break;
    case 0x07: f_ = value; break;
    case 0x08:
      opc_ = value;
      buzzerOn_.store((value & 0x40) != 0, std::memory_order_relaxed);
      // PC0-PC5 -- see rtc_'s class comment for the confirmed pin mapping
      // (PC0=DATA IN, PC1=STB, PC2=CLK, PC3-5=C0-C2).
      rtc_.setControlPins((value & 0x01) != 0, (value & 0x02) != 0, (value & 0x04) != 0,
                           (value & 0x08) != 0, (value & 0x10) != 0, (value & 0x20) != 0);
      break;
    case 0x09: g_ = value; break;
    case 0x0A: msk_ = value; break;
    case 0x0B: if_ = value; break;
    case 0x0C: dda_ = value; break;
    case 0x0D: ddb_ = value; break;
    case 0x0E: opa_ = value; break;
    case 0x0F: opb_ = value; break;
    case 0x05: break;  // U is read-only ("write: n/a" per Service Manual register table)
    default: break;  // divider reset (0100): not modeled
  }
}

void IoPortController::advanceCycles(int cycles) {
  if (txCyclesRemaining_ <= 0) return;
  txCyclesRemaining_ -= cycles;
  if (txCyclesRemaining_ <= 0) {
    txCyclesRemaining_ = 0;
    if_ |= kTdFlagBit;
  }
}

void IoPortController::advanceRealTime(double elapsedSeconds) {
  if (rtc_.advanceRealTime(elapsedSeconds)) if_ |= kTpFlagBit;
}

uint8_t IoPortController::opaOutput() const {
  // Bits set to output (DDA=1) drive opa_'s value; bits left as input
  // (DDA=0) read back as 1 (undriven/pulled-up). Real firmware (and our
  // own keyscan probe, docs/pc1500_keyscan_probe.md) always sets DDA=FFH
  // before using PA as keyboard strobe outputs.
  return static_cast<uint8_t>((opa_ & dda_) | static_cast<uint8_t>(~dda_));
}

uint8_t Bus::readME0(uint16_t addr) {
  if (addr >= 0x8000 && addr <= 0xBFFF) {
    uint8_t v;
    if (module_.tryRead(addr, pv_, pu_, v)) return v;
    if (module2_.tryRead(addr, pv_, pu_, v)) return v;
    return 0xFF;  // empty socket, or a module present but not selected by the current PV level
  }
  if (isUnmapped(addr)) return 0xFF;
  return me0_[effectiveAddr(addr)];
}

void Bus::writeME0(uint16_t addr, uint8_t value) {
  if (isUnmapped(addr) || isRom(addr)) return;
  me0_[effectiveAddr(addr)] = value;
}

namespace {
// CS0/CS1/CS2 are tied to AD12/AD13/(fixed) -- AD14/AD15 aren't part of the
// decode. Confirmed on real hardware: F00AH/F00BH and B00AH/B00BH read back
// identical, live values (F000H=1111..., B000H=1011... -- they differ only
// in bit 14). So the controller is selected whenever bits 12-13 are both
// set, regardless of bits 14-15 (or bits 4-11, which are likewise unused --
// only RS0-RS3, bits 0-3, select the register).
bool IoControllerSelected(uint16_t addr) { return (addr & 0x3000) == 0x3000; }
}  // namespace

uint8_t Bus::readME1(uint16_t addr) {
  if (IoControllerSelected(addr)) return io_.read(static_cast<uint8_t>(addr & 0x0F));
  return 0xFF;
}

void Bus::writeME1(uint16_t addr, uint8_t value) {
  if (IoControllerSelected(addr)) io_.write(static_cast<uint8_t>(addr & 0x0F), value);
}

uint8_t Bus::readInputPort() { return keyboard_.scan(io_.opaOutput()); }

namespace {
bool isCursorKey(Key key) {
  return key == Key::Left || key == Key::Right || key == Key::Up || key == Key::Down ||
         key == Key::UpDownRocker;
}
}  // namespace

void Bus::setKeyState(Key key, bool pressed) {
  if (pressed) {
    // A fresh press cancels any release we were about to apply for this
    // same key (host OS key-repeat, or a very fast re-tap) -- it's back
    // down, so there's nothing left to release.
    for (auto it = pendingReleases_.begin(); it != pendingReleases_.end(); ++it) {
      if (it->key == key) {
        pendingReleases_.erase(it);
        break;
      }
    }
    keyboard_.setKeyState(key, true);
    if (isCursorKey(key)) {
      // Only reset the repeat timer on a genuinely new press -- host OS
      // key-repeat re-sends "pressed" for a key that's already held, at a
      // much faster rate than our ~200ms window, which would otherwise
      // keep resetting the counter before it ever fires again.
      if (!cursorKeyHeld_ || heldCursorKey_ != key) {
        cursorRepeatCycles_ = 0;
      }
      cursorKeyHeld_ = true;
      heldCursorKey_ = key;
    }
    return;
  }

  // Defer the actual release: the ROM only polls the keyboard once per
  // timer-interrupt period (~25ms). A host keypress briefer than that
  // (or one whose press and release both land in the same emulated cycle
  // batch) could otherwise go all the way down and back up without any
  // ROM scan ever seeing it. Holding it down a little longer than the
  // host actually did guarantees at least one scan period sees it.
  pendingReleases_.push_back({key, kMinimumHoldCycles});
}

void Bus::applyRelease(Key key) {
  keyboard_.setKeyState(key, false);
  if (isCursorKey(key)) {
    if (key == heldCursorKey_) cursorKeyHeld_ = false;
  }
  // No forced 7B0EH-bit-0 clear here for ordinary keys anymore -- see
  // setKeyState's comment. That was compensating for the CPU timer being
  // modeled as a linear counter instead of the real 9-bit polynomial one
  // (fixed 2026-08-02, lh5801_timer_table.h); with the real polynomial
  // sequence, the ROM's own countdown clears the gate within a few
  // thousand cycles on its own (confirmed by direct trace, well under one
  // ~32700-cycle timer period), not the ~200ms the old linear model
  // implied. The forced clear was actively wrong, not just redundant: it
  // broke BASWORD's own keyboard-hook timing (confirmed -- disabling it
  // is what made `BASWORD +"name";"..."` start actually registering
  // keywords, see [[pc1500_keyword_table_mechanism]]).
}

void Bus::advanceCycles(int cycles) {
  io_.advanceCycles(cycles);
  if (cursorKeyHeld_) {
    cursorRepeatCycles_ += cycles;
    if (cursorRepeatCycles_ >= kCursorRepeatCycles) {
      cursorRepeatCycles_ = 0;
      writeME0(0x7B0E, static_cast<uint8_t>(readME0(0x7B0E) & 0xFE));
    }
  }
  for (auto it = pendingReleases_.begin(); it != pendingReleases_.end();) {
    it->cyclesRemaining -= cycles;
    if (it->cyclesRemaining <= 0) {
      applyRelease(it->key);
      it = pendingReleases_.erase(it);
    } else {
      ++it;
    }
  }
}

void Bus::loadME0(uint16_t addr, const uint8_t* data, size_t size) {
  for (size_t i = 0; i < size; i++) {
    uint32_t target = static_cast<uint32_t>(addr) + i;
    if (target > 0xFFFF) break;
    me0_[static_cast<uint16_t>(target)] = data[i];
  }
}

}  // namespace pc1500
