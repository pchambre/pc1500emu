// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#include "basic_text.h"

#include <cstdio>

#include "basic_tokens.h"

namespace pc1500::basic {

namespace {

bool isNoSpaceChar(char c) {
  return c == '(' || c == ')' || c == ',' || c == ';' || c == ':' || c == '"';
}

}  // namespace

bool detokenizeBasicProgram(const std::vector<uint8_t>& bytes, std::string* outText,
                             std::string* error) {
  outText->clear();
  size_t pos = 0;
  while (true) {
    if (pos >= bytes.size()) {
      *error = "missing 0xFF end-of-program marker";
      return false;
    }
    if (bytes[pos] == 0xFF) break;
    if (pos + 3 > bytes.size()) {
      *error = "line header runs off the end of the data";
      return false;
    }
    uint16_t lineNo = (static_cast<uint16_t>(bytes[pos]) << 8) | bytes[pos + 1];
    uint8_t lineSize = bytes[pos + 2];
    size_t contentStart = pos + 3;
    if (contentStart + lineSize > bytes.size()) {
      *error = "line " + std::to_string(lineNo) + ": content runs off the end of the data";
      return false;
    }
    // lineSize includes the trailing 0x0DH, which isn't part of the
    // rendered content.
    size_t contentLen = lineSize > 0 ? static_cast<size_t>(lineSize) - 1 : 0;
    size_t contentEnd = contentStart + contentLen;

    outText->append(std::to_string(lineNo));
    outText->push_back(' ');

    bool inQuote = false;
    bool pendingSpaceBeforeNext = false;
    size_t i = contentStart;
    while (i < contentEnd) {
      uint8_t b = bytes[i];
      if (inQuote) {
        outText->push_back(static_cast<char>(b));
        if (b == '"') inQuote = false;
        i++;
        continue;
      }
      if (b == '"') {
        // Unlike the general no-space punctuation set, an *opening* quote
        // still wants the space a preceding keyword requested (GOSUB
        // "CRASH", not GOSUB"CRASH") -- it's only a *closing* quote
        // immediately before the next keyword that suppresses one (the
        // "CRASH"FOR idiom seen in real listings), which the isKeyword
        // check below already handles via isNoSpaceChar on the preceding
        // character.
        if (pendingSpaceBeforeNext) outText->push_back(' ');
        inQuote = true;
        outText->push_back('"');
        pendingSpaceBeforeNext = false;
        i++;
        continue;
      }

      std::string unit;
      bool isKeyword = false;
      if (b < 0x80) {
        unit.push_back(static_cast<char>(b));
        i++;
      } else {
        if (i + 1 >= contentEnd) {
          *error = "line " + std::to_string(lineNo) + ": truncated token at end of line";
          return false;
        }
        uint16_t code = (static_cast<uint16_t>(b) << 8) | bytes[i + 1];
        const char* kw = keywordForCode(code);
        if (kw) {
          unit = kw;
        } else {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "[%04X]", code);
          unit = buf;
        }
        isKeyword = true;
        i += 2;
      }

      bool needSpaceBefore = pendingSpaceBeforeNext && !isNoSpaceChar(unit.front());
      if (isKeyword && !outText->empty()) {
        char last = outText->back();
        if (last != ' ' && !isNoSpaceChar(last)) needSpaceBefore = true;
      }
      if (needSpaceBefore) outText->push_back(' ');
      outText->append(unit);
      pendingSpaceBeforeNext = isKeyword;
    }
    outText->push_back('\n');
    pos = contentStart + lineSize;
  }
  return true;
}

}  // namespace pc1500::basic
