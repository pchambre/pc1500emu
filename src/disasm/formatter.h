// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "analyzer.h"
#include "known_symbols.h"

// Renders an AnalysisResult (classified bytes + recognized keyword/vector
// tables) to sdas-syntax text, directly reassemblable by sdcc-pc1500's
// sdaslh5801. See opcode_table.h for the instruction decoder this reuses.
namespace pc1500::disasm {

// Which assembler syntax formatListing renders. Sdas (the default) is this
// project's own house dialect -- lowercase mnemonics/registers, "0x"-prefixed
// hex, ".area CODE (ABS)"/".org"/".db"/".dw"/".ascii" -- directly
// reassemblable by sdcc-pc1500's sdaslh5801. Tasm renders the same decoded
// instructions in the Telemark Assembler (tasm5801.tab) dialect real
// hand-written PC-1500 sources often use instead: uppercase mnemonics/
// registers, "$"-prefixed hex, ".ORG" with no ".area" wrapper (tasm has no
// segment/relocation concept), ".DB"/".DW"/".TEXT" -- confirmed against a
// real hand-written tasm5801 source (see tasm_convert.h's own comment for
// the cross-check this was based on). Not reassemblable by sdaslh5801 --
// this is for producing output a TASM user can read/maintain, not for
// round-tripping through this project's own toolchain.
enum class AsmDialect { Sdas, Tasm };

struct FormatOptions {
  // Adds a trailing "; 0xNNNN: XX XX ..." comment to every code/data line,
  // showing the source address and raw bytes -- for human review, not
  // needed for reassembly.
  bool annotate = false;

  // A --symbols-file's parsed contents (empty if none was given) --
  // annotated the same way as known_symbols.cpp's built-in table (always
  // on, "; NAME -- comment"), and checked ahead of it so a user entry can
  // override a built-in one at the same address. See
  // known_symbols.h's loadUserSymbolsFile/lookupSymbol.
  std::vector<UserSymbol> userSymbols;

  // Which syntax to render -- see AsmDialect above. Default Sdas matches
  // this function's long-standing behavior.
  AsmDialect dialect = AsmDialect::Sdas;
};

std::string formatListing(const std::vector<uint8_t>& image, const AnalysisResult& result,
                           const FormatOptions& options = {});

}  // namespace pc1500::disasm
