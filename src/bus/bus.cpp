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
    case 0x0F: return opb_;
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
  return me0_[addr];
}

void Bus::writeME0(uint16_t addr, uint8_t value) {
  if (isUnmapped(addr) || isRom(addr)) return;
  me0_[addr] = value;
}

uint8_t Bus::readME1(uint16_t addr) {
  if (addr >= 0xF000 && addr <= 0xF00F) {
    return io_.read(static_cast<uint8_t>(addr & 0x0F));
  }
  return 0xFF;  // nothing else mapped in ME1 on a stock PC-1500
}

void Bus::writeME1(uint16_t addr, uint8_t value) {
  if (addr >= 0xF000 && addr <= 0xF00F) {
    io_.write(static_cast<uint8_t>(addr & 0x0F), value);
  }
}

uint8_t Bus::readInputPort() { return keyboard_.scan(io_.opaOutput()); }

void Bus::loadME0(uint16_t addr, const uint8_t* data, size_t size) {
  for (size_t i = 0; i < size; i++) {
    uint32_t target = static_cast<uint32_t>(addr) + i;
    if (target > 0xFFFF) break;
    me0_[static_cast<uint16_t>(target)] = data[i];
  }
}

}  // namespace pc1500
