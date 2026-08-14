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
#include "known_symbols.h"
#include "tasm_convert.h"

namespace {

void printUsage(const char* argv0) {
  std::fprintf(
      stderr,
      "Usage: %s [--mode base|module|program] [--base 0xNNNN] [-o out.asm] [--annotate] <romfile>\n"
      "       %s --mode convert [-o out.asm] <tasm.asm>\n"
      "\n"
      "  --mode base    Base PC-1500 ROM (ROM1.BIN): seeds the reset/interrupt\n"
      "                 vectors and the built-in keyword table. Default load\n"
      "                 address 0xC000 (the real ROM1.BIN footprint).\n"
      "  --mode module  Expansion-module ROM: scans for the 0x55 sentinel at\n"
      "                 every 2KB-aligned page and auto-detects its keyword\n"
      "                 table(s). Default load address 0x8000.\n"
      "  --mode program Standalone BASIC-POKEd/CALLed ML routine: no vectors, no\n"
      "                 keyword table -- just traverses from --seed (repeatable),\n"
      "                 defaulting to --base itself if no --seed is given (the\n"
      "                 common case: a routine CALLed at its own load address).\n"
      "                 --base is required (no universal convention to default to).\n"
      "  --mode convert Source-to-source: rewrites a hand-written TASM\n"
      "                 (tasm5801.tab) .asm file to this project's sdas dialect,\n"
      "                 reassemblable by sdaslh5801. <tasm.asm> is TASM source,\n"
      "                 not a ROM/BIN. Ignores --base/--seed/--annotate/\n"
      "                 --dialect/--symbols-file. Scoped to single-.ORG sources\n"
      "                 with no #include/#define/#ifdef preprocessing, .EXPORT,\n"
      "                 or MACRO/ENDM blocks -- see tasm_convert.h for exactly\n"
      "                 what's supported and why. Reports warnings (best-effort\n"
      "                 pass-through) and errors (unsupported construct, exits\n"
      "                 nonzero, no output written) to stderr.\n"
      "  --base 0xNNNN  Override the default load address for the chosen mode.\n",
      argv0, argv0);
  std::fprintf(
      stderr,
      "  --seed 0xNNNN  Traverse from this address too, in addition to whatever\n"
      "                 vectors/keyword tables are found automatically (repeatable).\n"
      "                 Useful for small/hand-built module ROMs with too few\n"
      "                 keyword-table entries to pass the auto-detector's\n"
      "                 false-positive-avoidance confidence check -- pc1500disasm\n"
      "                 reports these to stderr as 'low-confidence' candidates,\n"
      "                 each entry's address is exactly what --seed wants. In\n"
      "                 program mode, --seed is the entry point list itself (see\n"
      "                 above) rather than an addition to auto-detected seeds.\n"
      "  -o out.asm     Write the listing here instead of stdout.\n"
      "  --annotate     Add a trailing '; 0xNNNN: XX XX ...' comment per line.\n"
      "  --dialect sdas|tasm\n"
      "                 Output syntax. sdas (default) is reassemblable by\n"
      "                 sdaslh5801. tasm renders the same decoded instructions in\n"
      "                 the Telemark Assembler (tasm5801.tab) dialect real\n"
      "                 hand-written PC-1500 sources often use instead (uppercase\n"
      "                 mnemonics/registers, '$'-prefixed hex, no .area wrapper) --\n"
      "                 for reading/porting, not reassembly by this project's own\n"
      "                 toolchain. See --mode convert to go the other direction.\n"
      "  --symbols-file <path>\n"
      "                 Extra address annotations (\"; NAME -- comment\", same as the\n"
      "                 built-in table), on top of known_symbols.cpp's own -- lets you\n"
      "                 add your own without a code change/rebuild. One entry per line:\n"
      "                 '<addr> <name> <comment...>' (addr takes an optional 0x prefix,\n"
      "                 always hex; comment is the rest of the line, may contain spaces).\n"
      "                 Blank lines and lines starting with '#' are ignored. A malformed\n"
      "                 line is skipped with a warning; a same-address entry here\n"
      "                 overrides the built-in table. Example line:\n"
      "                   0xE243 KEYSCAN_WAIT scan keyboard, wait for a key\n",
      argv0);
}

std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// Shared by both --mode convert's text output and the byte-disassembly
// modes' listing output.
bool writeOutput(const std::string& text, const std::string& outPath) {
  if (outPath.empty()) {
    std::fwrite(text.data(), 1, text.size(), stdout);
    return true;
  }
  std::ofstream f(outPath, std::ios::binary);
  if (!f) {
    std::fprintf(stderr, "pc1500disasm: could not open '%s' for writing\n", outPath.c_str());
    return false;
  }
  f.write(text.data(), static_cast<std::streamsize>(text.size()));
  std::fprintf(stderr, "pc1500disasm: wrote %s\n", outPath.c_str());
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::string mode = "base";
  bool baseGiven = false;
  uint32_t base = 0;
  std::string outPath;
  bool annotate = false;
  std::string romPath;
  std::vector<uint16_t> seeds;
  std::string symbolsFilePath;
  std::string dialectArg = "sdas";

  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--mode" && i + 1 < argc) {
      mode = argv[++i];
    } else if (arg == "--base" && i + 1 < argc) {
      base = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 16));
      baseGiven = true;
    } else if (arg == "--seed" && i + 1 < argc) {
      seeds.push_back(static_cast<uint16_t>(std::strtoul(argv[++i], nullptr, 16)));
    } else if (arg == "-o" && i + 1 < argc) {
      outPath = argv[++i];
    } else if (arg == "--annotate") {
      annotate = true;
    } else if (arg == "--dialect" && i + 1 < argc) {
      dialectArg = argv[++i];
    } else if (arg == "--symbols-file" && i + 1 < argc) {
      symbolsFilePath = argv[++i];
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

  if (mode == "convert") {
    std::vector<uint8_t> srcBytes = readFile(romPath);
    if (srcBytes.empty()) {
      std::fprintf(stderr, "pc1500disasm: could not read '%s' (or it's empty)\n", romPath.c_str());
      return 1;
    }
    std::string source(srcBytes.begin(), srcBytes.end());
    pc1500::disasm::TasmConvertResult conv = pc1500::disasm::convertTasmToSdas(source);
    for (const auto& w : conv.warnings) {
      std::fprintf(stderr, "pc1500disasm: warning: %s\n", w.c_str());
    }
    if (!conv.ok()) {
      for (const auto& e : conv.errors) {
        std::fprintf(stderr, "pc1500disasm: error: %s\n", e.c_str());
      }
      return 1;
    }
    return writeOutput(conv.output, outPath) ? 0 : 1;
  }

  if (mode != "base" && mode != "module" && mode != "program") {
    std::fprintf(stderr, "pc1500disasm: --mode must be 'base', 'module', 'program', or 'convert'\n");
    return 1;
  }
  if (mode == "program" && !baseGiven) {
    std::fprintf(stderr,
                  "pc1500disasm: --mode program requires --base (no universal load address "
                  "for a standalone ML routine)\n");
    return 1;
  }
  if (!baseGiven) base = (mode == "base") ? 0xC000 : 0x8000;
  if (base > 0xFFFF) {
    std::fprintf(stderr, "pc1500disasm: --base must fit in 16 bits\n");
    return 1;
  }
  pc1500::disasm::AsmDialect dialect;
  if (dialectArg == "sdas") {
    dialect = pc1500::disasm::AsmDialect::Sdas;
  } else if (dialectArg == "tasm") {
    dialect = pc1500::disasm::AsmDialect::Tasm;
  } else {
    std::fprintf(stderr, "pc1500disasm: --dialect must be 'sdas' or 'tasm'\n");
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

  pc1500::disasm::AnalysisResult result;
  if (mode == "base") {
    result = pc1500::disasm::analyzeBaseRom(image, static_cast<uint16_t>(base), seeds);
  } else if (mode == "module") {
    result = pc1500::disasm::analyzeModuleRom(image, static_cast<uint16_t>(base), seeds);
  } else {
    result = pc1500::disasm::analyzeProgram(image, static_cast<uint16_t>(base), seeds);
  }

  for (const auto& kt : result.lowConfidenceTables) {
    std::fprintf(stderr,
                  "pc1500disasm: found a %zu-entry keyword-table candidate at 0x%04X, below the "
                  "auto-detector's confidence threshold (needs 2+) -- not applied automatically:\n",
                  kt.entries.size(), kt.tableAddr);
    for (const auto& e : kt.entries) {
      std::fprintf(stderr, "  %s -> 0x%04X (add with --seed 0x%04X)\n", e.name.c_str(), e.address,
                    e.address);
    }
  }

  pc1500::disasm::FormatOptions options;
  options.annotate = annotate;
  options.dialect = dialect;
  if (!symbolsFilePath.empty()) {
    std::string loadError;
    if (!pc1500::disasm::loadUserSymbolsFile(symbolsFilePath, &options.userSymbols, &loadError)) {
      std::fprintf(stderr, "pc1500disasm: --symbols-file '%s': %s\n", symbolsFilePath.c_str(),
                    loadError.c_str());
      return 1;
    }
  }
  std::string listing = pc1500::disasm::formatListing(image, result, options);
  return writeOutput(listing, outPath) ? 0 : 1;
}
