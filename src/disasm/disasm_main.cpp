// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
//
// CLI for the LH5801/PC-1500 ROM disassembler. See README.md's
// disassembler section (or the plan this was built from) for the two
// modes' entry-point-seeding strategy.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "analyzer.h"
#include "formatter.h"

namespace {

void printUsage(const char* argv0) {
  std::fprintf(stderr,
               "Usage: %s [--mode base|module] [--base 0xNNNN] [-o out.asm] [--annotate] <romfile>\n"
               "\n"
               "  --mode base    Base PC-1500 ROM (ROM1.BIN): seeds the reset/interrupt\n"
               "                 vectors and the built-in keyword table. Default load\n"
               "                 address 0xC000 (the real ROM1.BIN footprint).\n"
               "  --mode module  Expansion-module ROM: scans for the 0x55 sentinel at\n"
               "                 every 2KB-aligned page and auto-detects its keyword\n"
               "                 table(s). Default load address 0x8000.\n"
               "  --base 0xNNNN  Override the default load address for the chosen mode.\n"
               "  -o out.asm     Write the listing here instead of stdout.\n"
               "  --annotate     Add a trailing '; 0xNNNN: XX XX ...' comment per line.\n",
               argv0);
}

std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

}  // namespace

int main(int argc, char** argv) {
  std::string mode = "base";
  bool baseGiven = false;
  uint32_t base = 0;
  std::string outPath;
  bool annotate = false;
  std::string romPath;

  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--mode" && i + 1 < argc) {
      mode = argv[++i];
    } else if (arg == "--base" && i + 1 < argc) {
      base = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 16));
      baseGiven = true;
    } else if (arg == "-o" && i + 1 < argc) {
      outPath = argv[++i];
    } else if (arg == "--annotate") {
      annotate = true;
    } else if (arg == "-h" || arg == "--help") {
      printUsage(argv[0]);
      return 0;
    } else if (!arg.empty() && arg[0] != '-') {
      romPath = arg;
    } else {
      std::fprintf(stderr, "pc1500disasm: unrecognized argument '%s'\n", arg.c_str());
      printUsage(argv[0]);
      return 1;
    }
  }

  if (romPath.empty()) {
    printUsage(argv[0]);
    return 1;
  }
  if (mode != "base" && mode != "module") {
    std::fprintf(stderr, "pc1500disasm: --mode must be 'base' or 'module'\n");
    return 1;
  }
  if (!baseGiven) base = (mode == "base") ? 0xC000 : 0x8000;
  if (base > 0xFFFF) {
    std::fprintf(stderr, "pc1500disasm: --base must fit in 16 bits\n");
    return 1;
  }

  std::vector<uint8_t> image = readFile(romPath);
  if (image.empty()) {
    std::fprintf(stderr, "pc1500disasm: could not read '%s' (or it's empty)\n", romPath.c_str());
    return 1;
  }
  if (base + image.size() > 0x10000) {
    std::fprintf(stderr, "pc1500disasm: '%s' (%zu bytes) doesn't fit at base 0x%04X\n",
                 romPath.c_str(), image.size(), base);
    return 1;
  }

  pc1500::disasm::AnalysisResult result =
      (mode == "base") ? pc1500::disasm::analyzeBaseRom(image, static_cast<uint16_t>(base))
                        : pc1500::disasm::analyzeModuleRom(image, static_cast<uint16_t>(base));

  pc1500::disasm::FormatOptions options;
  options.annotate = annotate;
  std::string listing = pc1500::disasm::formatListing(image, result, options);

  if (outPath.empty()) {
    std::fwrite(listing.data(), 1, listing.size(), stdout);
  } else {
    std::ofstream f(outPath, std::ios::binary);
    if (!f) {
      std::fprintf(stderr, "pc1500disasm: could not open '%s' for writing\n", outPath.c_str());
      return 1;
    }
    f.write(listing.data(), static_cast<std::streamsize>(listing.size()));
    std::fprintf(stderr, "pc1500disasm: wrote %s\n", outPath.c_str());
  }
  return 0;
}
