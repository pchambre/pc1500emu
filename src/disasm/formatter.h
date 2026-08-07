// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "analyzer.h"

// Renders an AnalysisResult (classified bytes + recognized keyword/vector
// tables) to sdas-syntax text, directly reassemblable by sdcc-pc1500's
// sdaslh5801. See opcode_table.h for the instruction decoder this reuses.
namespace pc1500::disasm {

struct FormatOptions {
  // Adds a trailing "; 0xNNNN: XX XX ..." comment to every code/data line,
  // showing the source address and raw bytes -- for human review, not
  // needed for reassembly.
  bool annotate = false;
};

std::string formatListing(const std::vector<uint8_t>& image, const AnalysisResult& result,
                           const FormatOptions& options = {});

}  // namespace pc1500::disasm
