// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
//
// pc1500runner: runs a machine-code program headlessly and checks what it
// does -- boots ROM1, loads a .bin, CALLs it from BASIC, plays a key
// script, and reads the LCD back as text. Made for testing C programs
// built with the LH5801 SDCC port (sdcc-pc1500's support/regression-lh5801
// runs its tests through it), but nothing in it is specific to them.
//
// usage: pc1500runner <rom> <bin> <load hex> [options] [script...]
//   --ram0000 N    bytes of RAM at 0000H (e.g. 16384)
//   --ramext N     bytes of extension RAM at 4800H (e.g. 8192, 10240)
//   --call ADDR    CALL address (default: the load address)
//   --new ADDR     type NEW &ADDR first, reserving the program's RAM
//   --map FILE     an sdld .map: lets addresses below name symbols
//   -f FILE        read more script tokens from FILE (whitespace-separated,
//                  # to end of line is a comment)
//   -q             only print failures and the final line
//
// script tokens, run in order after the CALL:
//   t:TEXT        type TEXT, one key every 250 ms (a person's pace -- the
//                 ROM's key wait drops a key that follows another too closely)
//   k:NAME        tap ENT CL UP DOWN LEFT RIGHT SPACE SHIFT DEF SML OFF MODE,
//                 or a character; then 250 ms
//   w:MS          run MS milliseconds
//   s             print the screen
//   u:TEXT:MS     run until the screen contains TEXT, at most MS; fails on
//                 timeout
//   e:TEXT        fail unless the screen contains TEXT now
//                 (in u:/e: TEXT, '_' stands for a space)
//   r:ADDR:N      print N bytes from ADDR
//   p:ADDR:TEXT   write TEXT's bytes at ADDR (p:ADDR:=HEX writes hex bytes)
//   x:ADDR:HEX    fail unless the bytes at ADDR are HEX (e.g. x:_w:0000019A)
// ADDR: terms joined by '+': a hex number, a map symbol (_name), or
// [ADDR] -- the byte at ADDR (so _deck+[_b]+1 is the card after deck[b]).
//
// Exit status: 0 if every check passed, 1 if one failed, 2 on bad usage.
// The LCD is read with the ROM's own font, learned at startup from BASIC
// PRINTs; a character drawn inverted reads as its lowercase, an
// unrecognized cell as '~'.
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "bus.h"
#include "keyboard.h"
#include "lcd.h"
#include "lh5801.h"
#include "text_loader.h"

using pc1500::Key;

namespace {

// Must match src/host/main.cpp's timing constants -- see
// tests/basic_load_roundtrip_test.cpp's identical copy.
constexpr int kCyclesPerSecond = 1300000;
constexpr int kCyclesPerFrame = kCyclesPerSecond / 60;
constexpr int kCyclesPerTimerTick = 8;
constexpr int kKeyPaceMs = 250;

struct Machine {
  pc1500::Keyboard keyboard;
  pc1500::Bus bus{keyboard};
  lh5801::CPU cpu{bus};
  int sinceTick = 0;
  long long cycles = 0;

  void step(long n) {
    for (long i = 0; i < n;) {
      int c = cpu.step();
      int used = c > 0 ? c : 1;
      i += used;
      cycles += used;
      sinceTick += used;
      bus.advanceCycles(used);
      while (sinceTick >= kCyclesPerTimerTick) {
        cpu.tickTimer();
        sinceTick -= kCyclesPerTimerTick;
      }
    }
  }
  void ms(long n) { step(n * (kCyclesPerSecond / 1000)); }
  void run(const std::deque<pc1500::basic::QueuedKeyAction>& actions) {
    for (const auto& a : actions) {
      bus.setKeyState(a.key, a.pressed);
      step(static_cast<long>(a.framesToWait) * kCyclesPerFrame);
    }
  }
  void tap(Key k) {
    run({{k, true, pc1500::basic::kTapFrames}, {k, false, pc1500::basic::kIdleFrames}});
  }
  bool type(const std::string& s) {
    bool ok = true;
    for (char c : s) {
      std::deque<pc1500::basic::QueuedKeyAction> a;
      if (!pc1500::basic::charToTapActions(c, &a)) ok = false;
      run(a);
    }
    return ok;
  }
  double seconds() const { return cycles / static_cast<double>(kCyclesPerSecond); }
};

std::vector<uint8_t> readFile(const char* path) {
  std::ifstream f(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// --- the screen, read with the ROM's own font -----------------------------

pc1500::Lcd lcd;
std::map<uint64_t, char> glyphs;

uint64_t cellAt(Machine& m, int cell) {
  uint64_t g = 0;
  for (int c = 0; c < 5; c++)
    g = (g << 8) | lcd.columnBits(cell * 6 + c, true, [&](uint16_t a) { return m.bus.readME0(a); });
  return g;
}

uint64_t inverted(uint64_t g) {
  uint64_t r = 0;
  for (int c = 0; c < 5; c++) r |= static_cast<uint64_t>(~(g >> (8 * c)) & 0x7F) << (8 * c);
  return r;
}

std::string screen(Machine& m) {
  std::string out;
  for (int i = 0; i < 26; i++) {
    uint64_t g = cellAt(m, i);
    auto it = glyphs.find(g);
    if (it != glyphs.end()) {
      out += it->second;
    } else if ((it = glyphs.find(inverted(g))) != glyphs.end()) {
      out += it->second == ' ' ? '_' : static_cast<char>(std::tolower(it->second));
    } else {
      out += '~';
    }
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

void learnPrinted(Machine& m, const std::string& chars, bool lowercase) {
  m.tap(Key::Cl);
  std::deque<pc1500::basic::QueuedKeyAction> a;
  pc1500::basic::SmlAwareTyper typer(m.bus);
  for (char c : "PRINT \"" + chars + "\"") typer.typeChar(c, &a);
  typer.setActive(false, &a);
  m.run(a);
  m.tap(Key::Ent);
  m.ms(300);
  for (size_t i = 0; i < chars.size(); i++) glyphs[cellAt(m, static_cast<int>(i))] = chars[i];
  (void)lowercase;
}

void learnFont(Machine& m) {
  learnPrinted(m, "ABCDEFGHIJKLMNOPQRSTUVWXYZ", false);
  learnPrinted(m, "0123456789 .,;:!?+-*/=<>()", false);
  learnPrinted(m, "#$%&@^", false);
  learnPrinted(m, "abcdefghijklmnopqrstuvwxyz", true);
  m.tap(Key::Cl);
  m.ms(100);
}

// --- addresses ---------------------------------------------------------------

std::map<std::string, unsigned> symbols;

void loadMap(const char* path) {
  std::ifstream f(path);
  std::string line;
  while (std::getline(f, line)) {
    std::istringstream is(line);
    std::string addr, name;
    if (!(is >> addr >> name) || name.empty() || name[0] != '_' || addr.size() != 8) continue;
    bool hex = true;
    for (char c : addr) hex = hex && std::isxdigit(static_cast<unsigned char>(c));
    if (hex) symbols[name] = std::stoul(addr, nullptr, 16) & 0xFFFF;
  }
}

bool evalAddr(Machine& m, const std::string& s, unsigned* out);

bool evalTerm(Machine& m, const std::string& t, unsigned* out) {
  if (t.empty()) return false;
  if (t.front() == '[' && t.back() == ']') {
    unsigned a;
    if (!evalAddr(m, t.substr(1, t.size() - 2), &a)) return false;
    *out = m.bus.readME0(static_cast<uint16_t>(a));
    return true;
  }
  if (t[0] == '_') {
    auto it = symbols.find(t);
    if (it == symbols.end()) return false;
    *out = it->second;
    return true;
  }
  char* end = nullptr;
  *out = static_cast<unsigned>(std::strtoul(t.c_str(), &end, 16));
  return end && *end == 0;
}

// Terms joined by '+' (brackets nest, so split only at depth 0).
bool evalAddr(Machine& m, const std::string& s, unsigned* out) {
  unsigned sum = 0;
  int depth = 0;
  size_t start = 0;
  for (size_t i = 0; i <= s.size(); i++) {
    if (i < s.size() && s[i] == '[') depth++;
    if (i < s.size() && s[i] == ']') depth--;
    if (i == s.size() || (s[i] == '+' && depth == 0)) {
      unsigned v;
      if (!evalTerm(m, s.substr(start, i - start), &v)) return false;
      sum += v;
      start = i + 1;
    }
  }
  *out = sum & 0xFFFF;
  return true;
}

// Script tokens can't contain spaces, so '_' in u:/e: text stands for one.
std::string spaces(std::string s) {
  for (char& c : s)
    if (c == '_') c = ' ';
  return s;
}

// "ADDR:REST" where ADDR may itself contain ':'-free brackets.
bool splitAddr(const std::string& arg, std::string* addr, std::string* rest) {
  size_t c = arg.find(':');
  if (c == std::string::npos) return false;
  *addr = arg.substr(0, c);
  *rest = arg.substr(c + 1);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr,
                 "usage: pc1500runner <rom> <bin> <load hex> [--ram0000 N] [--ramext N] [--call ADDR]\n"
                 "                    [--new ADDR] [--map FILE] [-f FILE] [-q] [script...]\n");
    return 2;
  }
  std::vector<uint8_t> rom = readFile(argv[1]);
  std::vector<uint8_t> bin = readFile(argv[2]);
  unsigned load = std::stoul(argv[3], nullptr, 16);
  unsigned call = load;
  size_t ram0000 = 0, ramext = 0;
  std::string reserve;
  bool quiet = false;
  std::vector<std::string> script;

  for (int i = 4; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s needs a value\n", a.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--ram0000") ram0000 = std::stoul(next());
    else if (a == "--ramext") ramext = std::stoul(next());
    else if (a == "--call") call = std::stoul(next(), nullptr, 16);
    else if (a == "--new") reserve = next();
    else if (a == "--map") loadMap(next().c_str());
    else if (a == "-q") quiet = true;
    else if (a == "-f") {
      std::ifstream f(next());
      std::string line;
      while (std::getline(f, line)) {
        line = line.substr(0, line.find('#'));
        std::istringstream is(line);
        std::string tok;
        while (is >> tok) script.push_back(tok);
      }
    } else {
      script.push_back(a);
    }
  }
  if (rom.empty() || bin.empty()) {
    std::fprintf(stderr, "can't read %s\n", rom.empty() ? argv[1] : argv[2]);
    return 2;
  }

  Machine m;
  m.bus.ioPort().useManualRtcClock();
  m.bus.loadME0(0xC000, rom.data(), rom.size());
  if (ram0000) m.bus.setExtRam0000Size(ram0000);
  if (ramext) m.bus.setExtRamExtSize(ramext);
  m.cpu.reset();
  while (!m.cpu.halted() && m.cycles < 20'000'000) m.step(1);
  m.step(4'000'000);
  m.tap(Key::Cl);
  m.type("NEW0");  // the cold-boot state, cleared as on a real machine
  m.tap(Key::Ent);
  m.ms(300);
  learnFont(m);
  if (!reserve.empty()) {
    m.tap(Key::Cl);
    m.type("NEW &" + reserve);
    m.tap(Key::Ent);
    m.ms(500);
  }
  if (!quiet)
    std::printf("[%zu glyphs; BASIC from %04X]\n", glyphs.size(), pc1500::basic::basicProgramStart(m.bus));
  m.bus.loadME0(static_cast<uint16_t>(load), bin.data(), bin.size());
  char callText[32];
  std::snprintf(callText, sizeof callText, "CALL &%04X", call);
  m.type(callText);
  m.tap(Key::Ent);

  static const std::map<std::string, Key> names = {
      {"ENT", Key::Ent},   {"CL", Key::Cl},       {"UP", Key::Up},       {"DOWN", Key::Down},
      {"LEFT", Key::Left}, {"RIGHT", Key::Right}, {"SPACE", Key::Space}, {"SHIFT", Key::Shift},
      {"DEF", Key::Def},   {"SML", Key::Sml},     {"OFF", Key::Off},     {"MODE", Key::Mode}};
  int failures = 0;
  auto fail = [&](const std::string& tok, const std::string& why) {
    std::printf("%8.2fs FAIL %s: %s |%s|\n", m.seconds(), tok.c_str(), why.c_str(), screen(m).c_str());
    failures++;
  };

  for (const std::string& tok : script) {
    std::string arg = tok.size() > 2 ? tok.substr(2) : "";
    if (tok.rfind("t:", 0) == 0) {
      for (char c : arg) {
        if (!m.type(std::string(1, c))) fail(tok, std::string("no key for '") + c + "'");
        m.ms(kKeyPaceMs);
      }
    } else if (tok.rfind("k:", 0) == 0) {
      auto it = names.find(arg);
      if (it != names.end()) m.tap(it->second);
      else if (!m.type(arg)) fail(tok, "no such key");
      m.ms(kKeyPaceMs);
    } else if (tok.rfind("w:", 0) == 0) {
      m.ms(std::stol(arg));
    } else if (tok == "s") {
      std::printf("%8.2fs |%s|\n", m.seconds(), screen(m).c_str());
    } else if (tok.rfind("u:", 0) == 0) {
      size_t c = arg.rfind(':');
      std::string text = spaces(arg.substr(0, c));
      long maxMs = std::stol(arg.substr(c + 1)), t = 0;
      while (t < maxMs && screen(m).find(text) == std::string::npos) {
        m.ms(20);
        t += 20;
      }
      if (t >= maxMs && screen(m).find(text) == std::string::npos) fail(tok, "timed out");
      else if (!quiet) std::printf("%8.2fs |%s|\n", m.seconds(), screen(m).c_str());
    } else if (tok.rfind("e:", 0) == 0) {
      if (screen(m).find(spaces(arg)) == std::string::npos) fail(tok, "not on screen");
    } else if (tok.rfind("r:", 0) == 0 || tok.rfind("p:", 0) == 0 || tok.rfind("x:", 0) == 0) {
      std::string where, rest;
      unsigned addr;
      if (!splitAddr(arg, &where, &rest) || !evalAddr(m, where, &addr)) {
        fail(tok, "bad address");
        continue;
      }
      if (tok[0] == 'r') {
        std::printf("[%04X]", addr);
        for (int k = 0; k < std::stoi(rest); k++)
          std::printf(" %02X", m.bus.readME0(static_cast<uint16_t>(addr + k)));
        std::printf("\n");
      } else if (tok[0] == 'p' && !rest.empty() && rest[0] == '=') {
        for (size_t k = 1; k + 1 < rest.size(); k += 2)
          m.bus.writeME0(static_cast<uint16_t>(addr + (k - 1) / 2),
                         static_cast<uint8_t>(std::stoul(rest.substr(k, 2), nullptr, 16)));
      } else if (tok[0] == 'p') {
        for (size_t k = 0; k < rest.size(); k++)
          m.bus.writeME0(static_cast<uint16_t>(addr + k), static_cast<uint8_t>(rest[k]));
      } else {
        std::string have;
        char hex[3];
        for (size_t k = 0; k < rest.size() / 2; k++) {
          std::snprintf(hex, sizeof hex, "%02X", m.bus.readME0(static_cast<uint16_t>(addr + k)));
          have += hex;
        }
        std::string want = rest;
        for (char& ch : want) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        if (have != want) fail(tok, "memory is " + have);
      }
    } else {
      fail(tok, "unknown script token");
    }
  }

  std::printf("%s: %d failure(s), %.2fs, PC=%04X\n", failures ? "FAIL" : "PASS", failures, m.seconds(),
              m.cpu.p());
  return failures ? 1 : 0;
}
