// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#include "text_loader.h"

#include <cctype>
#include <fstream>
#include <sstream>

#include "basic_text.h"

namespace pc1500::basic {

namespace {

// Splits a source line's content (i.e. with the leading line number and
// its separating space already removed -- see splitLineNumber) into
// atoms -- minimal chunks that are never safe to split in the middle of,
// so any run of adjacent atoms can be typed together and ENTERed as one
// pass. Confirmed on real hardware (Paul, 2026-08-09): a line does NOT
// need to be a complete/valid statement to be typed and tokenized in one
// pass -- e.g. `IF O=72` can be entered on its own, then resumed and
// extended with `OR O=13`, etc. -- so the old colon-only splitting was
// unnecessarily conservative (it made a single long colon-free statement,
// e.g. a chain of `X=NOR Y=N...THEN`, look unsplittable when it isn't).
// The only real constraint is not to cut a single lexeme in half:
//   - a quoted string literal (through its closing quote, or to end of
//     content if unterminated) is one atom
//   - a maximal run of letters/digits (keyword, identifier, or number --
//     these aren't distinguished from each other, since it's never safe
//     to split any of them internally either way) is one atom
//   - a two-character relational operator (<=, >=, <>) is one atom
//   - every other character (spaces, other operators/punctuation,
//     including ':') is its own one-character atom
// Concatenating all returned atoms reproduces `content` exactly.
std::vector<std::string> splitIntoAtoms(const std::string& content) {
  std::vector<std::string> atoms;
  size_t i = 0;
  while (i < content.size()) {
    char c = content[i];
    if (c == '"') {
      size_t start = i++;
      while (i < content.size() && content[i] != '"') i++;
      if (i < content.size()) i++;  // include closing quote
      atoms.push_back(content.substr(start, i - start));
      continue;
    }
    if (std::isalnum(static_cast<unsigned char>(c))) {
      size_t start = i;
      while (i < content.size() && std::isalnum(static_cast<unsigned char>(content[i]))) i++;
      atoms.push_back(content.substr(start, i - start));
      continue;
    }
    if ((c == '<' || c == '>') && i + 1 < content.size() &&
        (content[i + 1] == '=' || (c == '<' && content[i + 1] == '>'))) {
      atoms.push_back(content.substr(i, 2));
      i += 2;
      continue;
    }
    atoms.push_back(std::string(1, c));
    i++;
  }
  return atoms;
}

// Splits a source line into its leading numeric line number and
// everything after it, skipping at most one separating space (matching
// this project's own typing convention, "10 PRINT ..." -- not required
// if the source line has none). False if `line` doesn't start with a
// digit at all.
bool splitLineNumber(const std::string& line, std::string* numberStr, std::string* content) {
  size_t i = 0;
  while (i < line.size() && line[i] >= '0' && line[i] <= '9') i++;
  if (i == 0) return false;
  *numberStr = line.substr(0, i);
  size_t contentStart = i;
  if (contentStart < line.size() && line[contentStart] == ' ') contentStart++;
  *content = line.substr(contentStart);
  return true;
}

// Walks the line-record chain (same layout findBasicProgramEnd/
// readBasicProgramBytes use: repeated [2-byte line#][1-byte size]
// [content...]) looking for the record whose line# field equals
// `lineNumber`. Returns the record's own start address (the line#'s
// address, not the content's) via *outAddr; false if not found (the
// program's own 0xFFH end marker was reached first).
bool findLineRecord(pc1500::Bus& bus, uint16_t lineNumber, uint32_t* outAddr) {
  uint32_t addr = kBasicProgramStart;
  while (addr <= 0xFFFF) {
    uint8_t hi = bus.readME0(static_cast<uint16_t>(addr));
    if (hi == 0xFF) return false;
    if (addr + 2 > 0xFFFF) return false;
    uint8_t lo = bus.readME0(static_cast<uint16_t>(addr + 1));
    uint16_t thisLineNumber = static_cast<uint16_t>((hi << 8) | lo);
    uint8_t lineSize = bus.readME0(static_cast<uint16_t>(addr + 2));
    if (thisLineNumber == lineNumber) {
      *outAddr = addr;
      return true;
    }
    addr += 3 + lineSize;
  }
  return false;
}

// Reads one line's current stored bytes in isolation (not the whole
// program) and detokenizes them, for verifying a long line's multi-pass
// append actually landed -- see typeBasicProgramText's typeLongLine.
// Wraps the single record in a synthetic one-line "program" (the record
// itself plus a trailing 0xFFH) since detokenizeBasicProgram expects a
// full program blob, not a bare record.
std::string detokenizeStoredLine(pc1500::Bus& bus, uint16_t lineNumber, std::string* error) {
  uint32_t addr = 0;
  if (!findLineRecord(bus, lineNumber, &addr)) {
    *error = "line " + std::to_string(lineNumber) + " not found";
    return "";
  }
  uint8_t lineSize = bus.readME0(static_cast<uint16_t>(addr + 2));
  std::vector<uint8_t> blob;
  blob.reserve(static_cast<size_t>(lineSize) + 4);
  blob.push_back(bus.readME0(static_cast<uint16_t>(addr)));
  blob.push_back(bus.readME0(static_cast<uint16_t>(addr + 1)));
  blob.push_back(lineSize);
  for (int i = 0; i < lineSize; i++) {
    blob.push_back(bus.readME0(static_cast<uint16_t>(addr + 3 + i)));
  }
  blob.push_back(0xFF);
  std::string text;
  if (!detokenizeBasicProgram(blob, &text, error)) return "";
  return text;
}

// Collapses whitespace for a content-only comparison -- the ROM's
// tokenize/detokenize pass doesn't preserve spacing exactly (confirmed,
// see typeBasicProgramText's own header comment and
// tests/basic_load_roundtrip_test.cpp's identically-purposed stripSpaces),
// so a byte-exact substring check would false-fail on cosmetic-only
// differences.
std::string stripSpacesForCompare(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') out.push_back(c);
  }
  return out;
}

}  // namespace

bool charToTapActions(char c, std::deque<QueuedKeyAction>* out) {
  pc1500::Key direct{};
  bool hasDirect = true;
  char upper = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
  if (upper >= 'A' && upper <= 'Z') {
    direct = static_cast<pc1500::Key>(static_cast<int>(pc1500::Key::A) + (upper - 'A'));
  } else if (c >= '0' && c <= '9') {
    direct = static_cast<pc1500::Key>(static_cast<int>(pc1500::Key::Digit0) + (c - '0'));
  } else if (c == ' ') {
    direct = pc1500::Key::Space;
  } else if (c == '.') {
    direct = pc1500::Key::Period;
  } else if (c == '/') {
    direct = pc1500::Key::Slash;
  } else if (c == '+') {
    direct = pc1500::Key::Plus;
  } else if (c == '-') {
    direct = pc1500::Key::Minus;
  } else if (c == '=') {
    direct = pc1500::Key::Equals;
  } else if (c == '(') {
    // LeftParen/RightParen are dedicated, unshifted PC-1500 keys (IN6/PA3
    // and IN0/PA3 -- see docs/pc1500_hardware_reference.md's key matrix)
    // -- '(' and ')' need no PC-1500 Shift at all, unlike '<'/'>' below
    // which are the *shifted* meaning of the same two physical keys.
    direct = pc1500::Key::LeftParen;
  } else if (c == ')') {
    direct = pc1500::Key::RightParen;
  } else if (c == '*') {
    // Asterisk is the PC-1500's dedicated multiply key; unshifted it
    // types '*' -- shifted, it types ':' (handled below).
    direct = pc1500::Key::Asterisk;
  } else {
    hasDirect = false;
  }
  if (hasDirect) {
    out->push_back({direct, true, kTapFrames});
    out->push_back({direct, false, kIdleFrames});
    return true;
  }
  // Symbols needing a PC-1500 Shift-tap sequence -- target keys
  // (F1-F6/Equals/Space/Slash/LeftParen/RightParen/Asterisk/Plus/Minus,
  // each PC-1500-Shifted) confirmed on real hardware to produce these
  // symbols, indexed here by the character itself.
  pc1500::Key shiftedTarget{};
  bool hasShifted = true;
  if (c == '!') {
    shiftedTarget = pc1500::Key::F1;
  } else if (c == '"') {
    shiftedTarget = pc1500::Key::F2;
  } else if (c == '#') {
    shiftedTarget = pc1500::Key::F3;
  } else if (c == '$') {
    shiftedTarget = pc1500::Key::F4;
  } else if (c == '%') {
    shiftedTarget = pc1500::Key::F5;
  } else if (c == '&') {
    shiftedTarget = pc1500::Key::F6;
  } else if (c == '@') {
    shiftedTarget = pc1500::Key::Equals;
  } else if (c == '^') {
    shiftedTarget = pc1500::Key::Space;
  } else if (c == '?') {
    shiftedTarget = pc1500::Key::Slash;
  } else if (c == ':') {
    shiftedTarget = pc1500::Key::Asterisk;
  } else if (c == '<') {
    shiftedTarget = pc1500::Key::LeftParen;
  } else if (c == '>') {
    shiftedTarget = pc1500::Key::RightParen;
  } else if (c == ';') {
    shiftedTarget = pc1500::Key::Plus;
  } else if (c == ',') {
    shiftedTarget = pc1500::Key::Minus;
  } else {
    hasShifted = false;
  }
  if (hasShifted) {
    out->push_back({pc1500::Key::Shift, true, kTapFrames});
    out->push_back({pc1500::Key::Shift, false, kIdleFrames});
    out->push_back({shiftedTarget, true, kTapFrames});
    out->push_back({shiftedTarget, false, kIdleFrames});
    return true;
  }
  return false;
}

uint32_t findBasicProgramEnd(pc1500::Bus& bus) {
  uint32_t addr = kBasicProgramStart;
  while (addr <= 0xFFFF) {
    uint8_t hi = bus.readME0(static_cast<uint16_t>(addr));
    if (hi == 0xFF) return addr;  // end-of-program marker
    if (addr + 2 > 0xFFFF) break;
    uint8_t lineSize = bus.readME0(static_cast<uint16_t>(addr + 2));
    // lineSize already includes the trailing 0DH terminator byte (verified
    // against the manual's own worked example: line "10 PRINT A"'s size
    // byte is 04H, covering exactly F0 97 41 0D -- 3 content bytes plus
    // the CR, not just the content).
    addr += 3 + lineSize;  // line#(2) + size(1) + (content + CR)(lineSize)
  }
  return 0;
}

std::vector<uint8_t> readBasicProgramBytes(pc1500::Bus& bus, std::string* error) {
  uint32_t endAddr = findBasicProgramEnd(bus);
  if (endAddr == 0) {
    *error = "Could not find end of BASIC program (corrupt program area?).";
    return {};
  }
  std::vector<uint8_t> data;
  data.reserve(endAddr - kBasicProgramStart + 1);
  for (uint32_t a = kBasicProgramStart; a <= endAddr; a++) {
    data.push_back(bus.readME0(static_cast<uint16_t>(a)));
  }
  return data;
}

bool saveBasicProgram(pc1500::Bus& bus, const char* path, std::string* error) {
  std::vector<uint8_t> data = readBasicProgramBytes(bus, error);
  if (data.empty()) return false;
  std::ofstream f(path, std::ios::binary);
  if (!f) {
    *error = "Could not open file for writing.";
    return false;
  }
  f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  return true;
}

bool saveBasicTextFile(pc1500::Bus& bus, const char* path, std::string* error) {
  std::vector<uint8_t> data = readBasicProgramBytes(bus, error);
  if (data.empty()) return false;
  std::string text;
  if (!pc1500::basic::detokenizeBasicProgram(data, &text, error)) return false;
  std::ofstream f(path, std::ios::binary);
  if (!f) {
    *error = "Could not open file for writing.";
    return false;
  }
  f.write(text.data(), static_cast<std::streamsize>(text.size()));
  return true;
}

namespace {
std::vector<uint8_t> readFile(const char* path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}
}  // namespace

bool loadBasicProgram(pc1500::Bus& bus, const char* path, std::string* error) {
  std::vector<uint8_t> data = readFile(path);
  if (data.empty()) {
    *error = "Could not read file (or file is empty).";
    return false;
  }
  if (data.back() != 0xFF) {
    *error = "File doesn't end with the BASIC end-of-program marker (FFH) -- not a saved BASIC program?";
    return false;
  }
  bus.loadME0(kBasicProgramStart, data.data(), data.size());
  uint32_t endAddr = kBasicProgramStart + data.size() - 1;
  bus.writeME0(kProgramEndPointerAddr, static_cast<uint8_t>(endAddr >> 8));
  bus.writeME0(kProgramEndPointerAddr + 1, static_cast<uint8_t>(endAddr & 0xFF));
  return true;
}

bool typeBasicProgramText(pc1500::Bus& bus, lh5801::CPU& cpu, const std::string& text,
                           int cyclesPerFrame, int cyclesPerTimerTick, std::string* error,
                           std::function<void()> onProgress) {
  int cyclesSinceTimerTick = 0;
  auto stepCycles = [&](long cycles) {
    for (long i = 0; i < cycles;) {
      int c = cpu.step();
      int used = (c > 0) ? c : 1;
      i += used;
      cyclesSinceTimerTick += used;
      bus.advanceCycles(used);
      while (cyclesSinceTimerTick >= cyclesPerTimerTick) {
        cpu.tickTimer();
        cyclesSinceTimerTick -= cyclesPerTimerTick;
      }
    }
  };
  auto runKeyAction = [&](const QueuedKeyAction& action) {
    bus.setKeyState(action.key, action.pressed);
    stepCycles(static_cast<long>(action.framesToWait) * cyclesPerFrame);
  };
  // SML is a persistent lowercase-input toggle on real hardware (confirmed:
  // it stays in effect across Enter, not just within one line), so this
  // state has to live for the whole function rather than being tracked
  // per-line or per-character. charToTapActions folds 'a'-'z' to the same
  // physical key as 'A'-'Z' (there's only one physical key per letter --
  // case is a ROM-side keyboard mode, not a separate keystroke), so
  // without this, lowercase source text like hexload1500.bas's German
  // prompts ("Anfangsadresse (dez.):") always typed as uppercase.
  bool smlActive = false;
  auto setSml = [&](bool active) {
    if (active == smlActive) return;
    runKeyAction({pc1500::Key::Sml, true, kTapFrames});
    runKeyAction({pc1500::Key::Sml, false, kIdleFrames});
    smlActive = active;
  };
  auto typeChar = [&](char c) {
    setSml(c >= 'a' && c <= 'z');
    std::deque<QueuedKeyAction> actions;
    if (!charToTapActions(c, &actions)) return false;
    for (const QueuedKeyAction& action : actions) runKeyAction(action);
    return true;
  };
  auto pressEnter = [&]() {
    runKeyAction({pc1500::Key::Ent, true, kTapFrames});
    runKeyAction({pc1500::Key::Ent, false, kIdleFrames});
  };
  auto pressCl = [&]() {
    runKeyAction({pc1500::Key::Cl, true, kTapFrames});
    runKeyAction({pc1500::Key::Cl, false, kIdleFrames});
  };
  auto pressRight = [&]() {
    runKeyAction({pc1500::Key::Right, true, kTapFrames});
    runKeyAction({pc1500::Key::Right, false, kIdleFrames});
  };
  // 90 is well past any line length this project supports (max ~a few
  // hundred bytes of stored content) -- confirmed on real hardware that
  // Right Arrow simply stops advancing once at the true end of a line's
  // content, regardless of how many more times it's pressed, so
  // over-pressing here is safe rather than needing to compute/detect the
  // exact redisplayed length.
  auto navigateToLineEnd = [&]() {
    for (int i = 0; i < 90; i++) pressRight();
  };

  // The PC-1500's continuation-pass budget: a resumed edit pass's own
  // newly-typed raw characters, added to the line's *current stored* size
  // (read directly from bus memory -- this project's automation has that
  // ground truth available, unlike a human typing on real hardware, so
  // there's no need to estimate the redisplayed/detokenized length at
  // all), roughly must not exceed this -- used only to decide how many
  // atoms to *try* packing into a pass. It's a starting estimate, not an
  // exact boundary: empirically (2026-08-09, headless tests against a
  // real ROM dump) the true cutoff turned out to vary by a character or
  // two between otherwise-similar cases (e.g. Blackjack.bas line 670
  // accepted a storedSize 71 + 6-char append at total 77, but line 1270
  // silently dropped the last character of a storedSize 65 + 12-char
  // append, also at total 77) -- so rather than chase an exact constant,
  // typeLongLine detects a short append below and resumes typing exactly
  // where it left off instead of trusting this number to be precise.
  constexpr int kContinuationPassBudget = 77;

  // Finds the longest prefix of `attempted` that actually made it into
  // `detok` (space-tolerant, matching the check the caller already does
  // to decide pass/fail) -- used to recover from a pass that got
  // partially, silently truncated by the ROM's own input buffer instead
  // of failing outright.
  auto longestAcceptedPrefixLen = [&](const std::string& detok,
                                       const std::string& attempted) -> size_t {
    std::string detokStripped = stripSpacesForCompare(detok);
    for (size_t len = attempted.size(); len > 0; len--) {
      if (detokStripped.find(stripSpacesForCompare(attempted.substr(0, len))) !=
          std::string::npos) {
        return len;
      }
    }
    return 0;
  };

  // Types one source-text line whose raw length exceeds the ROM's
  // single-pass 79-character input limit, across multiple LIST-and-append
  // editing passes -- the real technique PC-1500 users used to enter
  // lines whose *tokenized* stored size exceeds what any single raw
  // input burst could produce (tokenization compresses keywords, so a
  // line's stored size can keep growing well past 79 across passes even
  // though each individual pass's raw typing stays under the cap). A
  // line does not need to be a complete/valid statement at the end of any
  // given pass -- confirmed on real hardware -- so passes split at atom
  // boundaries (see splitIntoAtoms), not just at colons.
  //
  // Pass 1 packs as many whole atoms as fit in the ordinary 79-char
  // fresh-line limit. Each subsequent pass reads the line's real current
  // stored size, computes exactly how much raw-char room is left under
  // kContinuationPassBudget, and packs as many more whole atoms as fit --
  // then LISTs the line back up, navigates to its end, types the packed
  // atoms, and presses Enter again. After every pass (including pass 1,
  // via the same beforeEnd/afterEnd check the short-line path below
  // uses), and additionally after every continuation pass specifically,
  // the freshly-stored line is detokenized and checked to actually
  // contain what was just typed -- a safety net independent of the
  // budget arithmetic above being exactly right, since a
  // silently-truncated append is otherwise indistinguishable from a
  // normally-compact tokenization (both make stored size grow by less
  // than what was typed).
  auto typeLongLine = [&](const std::string& fullLine) -> bool {
    std::string numberStr, content;
    if (!splitLineNumber(fullLine, &numberStr, &content)) {
      *error = "long line has no leading line number, can't resume-edit it: " + fullLine;
      return false;
    }
    uint16_t lineNumber = static_cast<uint16_t>(std::stoul(numberStr));
    std::vector<std::string> segments = splitIntoAtoms(content);

    size_t maxPack = 0;
    {
      std::string probe = numberStr + " ";
      while (maxPack < segments.size() && probe.size() + segments[maxPack].size() <= 79) {
        probe += segments[maxPack];
        maxPack++;
      }
    }
    if (maxPack == 0) {
      *error = "line " + numberStr +
               " has a single unsplittable token too long to enter even as the first pass: " +
               segments[0];
      return false;
    }
    // The ROM's tokenizer rejects a pass outright if it ends with an
    // unbalanced paren (confirmed via DungeonQuest.bas line 35: greedily
    // packing to the 79-char limit lands mid-"A$(X,Y)", cutting right
    // after the "(", and the ROM refuses to store the line at all rather
    // than accepting the partial expression). Rather than special-casing
    // parens specifically -- there may be other tokenizer rules like this
    // one that haven't been hit yet -- back the packed atom count off by
    // one and retry whenever the ROM rejects a pass entirely, down to a
    // single atom if it comes to that.
    size_t atomCount = maxPack;
    std::string pass1Text;
    bool accepted = false;
    while (atomCount > 0) {
      pass1Text = numberStr + " ";
      for (size_t i = 0; i < atomCount; i++) pass1Text += segments[i];
      for (char c : pass1Text) {
        if (!typeChar(c)) {
          *error = "no keystroke mapping for character '" + std::string(1, c) + "' in line: " + fullLine;
          return false;
        }
      }
      pressEnter();
      stepCycles(4L * kIdleFrames * cyclesPerFrame);
      std::string verifyError;
      std::string detok = detokenizeStoredLine(bus, lineNumber, &verifyError);
      if (stripSpacesForCompare(detok).find(stripSpacesForCompare(pass1Text)) != std::string::npos) {
        accepted = true;
        break;
      }
      // Rejected outright -- clear the failed attempt and retry with one
      // fewer atom packed in.
      pressCl();
      atomCount--;
    }
    if (!accepted) {
      *error = "line " + numberStr +
               ": the ROM rejected every first-pass packing down to a single token, starting with: " +
               segments[0];
      return false;
    }
    size_t segIdx = atomCount;

    // `carry`: leftover text from a pass that got partially, silently
    // truncated by the ROM -- must be retyped (from exactly where it left
    // off) before any further atoms are packed. Non-empty only while
    // recovering from a short append; `carryTargetIdx` is the segIdx a
    // fully-accepted carry represents (i.e. what segIdx becomes once
    // carry is completely flushed).
    std::string carry;
    size_t carryTargetIdx = segIdx;
    int carryStalls = 0;
    // Caps how many atoms a fresh (non-carry) pack may pack in, beyond the
    // ordinary kContinuationPassBudget constraint -- normally no extra
    // limit, but ratcheted down by one atom whenever a freshly-packed pass
    // is rejected outright (see below), so the next attempt lands on an
    // earlier split point instead of retrying the identical text. Reset
    // once a pack actually lands.
    size_t maxFreshPackAtoms = segments.size();
    while (segIdx < segments.size() || !carry.empty()) {
      if (onProgress) onProgress();
      std::string passText;
      size_t startIdx = segIdx;
      bool freshPack = carry.empty();
      if (!carry.empty()) {
        passText = carry;
      } else {
        uint32_t addr = 0;
        if (!findLineRecord(bus, lineNumber, &addr)) {
          *error = "line " + numberStr + " not found after a previous pass";
          return false;
        }
        int storedSize = bus.readME0(static_cast<uint16_t>(addr + 2));
        int room = kContinuationPassBudget - storedSize;
        if (room <= 0) {
          *error = "line " + numberStr + " has no room left (stored size " +
                   std::to_string(storedSize) + ") to append: " + segments[segIdx];
          return false;
        }
        while (segIdx < segments.size() && segIdx - startIdx < maxFreshPackAtoms &&
               static_cast<int>(passText.size() + segments[segIdx].size()) <= room) {
          passText += segments[segIdx];
          segIdx++;
        }
        if (segIdx == startIdx) {
          *error = "line " + numberStr + "'s next token doesn't fit in the " +
                   std::to_string(room) + " characters left this pass: " + segments[segIdx];
          return false;
        }
        carryTargetIdx = segIdx;
        segIdx = startIdx;  // only committed once this pass is confirmed below
      }

      for (char c : std::string("LIST ") + numberStr) {
        if (!typeChar(c)) {
          *error = "no keystroke mapping for character '" + std::string(1, c) + "'";
          return false;
        }
      }
      pressEnter();
      stepCycles(4L * kIdleFrames * cyclesPerFrame);
      navigateToLineEnd();
      for (char c : passText) {
        if (!typeChar(c)) {
          *error = "no keystroke mapping for character '" + std::string(1, c) + "' in line: " + fullLine;
          return false;
        }
      }
      pressEnter();
      stepCycles(4L * kIdleFrames * cyclesPerFrame);

      std::string verifyError;
      std::string detok = detokenizeStoredLine(bus, lineNumber, &verifyError);
      size_t acceptedLen = detok.empty() ? 0 : longestAcceptedPrefixLen(detok, passText);
      if (acceptedLen >= passText.size()) {
        // Fully accepted: commit the atoms this pass represented.
        carry.clear();
        maxFreshPackAtoms = segments.size();
        segIdx = carryTargetIdx;
        continue;
      }
      if (freshPack && acceptedLen == 0 && carryTargetIdx - startIdx > 1) {
        // Rejected outright (nothing landed at all), same tokenizer
        // behavior as typeLongLine's pass-1 packing above (e.g. an
        // unbalanced paren mid-expression) -- not the silent-truncation
        // case below, which always lands *something*. Retrying the exact
        // same text would just fail the same way every time, so back this
        // fresh pack off by one atom and retry the packing from scratch
        // instead of falling into the carry-retry path.
        pressCl();
        maxFreshPackAtoms = (carryTargetIdx - startIdx) - 1;
        segIdx = startIdx;
        continue;
      }
      // Partially (or not at all) accepted -- the ROM's own input buffer
      // silently dropped the tail rather than rejecting the pass outright
      // (confirmed: this can cut mid-keyword, e.g. "GOSUB" -> "GOSU").
      // Resume from exactly where it left off, without needing to know
      // the precise cutoff in advance.
      std::string remainder = passText.substr(acceptedLen);
      if (remainder.size() == carry.size() && remainder == carry) {
        carryStalls++;
      } else {
        carryStalls = 0;
      }
      carry = remainder;
      segIdx = startIdx;
      if (carryStalls > 5) {
        *error = "line " + numberStr + ": append of '" + passText +
                 "' made no progress after repeated retries (likely silently truncated) -- "
                 "stored line now reads: " + detok;
        return false;
      }
    }
    return true;
  };

  // The ROM boots (and returns after certain operations) to a state that
  // doesn't respond to typed characters until CL is pressed once --
  // confirmed empirically. One press here, not per-line: once the first
  // line is accepted, the ROM's own line editor returns to a fresh prompt
  // ready for the next line on its own.
  pressCl();

  // Full replace, not merge: clear the program through the ROM's own NEW
  // command (typed, like everything else here) rather than poking
  // kBasicProgramStart/kProgramEndPointerAddr directly -- confirmed
  // empirically that a direct poke leaves the program area in a state the
  // ROM's own periodic memory-validity pass doesn't recognize as either
  // "freshly typed" or "properly cleared", and it quietly re-zeroes
  // 4000H-47FFH (including whatever we'd just typed) within the next
  // several hundred thousand cycles.
  for (char c : std::string("NEW0")) typeChar(c);
  pressEnter();
  // NEW's own memory-clear work isn't done the instant Enter is processed
  // -- give it extra settling time beyond the usual per-key idle gap
  // before typing the first program line, confirmed empirically necessary.
  stepCycles(4L * kIdleFrames * cyclesPerFrame);

  std::vector<std::string> rejectedLines;
  std::istringstream lineStream(text);
  std::string line;
  while (std::getline(lineStream, line)) {
    if (onProgress) onProgress();
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.find_first_not_of(" \t") == std::string::npos) continue;  // blank line

    // The ROM's own line editor has a hard 79-character input limit
    // (confirmed empirically: an 80-character line has its 80th character
    // and everything after it silently dropped, with no error shown) --
    // but a line's *tokenized* stored size can still exceed that, via the
    // real multi-pass LIST-and-append technique typeLongLine implements
    // (see its own comment). Only a truly unsplittable line (a single
    // atom -- e.g. one identifier or a quoted string -- longer than any
    // single pass could ever fit) still fails outright, from inside
    // typeLongLine itself.
    if (line.size() > 79) {
      if (!typeLongLine(line)) return false;
      continue;
    }

    uint32_t beforeEnd = findBasicProgramEnd(bus);
    for (char c : line) {
      if (!typeChar(c)) {
        *error = "no keystroke mapping for character '" + std::string(1, c) + "' in line: " + line;
        return false;
      }
    }
    pressEnter();
    // A line's own tokenization/storage work isn't necessarily done the
    // instant Enter is processed -- same issue as NEW0's own extra
    // settling above, but per-line instead of once.
    stepCycles(4L * kIdleFrames * cyclesPerFrame);
    if (findBasicProgramEnd(bus) == beforeEnd) {
      // The ROM didn't grow the program area at all -- it rejected this
      // line (most likely a syntax error). We can't recover its specific
      // error text without decoding LCD pixels, but we can at least
      // surface which line didn't take instead of silently dropping it.
      rejectedLines.push_back(line);
    }
  }

  if (!rejectedLines.empty()) {
    std::string msg =
        std::to_string(rejectedLines.size()) + " line(s) rejected by the ROM (syntax error?): ";
    for (size_t i = 0; i < rejectedLines.size(); i++) {
      if (i > 0) msg += " | ";
      msg += rejectedLines[i];
    }
    *error = msg;
    return false;
  }
  setSml(false);
  return true;
}

}  // namespace pc1500::basic
