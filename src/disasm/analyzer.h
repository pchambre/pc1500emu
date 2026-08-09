// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>

// Pure static analysis: classifies a ROM image's bytes as code or data by
// recursive-descent traversal from a set of seed entry points, and locates
// BASIC keyword tables (both the base ROM's built-in one and a module
// ROM's own). No CPU/Bus/live execution involved -- see
// ~/Documents/PC1500/PC1500_BASIC_Keyword_Extension_Mechanism.md for the
// keyword-table mechanism this is built on, and opcode_table.h for the
// instruction decoder this drives.
namespace pc1500::disasm {

enum class ByteKind : uint8_t {
  Unknown,           // never reached by traversal -- rendered as generic data
  CodeStart,         // first byte of a decoded instruction
  CodeContinuation,  // a later byte of a decoded instruction
};

// One keyword-table entry: marker + name + code + address, per the
// mechanism doc's §1 -- confirmed (via basic_tokens.cpp's own independently
// transcribed keyword-code table, cross-checked against real ROM1.BIN
// bytes) to be the *same* layout for the base ROM's built-in table and
// every module ROM's own table, address field included in both.
struct KeywordEntry {
  uint16_t markerAddr = 0;
  std::string name;
  uint16_t code = 0;
  uint16_t address = 0;  // the routine's entry point -- seeded as code
};

struct KeywordTable {
  uint16_t indexAddr = 0;  // the 52-byte, 26-entry first-letter index start
  uint16_t tableAddr = 0;  // first entry's marker address
  uint16_t endAddr = 0;    // one past the terminator byte (or last parsed entry, if no terminator found)
  std::vector<KeywordEntry> entries;
};

struct VectorTableEntry {
  uint16_t slot = 0;    // 0xFF00 + index
  uint16_t target = 0;
  const char* name = nullptr;  // non-null for the four fixed MI/Timer/NMI/Reset slots
};

struct AnalysisResult {
  uint16_t base = 0;
  std::vector<ByteKind> kind;    // indexed by (addr - base), size == image size
  std::set<uint16_t> labels;     // every address that needs a label: entry points + branch/call targets
  std::vector<VectorTableEntry> vectorEntries;  // fixed vectors (base mode) + any FF00H-page slots referenced
  KeywordTable baseKeywordTable;                 // valid (non-empty entries) only in base mode
  std::vector<KeywordTable> moduleKeywordTables;  // valid only in module mode
  // Module mode only: for each 0x55-sentinel page where no candidate table
  // reached kMinEntriesForConfidence (analyzer.cpp), the single
  // highest-entry-count candidate found anyway (which may have as few as 1
  // entry) -- lets a caller report "found X but didn't trust it" rather
  // than silently rendering the whole page as data, for small/hand-built
  // module ROMs (e.g. a single-keyword test build) that are real but too
  // short to clear the false-positive-avoidance bar. Not auto-applied
  // (that bar exists for a real reason -- see findKeywordTableInPage's
  // comment); a caller that trusts one of these should pass its entries'
  // addresses back in via analyzeModuleRom's extraSeeds.
  std::vector<KeywordTable> lowConfidenceTables;
};

// Base ROM's built-in keyword table's first-letter index address --
// confirmed against real ROM1.BIN bytes cross-checked with
// src/basic/basic_tokens.cpp's independently-transcribed code table (e.g.
// AREAD/AND/ABS/ATN/ASN/ACS's stored code fields match exactly).
constexpr uint16_t kBaseKeywordIndexAddr = 0xC01E;

// Base-ROM mode: seeds the MI/Timer/NMI/Reset vectors (FFF8H-FFFFH) and the
// built-in keyword table at kBaseKeywordIndexAddr, then traverses. VEJ/VMJ/
// conditional-vector-call targets (FF00H-FFF7H) are resolved lazily, only
// for slots actually referenced by code found during traversal.
// `extraSeeds` are additional entry-point addresses to traverse from and
// label, on top of the above -- for known-good entry points a caller wants
// disassembled that nothing else here would discover (see
// AnalysisResult::lowConfidenceTables).
AnalysisResult analyzeBaseRom(const std::vector<uint8_t>& image, uint16_t base,
                               const std::vector<uint16_t>& extraSeeds = {});

// Module-ROM mode: scans for a 0x55 sentinel byte at every 2KB-aligned page
// boundary within [base, base+image.size()); for each page found, runs a
// validating scan for a keyword-table entry chain (see analyzer.cpp's
// findKeywordTableInPage), seeds every entry's address field, then
// traverses the same way as base mode (minus the base-specific table/
// vectors). `extraSeeds`: see analyzeBaseRom.
AnalysisResult analyzeModuleRom(const std::vector<uint8_t>& image, uint16_t base,
                                 const std::vector<uint16_t>& extraSeeds = {});

// Standalone-program mode: a BASIC-`POKE`d/`CALL`ed ML routine, with none
// of base/module ROM's own structure (no reset/interrupt vectors, no
// keyword table) to auto-seed from -- the only entry points are whatever
// the caller already knows (typically just the `CALL` address, often the
// same as `base` for a routine entered at its own load address). Pure
// traversal from `entryPoints`, nothing else; if `entryPoints` is empty,
// seeds from `base` alone, since that's the common case.
AnalysisResult analyzeProgram(const std::vector<uint8_t>& image, uint16_t base,
                               const std::vector<uint16_t>& entryPoints = {});

}  // namespace pc1500::disasm
