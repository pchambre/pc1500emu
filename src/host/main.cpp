#include <cstdio>

#include "bus.h"
#include "keyboard.h"
#include "lh5801.h"

int main() {
  pc1500::Keyboard keyboard;
  pc1500::Bus bus(keyboard);
  lh5801::CPU cpu(bus);
  cpu.reset();
  std::printf(
      "pc1500emu: CPU + memory map wired up, no ROM loaded (P=%04X) -- "
      "keyboard/LCD not implemented yet\n",
      cpu.p());
  return 0;
}
