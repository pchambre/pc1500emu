// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#pragma once

#include <array>
#include <cstdint>

namespace pc1500 {

// PC-1500 keyboard: 8x8 matrix (PA0-7 columns / IN0-7 rows), confirmed by
// pressing every key on a real PC-1500 and reading back the matrix
// position -- see docs/pc1500_hardware_reference.md "Physical key matrix"
// table and docs/pc1500_keyscan_probe.md for how it was done. The ON key
// is NOT part of this matrix -- it's wired straight to the CPU's BFI pin
// instead (see lh5801::CPU::pressOnKey()).
enum class Key {
  Digit0, Digit1, Digit2, Digit3, Digit4, Digit5, Digit6, Digit7, Digit8, Digit9,
  A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
  Plus, Minus, Equals, Asterisk, Slash, LeftParen, RightParen, Colon, Period,
  Left, Right, Up, Down, UpDownRocker,
  Shift, Sml, Mode, Def, Off, Cl, Rcl, Ent, Space,
  F1, F2, F3, F4, F5, F6,
};

class Keyboard {
 public:
  // Physical row/column primitive (row=IN0-7, col=PA0-7).
  void setKeyState(int row, int col, bool pressed);

  // Convenience wrapper looking up `key`'s matrix position.
  void setKeyState(Key key, bool pressed);

  // Strobes the matrix: driveLines is the column-strobe byte (from the
  // I/O port controller's OPA output; active-low, confirmed against real
  // hardware via the keyscan probe). Returns the resulting row-read byte,
  // active-low the same way -- bit r is 0 if some pressed key shares row r
  // with a currently-strobed (bit low) column.
  uint8_t scan(uint8_t driveLines) const;

  // True if any matrix key is currently held, regardless of column strobe
  // state -- used by Bus to detect the "nothing at all is pressed" moment.
  bool anyPressed() const;

 private:
  static constexpr int kRows = 8;
  static constexpr int kCols = 8;
  std::array<std::array<bool, kCols>, kRows> keys_{};
};

}  // namespace pc1500
