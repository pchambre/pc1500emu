#pragma once

#include <cstdint>

namespace lh5801 {

// Placeholder CPU core. Register set, flags, and instruction decode are
// pending ISA research (docs/lh5801_isa.md) and will replace this stub.
class CPU {
 public:
  void reset();
  int step();  // returns cycle count consumed
};

}  // namespace lh5801
