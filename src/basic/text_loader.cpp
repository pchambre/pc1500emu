// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#include "text_loader.h"

#include <fstream>
#include <sstream>

#include "basic_text.h"

namespace pc1500::basic {

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
                           int cyclesPerFrame, int cyclesPerTimerTick, std::string* error) {
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
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.find_first_not_of(" \t") == std::string::npos) continue;  // blank line

    // The ROM's own line editor has a hard 79-character input limit
    // (confirmed empirically: an 80-character line has its 80th character
    // and everything after it silently dropped, with no error shown).
    if (line.size() > 79) {
      *error = "line exceeds the PC-1500's 79-character input limit (" +
               std::to_string(line.size()) + " chars) and would be silently truncated by the "
               "ROM -- split it into multiple lines: " + line;
      return false;
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
