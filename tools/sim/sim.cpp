// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
//
// pc1500sim: runs a bare LH5801 program in a flat 64K of RAM, for SDCC's
// regression suite (sdcc-pc1500's support/regression/ports/lh5801) -- the
// part sim65 or ucsim plays for other targets. No ROM, LCD or keyboard;
// pc1500runner is the tool for programs that need the PC-1500 itself.
//
// usage: pc1500sim [-c CYCLES] <bin> [load hex]
//   bin     a raw image, loaded at the load address (default 0100)
//   -c      give up after CYCLES cycles (default 2000000000), exit status 2
//
// The program starts at the load address with S at FEFFH and a return
// address of FFF8H on the stack. It talks to the simulator through ME1,
// which a real PC-1500 only decodes at F000H-F00FH:
//   write #(FFF0H)  print the byte on stdout
//   write #(FFF1H)  stop; the byte is the exit status
// Returning to FFF8H also stops (status 0); a HLT stops with status 3.
// At the end it prints ucsim's "words read" and "Total time" lines, which
// the suite's get_ticks.py reads for its byte and cycle counts.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "lh5801.h"

namespace {

constexpr uint16_t kPutchar = 0xFFF0;
constexpr uint16_t kExit = 0xFFF1;
constexpr uint16_t kReturn = 0xFFF8;
constexpr uint16_t kStackTop = 0xFF00;

struct FlatBus : lh5801::MemoryBus {
  uint8_t ram[0x10000] = {};
  bool stopped = false;
  int status = 0;

  uint8_t readME0(uint16_t addr) override { return ram[addr]; }
  void writeME0(uint16_t addr, uint8_t value) override { ram[addr] = value; }
  uint8_t readME1(uint16_t) override { return 0xFF; }
  void writeME1(uint16_t addr, uint8_t value) override {
    if (addr == kPutchar) {
      std::fputc(value, stdout);
    } else if (addr == kExit) {
      stopped = true;
      status = value;
    }
  }
};

}  // namespace

int main(int argc, char** argv) {
  long long maxCycles = 2'000'000'000;
  const char* path = nullptr;
  unsigned load = 0x0100;
  for (int i = 1; i < argc; i++) {
    if (std::strcmp(argv[i], "-c") == 0 && i + 1 < argc) maxCycles = std::atoll(argv[++i]);
    else if (!path) path = argv[i];
    else load = std::strtoul(argv[i], nullptr, 16);
  }
  if (!path) {
    std::fprintf(stderr, "usage: pc1500sim [-c CYCLES] <bin> [load hex]\n");
    return 2;
  }
  std::ifstream f(path, std::ios::binary);
  std::vector<uint8_t> bin{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
  if (bin.empty() || load + bin.size() > 0x10000) {
    std::fprintf(stderr, "can't load %s at %04X\n", path, load);
    return 2;
  }

  static FlatBus bus;
  std::memcpy(bus.ram + load, bin.data(), bin.size());
  lh5801::CPU cpu{bus};
  cpu.reset();
  // The return address, as SJP pushes it: low byte at the higher address.
  bus.ram[kStackTop] = kReturn & 0xFF;
  bus.ram[kStackTop - 1] = kReturn >> 8;
  cpu.setS(kStackTop - 2);
  cpu.setP(static_cast<uint16_t>(load));

  long long cycles = 0;
  int status = 0;
  while (true) {
    if (bus.stopped) {
      status = bus.status;
      break;
    }
    if (cpu.p() == kReturn) break;
    if (cpu.halted()) {
      std::printf("\n--- HLT at %04X\n", cpu.p());
      status = 3;
      break;
    }
    if (cycles >= maxCycles) {
      std::printf("\n--- timeout after %lld cycles, PC=%04X\n", cycles, cpu.p());
      status = 2;
      break;
    }
    int c = cpu.step();
    cycles += c > 0 ? c : 1;
  }
  std::printf("\n%zu words read from %s\n", bin.size(), path);
  std::printf("Total time since last reset= %.6f sec (%lld clks)\n", cycles / 1300000.0, cycles);
  return status;
}
