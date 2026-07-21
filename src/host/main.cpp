#include <array>
#include <cstdio>

#include "lh5801.h"

namespace {

// Placeholder flat-memory bus for exercising the CPU core before the real
// PC-1500 memory map (task: Implement PC-1500 memory/bus subsystem) exists.
class FlatMemoryBus : public lh5801::MemoryBus {
 public:
  uint8_t readME0(uint16_t addr) override { return me0_[addr]; }
  void writeME0(uint16_t addr, uint8_t value) override { me0_[addr] = value; }
  uint8_t readME1(uint16_t addr) override { return me1_[addr]; }
  void writeME1(uint16_t addr, uint8_t value) override { me1_[addr] = value; }

 private:
  std::array<uint8_t, 65536> me0_{};
  std::array<uint8_t, 65536> me1_{};
};

}  // namespace

int main() {
  FlatMemoryBus bus;
  lh5801::CPU cpu(bus);
  cpu.reset();
  std::printf("pc1500emu: CPU core scaffolded, no ROM/bus wired up yet\n");
  return 0;
}
