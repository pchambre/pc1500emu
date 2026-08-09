// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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

// A user-supplied symbol, loaded from a --symbols-file at CLI startup --
// see loadUserSymbolsFile. Unlike KnownSymbol (a constexpr table pointing
// at string literals), this owns its strings: name/comment come from a
// file read at runtime, so they need real storage, not just a pointer.
struct UserSymbol {
  uint16_t addr = 0;
  bool me1 = false;  // v1 of the file format has no way to set this --
                      // every entry loaded from a file is me1=false.
  std::string name;
  std::string comment;
};

// Parses a simple line-based symbols file: blank lines and lines starting
// with '#' are ignored; every other line is `<addr> <name> <comment...>`
// (whitespace-separated address and name, then the rest of the line
// verbatim as the comment -- so the comment itself may contain spaces).
// `addr` accepts an optional "0x" prefix, parsed as hex either way
// (matching how disasm_main.cpp's own --base/--seed already parse
// addresses). A malformed line (bad hex, or fewer than two tokens) is
// skipped with a warning to stderr (1-based line number + the line's own
// text) rather than aborting the whole load -- one typo shouldn't cost
// every other entry in the file.
//
// Returns false (with *error set) only if `path` itself can't be opened;
// a file that opens successfully always returns true, even if every line
// in it was malformed and skipped.
bool loadUserSymbolsFile(const std::string& path, std::vector<UserSymbol>* out, std::string* error);

// A resolved symbol's name/comment, independent of which table (user or
// built-in) it actually came from -- see lookupSymbol.
struct ResolvedSymbol {
  std::string name;
  std::string comment;
};

// Checks `userSymbols` first (so a --symbols-file entry can override a
// built-in KnownSymbol at the same address+space, e.g. to correct or
// extend an existing one without touching this file), then falls back to
// findKnownSymbol. Returns std::nullopt if neither has a match.
std::optional<ResolvedSymbol> lookupSymbol(uint16_t addr, bool me1,
                                            const std::vector<UserSymbol>& userSymbols);

}  // namespace pc1500::disasm
