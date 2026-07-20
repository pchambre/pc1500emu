#include "bus.h"

namespace pc1500 {

uint8_t Bus::read(uint16_t) { return 0xFF; }

void Bus::write(uint16_t, uint8_t) {}

}  // namespace pc1500
