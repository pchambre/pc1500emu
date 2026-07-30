// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pc1500::basic {

// Converts a tokenized BASIC program (the on-device byte format: repeated
// [2-byte line#][1-byte size][size bytes: content..., last byte 0x0DH],
// terminated by a trailing 0xFFH -- PC-1500 Technical Reference Manual
// section 5-3-5 "Structure of program") into a readable text listing, one
// program line per output line ("<line#> <content>\n").
//
// This is a hand-written detokenizer, not a clone of the ROM's own LIST
// routine (Technical Reference Manual section 5-4-5 "Program display",
// entry E8CAH) -- that routine only renders to LCD pixels, and matching
// its exact (and not entirely consistent) spacing would require
// disassembling it. Keyword text is exact (from the verified section 5-2
// token table); spacing is our own convention: one space around each
// keyword unless that would sit next to `( ) , ; :` or a quote. The
// result is always readable and content-correct, but won't necessarily
// match a real device's LIST output character-for-character.
//
// An unrecognized 2-byte sequence with a high bit set (should not occur
// for a program the ROM itself produced) is rendered as "[XXXX]" (its
// hex code) rather than silently dropped.
//
// Returns false and sets *error on malformed input (missing 0xFF
// terminator, a line header/content run off the end of `bytes`, or a
// dangling single byte at the end of a line's content where a 2-byte
// token was expected).
bool detokenizeBasicProgram(const std::vector<uint8_t>& bytes, std::string* outText,
                             std::string* error);

}  // namespace pc1500::basic
