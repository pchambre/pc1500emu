// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#pragma once

#include <cstdint>

// A curated table of confirmed PC-1500 memory-map addresses and ROM entry
// points, for the disassembler's formatter to annotate output with --
// documentation only, not a rename (labels stay L<hex>: for guaranteed
// valid/unique sdas identifiers; see formatter.cpp). Every entry here was
// established elsewhere in this project (bus.cpp/bus.h comments,
// docs/pc1500_hardware_reference.md, src/basic/text_loader.h, and this
// session's own SML-regression and keyword-table investigations) --
// nothing new is asserted here, this is just those facts made available
// to the disassembler.
namespace pc1500::disasm {

struct KnownSymbol {
  uint16_t addr;
  // ME0 and ME1 are separate address spaces that happen to overlap
  // numerically (e.g. F00FH is an ordinary ME0 byte, but the I/O
  // controller's OPB register in ME1) -- me1=true entries only match a
  // lookup for a #(...)-style (ME1) operand, never a plain (...) one.
  bool me1;
  const char* name;
  const char* comment;
};

// Returns the symbol whose address and memory space exactly match, or
// nullptr.
const KnownSymbol* findKnownSymbol(uint16_t addr, bool me1);

}  // namespace pc1500::disasm
