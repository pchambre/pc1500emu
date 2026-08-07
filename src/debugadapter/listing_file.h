// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
//
// Parses an sdaslh5801 `-l` listing (.lst) file into an address<->source-
// line map, so the debug adapter can translate VS Code's source-line
// breakpoints into the addresses the emulator's `setbreakpoints` command
// understands, and translate a stopped `P` register back into a source
// line for the editor to highlight.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pc1500::dap {

struct ListingLine {
  uint16_t address = 0;
  int sourceLine = 0;  // 1-based
  std::string sourceText;
};

class ListingFile {
 public:
  // Parses `path`. Returns false and sets *error on failure (missing
  // file, or a file with zero recognizable listing lines).
  bool load(const std::string& path, std::string* error);

  // First address emitted for 1-based source line `sourceLine`, or
  // nullopt if that line emitted no bytes (blank line, comment,
  // zero-width directive) and so can't host a breakpoint.
  std::optional<uint16_t> addressForLine(int sourceLine) const;

  // Source line covering `address` exactly (the line whose listing entry
  // starts at that address), or nullopt if none does -- e.g. an address
  // in the middle of a multi-byte instruction, which sdaslh5801's listing
  // never emits as its own entry.
  std::optional<int> lineForAddress(uint16_t address) const;

  const std::vector<ListingLine>& lines() const { return lines_; }

 private:
  std::vector<ListingLine> lines_;
};

}  // namespace pc1500::dap
