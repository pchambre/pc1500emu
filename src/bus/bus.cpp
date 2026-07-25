#include "bus.h"

namespace pc1500 {

uint8_t IoPortController::read(uint8_t reg) const {
  switch (reg & 0x0F) {
    case 0x07: return f_;
    case 0x08: return opc_;
    case 0x09: return g_;
    case 0x0A: return msk_;
    case 0x0B: return if_;
    case 0x0C: return dda_;
    case 0x0D: return ddb_;
    case 0x0E: return opa_;
    case 0x0F: return static_cast<uint8_t>((opb_ & 0x7F) | (onKeyLine_ ? 0x80 : 0x00));
    default: return 0xFF;  // divider (0100)/serial (0101/0110): not modeled yet
  }
}

void IoPortController::write(uint8_t reg, uint8_t value) {
  switch (reg & 0x0F) {
    case 0x07: f_ = value; break;
    case 0x08: opc_ = value; break;
    case 0x09: g_ = value; break;
    case 0x0A: msk_ = value; break;
    case 0x0B: if_ = value; break;
    case 0x0C: dda_ = value; break;
    case 0x0D: ddb_ = value; break;
    case 0x0E: opa_ = value; break;
    case 0x0F: opb_ = value; break;
    default: break;  // divider reset/serial transmit: not modeled yet
  }
}

uint8_t IoPortController::opaOutput() const {
  // Bits set to output (DDA=1) drive opa_'s value; bits left as input
  // (DDA=0) read back as 1 (undriven/pulled-up). Real firmware (and our
  // own keyscan probe, docs/pc1500_keyscan_probe.md) always sets DDA=FFH
  // before using PA as keyboard strobe outputs.
  return static_cast<uint8_t>((opa_ & dda_) | static_cast<uint8_t>(~dda_));
}

uint8_t Bus::readME0(uint16_t addr) {
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
  } else if (!keyboard_.anyPressed()) {
    writeME0(0x7B0E, static_cast<uint8_t>(readME0(0x7B0E) & 0xFE));
  }
}

void Bus::advanceCycles(int cycles) {
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
