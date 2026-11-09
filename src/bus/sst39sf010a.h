// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
//
// Microchip SST39SF010A, the 128K flash on the CE-163F (banks 8-15) --
// datasheet DS20005022C. Only what software can see is modelled:
//  - reads, and the software-ID mode (0000H = BFH maker, 0001H = B5H part);
//  - the unlock sequence 5555H/AAH, 2AAAH/55H, then a command at 5555H:
//    A0H byte program, 80H erase (a second unlock, then 30H at an address
//    for its 4K sector, or 10H at 5555H for the chip), 90H software ID,
//    F0H exit;
//  - F0H on its own also leaves software-ID mode.
// Command addresses are A14-A0 only (A16-A15 don't care). Programming can
// only clear bits, as on the chip. Program and erase finish at once: a
// Data# poll or toggle-bit loop sees the final data on its first read.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace pc1500 {

class Sst39sf010a {
 public:
  static constexpr size_t kSize = 0x20000;
  static constexpr uint8_t kMakerId = 0xBF;
  static constexpr uint8_t kDeviceId = 0xB5;

  Sst39sf010a() { mem_.fill(0xFF); }

  uint8_t read(uint32_t addr) const {
    addr &= kSize - 1;
    if (idMode_) return (addr & 1) ? kDeviceId : kMakerId;
    return mem_[addr];
  }

  void write(uint32_t addr, uint8_t value) {
    addr &= kSize - 1;
    uint32_t cmd = addr & 0x7FFF;
    if (state_ == State::Program) {
      mem_[addr] &= value;
      state_ = State::Idle;
      return;
    }
    if (state_ == State::EraseCommand) {
      if (value == 0x30) {
        uint32_t sector = addr & ~static_cast<uint32_t>(0xFFF);
        for (uint32_t i = 0; i < 0x1000; ++i) mem_[sector + i] = 0xFF;
      } else if (value == 0x10 && cmd == 0x5555) {
        mem_.fill(0xFF);
      }
      state_ = State::Idle;
      return;
    }
    bool first = state_ == State::Idle || state_ == State::EraseSetup;
    bool second = state_ == State::Unlock1 || state_ == State::EraseUnlock1;
    if (first && cmd == 0x5555 && value == 0xAA) {
      state_ = state_ == State::Idle ? State::Unlock1 : State::EraseUnlock1;
      return;
    }
    if (second && cmd == 0x2AAA && value == 0x55) {
      state_ = state_ == State::Unlock1 ? State::Unlock2 : State::EraseCommand;
      return;
    }
    if (state_ == State::Unlock2 && cmd == 0x5555) {
      state_ = State::Idle;
      switch (value) {
        case 0xA0: state_ = State::Program; break;
        case 0x80: state_ = State::EraseSetup; break;
        case 0x90: idMode_ = true; break;
        case 0xF0: idMode_ = false; break;
        default: break;
      }
      return;
    }
    if (value == 0xF0) idMode_ = false;
    state_ = State::Idle;
  }

  bool idMode() const { return idMode_; }
  std::array<uint8_t, kSize>& memory() { return mem_; }
  const std::array<uint8_t, kSize>& memory() const { return mem_; }
  // Back to reading the array (a power cycle).
  void resetCommandState() {
    state_ = State::Idle;
    idMode_ = false;
  }

 private:
  enum class State { Idle, Unlock1, Unlock2, Program, EraseSetup, EraseUnlock1, EraseCommand };
  std::array<uint8_t, kSize> mem_{};
  State state_ = State::Idle;
  bool idMode_ = false;
};

}  // namespace pc1500
