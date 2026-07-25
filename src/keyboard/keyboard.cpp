#include "keyboard.h"

namespace pc1500 {

namespace {

// Row/column position for each named key, per the confirmed physical
// matrix (docs/pc1500_hardware_reference.md, rows IN0-IN7 x cols PA0-PA7).
constexpr Key kMatrix[8][8] = {
    {Key::Digit2, Key::Period, Key::Digit1, Key::RightParen, Key::Plus, Key::Equals, Key::Right, Key::Digit3},
    {Key::Digit5, Key::Minus, Key::Digit4, Key::L, Key::Asterisk, Key::Left, Key::Mode, Key::Digit6},
    {Key::Digit8, Key::Off, Key::Digit7, Key::O, Key::Slash, Key::P, Key::Cl, Key::Digit9},
    {Key::H, Key::S, Key::J, Key::K, Key::D, Key::Colon, Key::A, Key::G},
    {Key::Shift, Key::F1, Key::F5, Key::F6, Key::F2, Key::F3, Key::Def, Key::F4},
    {Key::Y, Key::W, Key::U, Key::I, Key::E, Key::R, Key::Q, Key::T},
    {Key::N, Key::X, Key::M, Key::LeftParen, Key::C, Key::V, Key::Z, Key::B},
    {Key::Up, Key::UpDownRocker, Key::Digit0, Key::Ent, Key::Rcl, Key::Space, Key::Sml, Key::Down},
};

}  // namespace

void Keyboard::setKeyState(int row, int col, bool pressed) {
  if (row < 0 || row >= kRows || col < 0 || col >= kCols) return;
  keys_[row][col] = pressed;
}

void Keyboard::setKeyState(Key key, bool pressed) {
  for (int row = 0; row < kRows; row++) {
    for (int col = 0; col < kCols; col++) {
      if (kMatrix[row][col] == key) {
        keys_[row][col] = pressed;
        return;
      }
    }
  }
}

bool Keyboard::anyPressed() const {
  for (const auto& row : keys_) {
    for (bool k : row) {
      if (k) return true;
    }
  }
  return false;
}

uint8_t Keyboard::scan(uint8_t driveLines) const {
  uint8_t result = 0xFF;
  for (int col = 0; col < kCols; col++) {
    bool colActive = ((driveLines >> col) & 1) == 0;
    if (!colActive) continue;
    for (int row = 0; row < kRows; row++) {
      if (keys_[row][col]) {
        result &= static_cast<uint8_t>(~(1 << row));
      }
    }
  }
  return result;
}

}  // namespace pc1500
