#include "keyboard.h"

namespace pc1500 {

void Keyboard::setKeyState(int, int, bool) {}

uint8_t Keyboard::scan(uint8_t) const { return 0xFF; }

}  // namespace pc1500
