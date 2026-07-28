// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#pragma once

#include <cstdint>
#include <functional>

namespace pc1500 {

// PC-1500's dot-matrix LCD: 156 columns x 7 rows, driven by four column-
// driver chips, each covering a 39-column quarter of the width: chip1 =
// columns 0-38, chip2 = 39-77, chip3 = 78-116, chip4 = 117-155. See
// docs/pc1500_hardware_reference.md "LCD" section for the full derivation
// (confirmed via a systematic real-hardware POKE/observe walkthrough,
// isolating one bit at a time).
//
// The addressing is NOT "one byte per column" -- it's confirmed to be:
//   - chip1 (cols 0-38) and chip3 (cols 78-116) share 7600H-764DH, split
//     by nibble: chip1 = low nibble, chip3 = high nibble.
//   - chip2 (cols 39-77) and chip4 (cols 117-155) share 7700H-774DH the
//     same way: chip2 = low nibble, chip4 = high nibble.
//     (Matches the manual's 4-2-3 chip-select schematic: V2 selects
//     display chips 1&3 at 7600H-76FFH, V3 selects chips 2&4 at
//     7700H-77FFH.)
//   - Within either range, each column needs TWO consecutive byte
//     offsets, not one: the even offset holds rows 0-3 (in that nibble's
//     low 4 bits), the odd offset holds rows 4-6 (in that nibble's low 3
//     bits, top bit of the nibble unused) -- so column N within a chip's
//     39-column span lives at base+2N (rows 0-3) and base+2N+1 (rows
//     4-6). 39 columns x 2 bytes = 78 bytes, exactly filling each range's
//     first 78 bytes (7600H-764DH, 7700H-774DH), matching the manual's
//     own worked "display reverse" example (see lh5801_test.cpp), which
//     inverts precisely those two 78-byte spans.
//
// Unlike a typical memory-mapped peripheral, nothing writes to the LCD
// through a dedicated register -- the display buffer is plain ME0 RAM,
// and this class only interprets that RAM's content.
class Lcd {
 public:
  static constexpr int kColumns = 156;
  static constexpr int kRows = 7;
  static constexpr int kColumnsPerChip = 39;
  static constexpr uint16_t kLeftHalfBase = 0x7600;   // chip1 (low nibble) / chip3 (high nibble)
  static constexpr uint16_t kRightHalfBase = 0x7700;  // chip2 (low nibble) / chip4 (high nibble)

  // The given column's (0-155) assembled 7-dot byte (bit0=top row ...
  // bit6=bottom row), or 0 (all dots off) if the display is off (SDP/RDP
  // flip-flop reset -- see lh5801::CPU::disp()). `readByte` reads one
  // byte from ME0, typically Bus::readME0.
  uint8_t columnBits(int column, bool displayOn,
                      const std::function<uint8_t(uint16_t)>& readByte) const;

  // Convenience single-dot accessor built on columnBits().
  bool dot(int column, int row, bool displayOn,
           const std::function<uint8_t(uint16_t)>& readByte) const;
};

}  // namespace pc1500
