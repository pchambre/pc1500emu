// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#include "analyzer.h"

#include <deque>

#include "opcode_table.h"

namespace pc1500::disasm {

namespace {

// Bounds-checked byte access relative to an image loaded at `base`.
// Returns -1 for any address outside [base, base+image.size()).
int byteAt(const std::vector<uint8_t>& image, uint16_t base, uint16_t addr) {
  if (addr < base) return -1;
  size_t off = static_cast<size_t>(addr) - base;
  if (off >= image.size()) return -1;
  return image[off];
}

bool inImage(const std::vector<uint8_t>& image, uint16_t base, uint16_t addr) {
  return addr >= base && (static_cast<size_t>(addr) - base) < image.size();
}

// Walks a keyword-table entry chain starting at `tableAddr` (the first
// entry's own marker byte), per the mechanism doc's §1 format: marker (low
// nibble = name length 1-15) + name (ASCII, that many bytes) + code
// (2 bytes, big-endian) + address (2 bytes, big-endian, the routine's entry
// point). Stops at a terminator (marker low nibble 0), an implausible
// marker/name (validated: name must be all uppercase A-Z -- real ROM
// content always is, confirmed against every entry sampled this session),
// or the image boundary. `indexAddr` is recorded as tableAddr-52 (the
// first-letter index the mechanism doc says always immediately precedes
// the table) whether or not the caller actually located it separately.
KeywordTable parseKeywordTableAt(const std::vector<uint8_t>& image, uint16_t base,
                                  uint16_t tableAddr) {
  KeywordTable kt;
  kt.tableAddr = tableAddr;
  kt.indexAddr = static_cast<uint16_t>(tableAddr - 52);
  uint16_t addr = tableAddr;
  while (true) {
    int marker = byteAt(image, base, addr);
    if (marker < 0) break;
    int length = marker & 0x0F;
    if (length == 0) {
      addr += 1;  // terminator byte itself
      break;
    }
    bool namesOk = true;
    std::string name;
    name.reserve(static_cast<size_t>(length));
    for (int i = 0; i < length; i++) {
      // Uppercase letters, plus the trailing type markers real BASIC
      // keyword names use: '$' (string functions/commands -- CHR$, MID$,
      // ...) and '#' (POKE#/PEEK#, confirmed against
      // src/basic/basic_tokens.cpp's own transcribed keyword list). Also
      // allows space: the real base ROM's table has at least one entry
      // between PAUSE and RUN whose name is blank (' '-padded) with a code
      // value (0xF1A3) not present anywhere in basic_tokens.cpp -- an
      // apparently reserved/unused table slot, confirmed by direct byte
      // inspection this session, still structured exactly like a real
      // entry (and so must still be walked over, not treated as the
      // terminator).
      int b = byteAt(image, base, static_cast<uint16_t>(addr + 1 + i));
      bool ok = b >= 0 && ((b >= 'A' && b <= 'Z') || b == '$' || b == '#' || b == ' ');
      if (!ok) {
        namesOk = false;
        break;
      }
      name.push_back(static_cast<char>(b));
    }
    if (!namesOk) break;
    int codeHi = byteAt(image, base, static_cast<uint16_t>(addr + 1 + length));
    int codeLo = byteAt(image, base, static_cast<uint16_t>(addr + 2 + length));
    int addrHi = byteAt(image, base, static_cast<uint16_t>(addr + 3 + length));
    int addrLo = byteAt(image, base, static_cast<uint16_t>(addr + 4 + length));
    if (codeHi < 0 || codeLo < 0 || addrHi < 0 || addrLo < 0) break;
    // Every code field sampled this session (base ROM: 0xF0xx/0xF1xx;
    // modules: 0xE0-0xE7 PV-low, 0xE8/0xF0 PV-high, per the mechanism
    // doc's confirmed dispatch-code ranges) has a high byte >= 0xE0 --
    // rejecting anything lower catches short coincidental "entries" in
    // ordinary code bytes before findKeywordTableInPage's sliding-window
    // scan reaches a real table (confirmed necessary against a real
    // CE-158.ROM dump this session: without this, a 2-byte false-positive
    // run just before the real SETCOM entry satisfied
    // kMinEntriesForConfidence first).
    if (codeHi < 0xE0) break;

    KeywordEntry entry;
    entry.markerAddr = addr;
    entry.name = name;
    entry.code = static_cast<uint16_t>((codeHi << 8) | codeLo);
    entry.address = static_cast<uint16_t>((addrHi << 8) | addrLo);
    kt.entries.push_back(entry);

    addr = static_cast<uint16_t>(addr + 5 + length);  // marker(1)+name+code(2)+address(2)
  }
  kt.endAddr = addr;
  return kt;
}

// Module-ROM keyword-table auto-detection: after a page's 0x55 sentinel is
// found, the table's exact offset within the page varies (real modules
// place module-specific driver code between the sentinel and the table --
// confirmed against the real CE-150.ROM dump, whose two tables sit at very
// different offsets within their own pages), so this slides a candidate
// marker-byte position across the rest of the page and validates each as a
// potential table start by requiring parseKeywordTableAt to yield at least
// kMinEntriesForConfidence consecutive valid entries -- verified this
// session to cleanly and unambiguously locate both of CE-150.ROM's real
// tables with no false positives.
constexpr int kMinEntriesForConfidence = 2;

// Cross-checks a below-confidence candidate (as few as 1 entry) against
// the 52-byte first-letter index that should immediately precede every
// real keyword table (mechanism doc §2): for each entry whose name is 2+
// characters, the index slot for its first letter must be a big-endian
// pointer to the entry's *second* letter (markerAddr + 2) -- exactly the
// doc's own worked examples (CHAIN, MERGE, RMT, ...). A well-formed index
// backing even a single entry is strong independent corroborating
// evidence the whole structure is real (a coincidental run of valid-
// looking code bytes would also need to coincidentally produce a correct
// pointer 52 bytes earlier), so this can safely accept what
// kMinEntriesForConfidence alone would reject -- confirmed against a
// real single-keyword module ROM this session (INVERT_minimal_8000_pvlow.ROM).
// Entries with 1-character names are skipped (no second letter to point
// at) and don't count for or against the check; a candidate with only
// such entries can't be validated this way at all.
bool indexValidatesCandidate(const std::vector<uint8_t>& image, uint16_t base,
                              const KeywordTable& candidate) {
  if (candidate.entries.empty()) return false;
  uint16_t indexAddr = static_cast<uint16_t>(candidate.tableAddr - 52);
  bool checkedAny = false;
  for (const auto& e : candidate.entries) {
    if (e.name.size() < 2) continue;
    checkedAny = true;
    int letterIdx = e.name[0] - 'A';
    if (letterIdx < 0 || letterIdx > 25) return false;
    uint16_t slot = static_cast<uint16_t>(indexAddr + letterIdx * 2);
    int hi = byteAt(image, base, slot);
    int lo = byteAt(image, base, static_cast<uint16_t>(slot + 1));
    if (hi < 0 || lo < 0) return false;
    uint16_t pointer = static_cast<uint16_t>((hi << 8) | lo);
    uint16_t expected = static_cast<uint16_t>(e.markerAddr + 2);
    if (pointer != expected) return false;
  }
  return checkedAny;
}

// `bestRejected`, if non-null, is set to the highest-entry-count candidate
// seen (even a 1-entry one) when no candidate reaches confidence or gets
// index-validated -- left default-constructed (empty entries) if literally
// nothing parsed at all.
bool findKeywordTableInPage(const std::vector<uint8_t>& image, uint16_t base, uint16_t pageStart,
                             uint16_t pageEnd, KeywordTable* out, KeywordTable* bestRejected = nullptr) {
  for (uint16_t candidate = static_cast<uint16_t>(pageStart + 1); candidate < pageEnd; candidate++) {
    KeywordTable kt = parseKeywordTableAt(image, base, candidate);
    if (kt.entries.empty()) continue;
    if (static_cast<int>(kt.entries.size()) >= kMinEntriesForConfidence ||
        indexValidatesCandidate(image, base, kt)) {
      *out = kt;
      return true;
    }
    if (bestRejected != nullptr && kt.entries.size() > bestRejected->entries.size()) {
      *bestRejected = kt;
    }
  }
  return false;
}

// The traversal worklist item: an address to decode as the start of an
// instruction.
void traverse(const std::vector<uint8_t>& image, uint16_t base, std::vector<ByteKind>& kind,
              std::set<uint16_t>& labels, std::vector<VectorTableEntry>& vectorEntries,
              std::deque<uint16_t> worklist) {
  while (!worklist.empty()) {
    uint16_t addr = worklist.front();
    worklist.pop_front();
    if (!inImage(image, base, addr)) continue;
    ByteKind& here = kind[static_cast<size_t>(addr) - base];
    if (here != ByteKind::Unknown) continue;  // already decoded from here, or claimed mid-instruction

    size_t avail = image.size() - (static_cast<size_t>(addr) - base);
    DecodedInstruction d = decodeOne(&image[static_cast<size_t>(addr) - base], avail, addr);
    if (!d.valid) continue;  // undefined opcode or truncated -- likely wandered into data, stop here

    here = ByteKind::CodeStart;
    for (int i = 1; i < d.length; i++) {
      uint16_t a = static_cast<uint16_t>(addr + i);
      if (!inImage(image, base, a)) break;
      ByteKind& k = kind[static_cast<size_t>(a) - base];
      if (k == ByteKind::Unknown) k = ByteKind::CodeContinuation;
    }

    uint16_t afterInstr = static_cast<uint16_t>(addr + d.length);
    switch (d.flow) {
      case ControlFlow::Jump:
        labels.insert(d.branchTarget);
        worklist.push_back(d.branchTarget);
        break;
      case ControlFlow::ConditionalBranch:
        labels.insert(d.branchTarget);
        worklist.push_back(d.branchTarget);
        worklist.push_back(afterInstr);
        break;
      case ControlFlow::Call:
        if (d.isVectorCall) {
          int hi = byteAt(image, base, d.vectorSlot);
          int lo = byteAt(image, base, static_cast<uint16_t>(d.vectorSlot + 1));
          if (hi >= 0 && lo >= 0) {
            uint16_t target = static_cast<uint16_t>((hi << 8) | lo);
            vectorEntries.push_back({d.vectorSlot, target, nullptr});
            labels.insert(target);
            worklist.push_back(target);
          }
        } else {
          labels.insert(d.branchTarget);
          worklist.push_back(d.branchTarget);
        }
        worklist.push_back(afterInstr);
        break;
      case ControlFlow::Return:
        break;  // no fall-through, no target
      case ControlFlow::Halt:
      case ControlFlow::Sequential:
        worklist.push_back(afterInstr);
        break;
    }
  }
}

}  // namespace

AnalysisResult analyzeBaseRom(const std::vector<uint8_t>& image, uint16_t base,
                               const std::vector<uint16_t>& extraSeeds) {
  AnalysisResult r;
  r.base = base;
  r.kind.assign(image.size(), ByteKind::Unknown);
  std::deque<uint16_t> worklist;
  for (uint16_t seed : extraSeeds) {
    r.labels.insert(seed);
    worklist.push_back(seed);
  }

  // MI / Timer / NMI / Reset vectors, per lh5801.cpp's dispatchInterrupt
  // (FFF8H=MI, FFFAH=Timer, FFFCH=NMI, FFFEH=Reset), 2 bytes each,
  // big-endian.
  const struct { uint16_t slot; const char* name; } kFixedVectors[] = {
      {0xFFF8, "MI"}, {0xFFFA, "Timer"}, {0xFFFC, "NMI"}, {0xFFFE, "Reset"},
  };
  for (const auto& v : kFixedVectors) {
    int hi = byteAt(image, base, v.slot);
    int lo = byteAt(image, base, static_cast<uint16_t>(v.slot + 1));
    if (hi < 0 || lo < 0) continue;
    uint16_t target = static_cast<uint16_t>((hi << 8) | lo);
    r.vectorEntries.push_back({v.slot, target, v.name});
    r.labels.insert(target);
    worklist.push_back(target);
  }

  // Built-in keyword table -- parsed for documentation/rendering; every
  // entry's address field is also a real code entry point (see
  // analyzer.h's kBaseKeywordIndexAddr comment for how this was confirmed
  // against basic_tokens.cpp). The table itself doesn't start at exactly
  // indexAddr+52 the way the mechanism doc's own phrasing ("immediately
  // before") might suggest -- confirmed against real ROM1.BIN bytes there
  // are 2 pad bytes between the index's end and AREAD's own marker -- so
  // this scans a small window past the index for the first entry chain,
  // the same validating approach findKeywordTableInPage uses for modules,
  // rather than trusting a fixed offset.
  if (inImage(image, base, kBaseKeywordIndexAddr)) {
    uint16_t searchStart = static_cast<uint16_t>(kBaseKeywordIndexAddr + 52);
    uint16_t searchEnd = static_cast<uint16_t>(searchStart + 16);
    findKeywordTableInPage(image, base, static_cast<uint16_t>(searchStart - 1), searchEnd,
                            &r.baseKeywordTable);
    r.baseKeywordTable.indexAddr = kBaseKeywordIndexAddr;
    for (const auto& e : r.baseKeywordTable.entries) {
      r.labels.insert(e.address);
      worklist.push_back(e.address);
    }
  }

  traverse(image, base, r.kind, r.labels, r.vectorEntries, worklist);
  return r;
}

AnalysisResult analyzeModuleRom(const std::vector<uint8_t>& image, uint16_t base,
                                 const std::vector<uint16_t>& extraSeeds) {
  AnalysisResult r;
  r.base = base;
  r.kind.assign(image.size(), ByteKind::Unknown);
  std::deque<uint16_t> worklist;
  for (uint16_t seed : extraSeeds) {
    r.labels.insert(seed);
    worklist.push_back(seed);
  }

  constexpr uint16_t kPageSize = 0x800;  // 2KB
  uint16_t pageStart = static_cast<uint16_t>(base & ~(kPageSize - 1));
  if (pageStart < base) pageStart = static_cast<uint16_t>(pageStart + kPageSize);
  for (uint16_t page = pageStart; page < base + image.size(); page = static_cast<uint16_t>(page + kPageSize)) {
    if (byteAt(image, base, page) != 0x55) continue;
    uint16_t pageEnd = static_cast<uint16_t>(page + kPageSize);
    if (pageEnd > base + image.size()) pageEnd = static_cast<uint16_t>(base + image.size());
    KeywordTable kt;
    KeywordTable rejected;
    if (!findKeywordTableInPage(image, base, page, pageEnd, &kt, &rejected)) {
      if (!rejected.entries.empty()) r.lowConfidenceTables.push_back(std::move(rejected));
      continue;
    }
    for (const auto& e : kt.entries) {
      r.labels.insert(e.address);
      worklist.push_back(e.address);
    }
    r.moduleKeywordTables.push_back(std::move(kt));
  }

  traverse(image, base, r.kind, r.labels, r.vectorEntries, worklist);
  return r;
}

}  // namespace pc1500::disasm
