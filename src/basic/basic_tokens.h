// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>

namespace pc1500::basic {

// PC-1500 BASIC keyword <-> internal-code table, transcribed from the
// *PC-1500 Technical Reference Manual* section 5-2 "Internal code chart"
// (p.115). Every BASIC command tokenizes to a fixed 2-byte code; the high
// byte is always one of F0H, F1H, E6H, E7H, E8H (all >= 0x80), which is
// what lets a detokenizer tell a token byte from a literal ASCII content
// byte (< 0x80) with no other context.
struct TokenEntry {
  uint16_t code;
  const char* keyword;
};

extern const TokenEntry kTokenTable[];
extern const size_t kTokenTableSize;

// Reverse (code -> keyword) lookup for the detokenizer. Returns nullptr if
// `code` isn't a recognized token.
const char* keywordForCode(uint16_t code);

}  // namespace pc1500::basic
