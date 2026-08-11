// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#include "formatter.h"

#include <algorithm>
#include <cstdio>
#include <optional>
#include <sstream>
#include <unordered_map>

#include "known_symbols.h"
#include "opcode_table.h"

namespace pc1500::disasm {

namespace {

// Maps a keyword routine's entry-point address back to the keyword
// name(s) whose table entry points there, e.g. E86AH -> {"WAIT"} -- so a
// label at that address can be annotated as the known implementation of
// that keyword, the same way known_symbols.cpp documents hardware
// addresses. Built once per formatListing call from every keyword table
// in the AnalysisResult (base ROM's built-in table, plus any module
// tables); a vector rather than a single name since nothing rules out two
// keywords sharing one entry point (e.g. a module keyword table entry
// that happens to alias a base ROM routine).
using KeywordAddrMap = std::unordered_map<uint16_t, std::vector<std::string>>;

KeywordAddrMap buildKeywordAddrMap(const AnalysisResult& result) {
  KeywordAddrMap map;
  auto addTable = [&](const KeywordTable& kt) {
    for (const auto& e : kt.entries) map[e.address].push_back(e.name);
  };
  addTable(result.baseKeywordTable);
  for (const auto& kt : result.moduleKeywordTables) addTable(kt);
  return map;
}

// Renders "WAIT" as "; WAIT keyword", or "WAIT, ATN" (comma-joined) in the
// rare case multiple keywords share one entry point -- empty string if
// `addr` isn't a keyword's own entry point.
std::string keywordComment(uint16_t addr, const KeywordAddrMap& keywordAddrs) {
  auto it = keywordAddrs.find(addr);
  if (it == keywordAddrs.end()) return "";
  std::string out = "  ; ";
  for (size_t i = 0; i < it->second.size(); i++) {
    if (i) out += ", ";
    out += it->second[i];
  }
  out += (it->second.size() == 1) ? " keyword" : " keywords";
  return out;
}

std::string hex2(uint32_t v) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "0x%02X", v & 0xFF);
  return buf;
}
std::string hex4(uint32_t v) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "0x%04X", v & 0xFFFF);
  return buf;
}
std::string label(uint16_t addr) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "L%04X", addr);
  return buf;
}

// Context-free operand rendering -- register names, memory-indirect
// syntax, and bare hex values. Target-address operands (Branch8, and the
// Imm16 used by JMP/SJP) are handled separately in renderInstruction,
// since only there is it known whether the value should render as a label.
std::string renderOperand(Operand kind, uint32_t value) {
  switch (kind) {
    case Operand::None: return "";
    case Operand::RegXL: return "xl"; case Operand::RegYL: return "yl"; case Operand::RegUL: return "ul";
    case Operand::RegXH: return "xh"; case Operand::RegYH: return "yh"; case Operand::RegUH: return "uh";
    case Operand::RegX: return "x"; case Operand::RegY: return "y"; case Operand::RegU: return "u";
    case Operand::RegA: return "a"; case Operand::RegS: return "s"; case Operand::RegP: return "p";
    case Operand::Me0IndX: return "(x)"; case Operand::Me0IndY: return "(y)"; case Operand::Me0IndU: return "(u)";
    case Operand::Me1IndX: return "#(x)"; case Operand::Me1IndY: return "#(y)"; case Operand::Me1IndU: return "#(u)";
    case Operand::Me0Abs: return "(" + hex4(value) + ")";
    case Operand::Me1Abs: return "#(" + hex4(value) + ")";
    case Operand::Imm8: return hex2(value);
    case Operand::Imm16: return hex4(value);
    case Operand::VejSelf: return hex2(value);
    case Operand::VecIdx8: return hex2(value);
    case Operand::Branch8: return "?";  // never reached -- see renderInstruction
  }
  return "?";
}

// True when this operand slot's value is a control-transfer target that
// should render as a label (Ln nnnn) rather than a bare literal: any
// Branch8 slot (BCH/BCS/.../LOP), or the Imm16 slot on JMP/SJP
// specifically (NOT on LDI S,imm16, which is a genuine 16-bit literal).
bool isTargetSlot(const DecodedInstruction& d, Operand slotKind) {
  if (d.isVectorCall) return false;  // VEJ/VMJ/etc render their own operand as bare hex, never a label
  if (slotKind == Operand::Branch8) return true;
  if (slotKind == Operand::Imm16 &&
      (d.flow == ControlFlow::Jump || d.flow == ControlFlow::Call)) {
    return true;
  }
  return false;
}

std::string renderInstruction(const DecodedInstruction& d) {
  std::string text = d.mnemonic;
  if (d.op1 != Operand::None) {
    text += " " + (isTargetSlot(d, d.op1) ? label(d.branchTarget) : renderOperand(d.op1, d.value1));
    if (d.op2 != Operand::None) {
      text += "," + (isTargetSlot(d, d.op2) ? label(d.branchTarget) : renderOperand(d.op2, d.value2));
    }
  }
  return text;
}

// Known-address documentation, always on (unlike --annotate's raw byte
// dump) -- see known_symbols.h. Only op1 is checked: every two-operand
// form in the opcode table (adi/ani/ori/bii/cpi/eai/ldi/lop) has an
// immediate, never an address, in op2. `userSymbols` (a --symbols-file's
// contents, empty if none was given) is checked ahead of the built-in
// table -- see lookupSymbol's own comment.
//
// Besides a direct Me0Abs/Me1Abs memory reference (e.g. LDA (addr)), a
// branch/call target (Branch8, or JMP/SJP's Imm16 -- see isTargetSlot,
// above) is checked too, via d.branchTarget rather than d.value1 -- same
// distinction renderInstruction already makes when deciding whether to
// render an operand as a label. Without this, an SJP to a known ROM
// routine (e.g. E243H) would never get annotated at all, since the
// routine's own address never appears as a labeled line in a small
// program-mode image that doesn't include E243H's bytes.
//
// Also checks describeBits (known_symbols.h) when op2 is an immediate
// (the mask a bii/ani/ori-style instruction tests/sets/clears) against a
// Me0Abs/Me1Abs op1 -- so e.g. `bii (0x764E),0x02` shows "[bit: SHIFT]"
// alongside (or instead of, if the address itself has no KnownSymbol
// entry) the byte-level STATUS1 annotation, rather than every instruction
// touching that byte looking identical regardless of which flag it
// actually cares about.
std::string symbolComment(const DecodedInstruction& d, const std::vector<UserSymbol>& userSymbols) {
  std::optional<ResolvedSymbol> sym;
  std::string bits;
  if (d.op1 == Operand::Me0Abs) {
    uint16_t addr = static_cast<uint16_t>(d.value1);
    sym = lookupSymbol(addr, /*me1=*/false, userSymbols);
    if (d.op2 == Operand::Imm8) bits = describeBits(addr, /*me1=*/false, static_cast<uint8_t>(d.value2));
  } else if (d.op1 == Operand::Me1Abs) {
    uint16_t addr = static_cast<uint16_t>(d.value1);
    sym = lookupSymbol(addr, /*me1=*/true, userSymbols);
    if (d.op2 == Operand::Imm8) bits = describeBits(addr, /*me1=*/true, static_cast<uint8_t>(d.value2));
  } else if (isTargetSlot(d, d.op1)) {
    sym = lookupSymbol(d.branchTarget, /*me1=*/false, userSymbols);
  }
  if (!sym && bits.empty()) return "";
  std::string out = "  ; ";
  if (sym) out += sym->name + " -- " + sym->comment;
  if (!bits.empty()) {
    if (sym) out += " ";
    out += "[bit: " + bits + "]";
  }
  return out;
}

// A label line, with a known-address comment and/or keyword back-link
// appended when applicable (documentation only -- the label text itself
// is always L<hex>, never renamed, so it stays a guaranteed-valid,
// guaranteed-unique sdas identifier for reassembly). Both can appear
// together (e.g. a keyword entry point that's also a documented hardware
// touch-point) -- they're independent annotations, not alternatives.
std::string labelLine(uint16_t addr, const std::vector<UserSymbol>& userSymbols,
                       const KeywordAddrMap& keywordAddrs) {
  std::string line = label(addr) + ":";
  line += keywordComment(addr, keywordAddrs);
  std::optional<ResolvedSymbol> sym = lookupSymbol(addr, /*me1=*/false, userSymbols);
  if (sym) {
    line += "  ; ";
    line += sym->name;
    line += " -- ";
    line += sym->comment;
  }
  line += "\n";
  return line;
}

bool isPrintable(uint8_t b) { return b >= 0x20 && b <= 0x7E; }

std::string escapeAscii(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}

// A pre-rendered span of output text covering [start, end) of the image,
// used for keyword-table and vector-table regions that need structured
// rendering instead of the generic code/data walk below.
struct SpecialRegion {
  uint16_t start;
  uint16_t end;  // exclusive
  std::string text;
};

void appendAnnotation(std::ostringstream& out, const FormatOptions& opts,
                       const std::vector<uint8_t>& image, uint16_t base, uint16_t addr, int length) {
  if (!opts.annotate) return;
  out << " ; " << hex4(addr) << ":";
  for (int i = 0; i < length; i++) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), " %02X", image[static_cast<size_t>(addr) + i - base]);
    out << buf;
  }
}

std::string renderKeywordTable(const std::vector<uint8_t>& image, uint16_t base,
                                const KeywordTable& kt, const std::vector<UserSymbol>& userSymbols,
                                const KeywordAddrMap& keywordAddrs) {
  std::ostringstream out;
  bool haveIndex = kt.indexAddr >= base &&
                    (static_cast<size_t>(kt.indexAddr) + 52) <= base + image.size();
  if (haveIndex) {
    out << "; first-letter index\n";
    out << labelLine(kt.indexAddr, userSymbols, keywordAddrs);
    for (int letter = 0; letter < 26; letter++) {
      uint16_t addr = static_cast<uint16_t>(kt.indexAddr + letter * 2);
      uint16_t v = static_cast<uint16_t>((image[addr - base] << 8) | image[addr + 1 - base]);
      out << "\t.dw " << hex4(v) << "  ; " << static_cast<char>('A' + letter) << "\n";
    }
  }
  out << "; keyword table (marker/name/code/address per entry)\n";
  for (const auto& e : kt.entries) {
    out << labelLine(e.markerAddr, userSymbols, keywordAddrs);
    out << "\t.db " << hex2((e.markerAddr < base ? 0 : image[e.markerAddr - base])) << "  ; marker (len="
        << e.name.size() << ")\n";
    out << "\t.ascii \"" << escapeAscii(e.name) << "\"\n";
    out << "\t.dw " << hex4(e.code) << "  ; code\n";
    out << "\t.dw " << label(e.address) << "  ; address\n";
  }
  // Terminator byte, if it's within range (endAddr-1).
  if (kt.endAddr > base && (static_cast<size_t>(kt.endAddr) - 1 - base) < image.size()) {
    out << "\t.db " << hex2(image[kt.endAddr - 1 - base]) << "  ; table terminator\n";
  }
  return out.str();
}

}  // namespace

std::string formatListing(const std::vector<uint8_t>& image, const AnalysisResult& result,
                           const FormatOptions& options) {
  std::ostringstream out;
  out << "\t.area CODE (ABS)\n";
  out << "\t.org " << hex4(result.base) << "\n\n";

  const KeywordAddrMap keywordAddrs = buildKeywordAddrMap(result);

  std::vector<SpecialRegion> specials;
  auto addKeywordTableRegion = [&](const KeywordTable& kt) {
    if (kt.entries.empty()) return;
    uint16_t start = (kt.indexAddr >= result.base &&
                       (static_cast<size_t>(kt.indexAddr) + 52) <= result.base + image.size())
                          ? kt.indexAddr
                          : kt.tableAddr;
    specials.push_back({start, kt.endAddr,
                         renderKeywordTable(image, result.base, kt, options.userSymbols, keywordAddrs)});
  };
  addKeywordTableRegion(result.baseKeywordTable);
  for (const auto& kt : result.moduleKeywordTables) addKeywordTableRegion(kt);

  for (const auto& v : result.vectorEntries) {
    std::ostringstream vout;
    vout << label(v.slot) << ":\n\t.dw " << label(v.target) << "  ; ";
    if (v.name != nullptr) {
      vout << v.name << " vector\n";
    } else {
      vout << "vector slot " << hex4(v.slot) << "\n";
    }
    specials.push_back({v.slot, static_cast<uint16_t>(v.slot + 2), vout.str()});
  }

  std::sort(specials.begin(), specials.end(),
            [](const SpecialRegion& a, const SpecialRegion& b) { return a.start < b.start; });
  // Drop duplicates/overlaps by start address (e.g. a VMJ/VCS-style
  // conditional vector call whose fetched index byte happens to collide
  // with one of the four fixed MI/Timer/NMI/Reset vector slots) -- keeping
  // more than one region claiming the same start address would otherwise
  // strand the second one behind the first's already-consumed bytes and
  // stall the main loop below.
  specials.erase(std::unique(specials.begin(), specials.end(),
                              [](const SpecialRegion& a, const SpecialRegion& b) {
                                return a.start == b.start;
                              }),
                  specials.end());

  // Driven by a byte OFFSET into `image` (always in [0, image.size()]),
  // not an absolute 16-bit address -- a real base-mode ROM fills exactly
  // to 0x10000 (e.g. 16KB at 0xC000), which would silently wrap a
  // uint16_t "end" address to 0 and terminate the loop immediately.
  // Addresses are only reconstructed (base + offset, safe since
  // disasm_main.cpp already validated base+image.size() <= 0x10000) at the
  // points that actually need one.
  size_t specialIdx = 0;
  size_t offset = 0;
  const size_t total = image.size();

  while (offset < total) {
    uint16_t addr = static_cast<uint16_t>(result.base + offset);

    // Skip any special region we've already passed (its start is behind
    // the current offset) -- can happen when one region's own bytes
    // overlap into where another was found to start.
    while (specialIdx < specials.size() &&
           static_cast<size_t>(specials[specialIdx].start) < static_cast<size_t>(addr)) {
      specialIdx++;
    }

    if (specialIdx < specials.size() && specials[specialIdx].start == addr) {
      out << specials[specialIdx].text;
      size_t regionLen = static_cast<size_t>(specials[specialIdx].end) - specials[specialIdx].start;
      specialIdx++;
      offset += (regionLen == 0) ? 1 : regionLen;  // never stall on a malformed 0-width region
      continue;
    }

    if (result.kind[offset] == ByteKind::CodeStart) {
      size_t avail = total - offset;
      DecodedInstruction d = decodeOne(&image[offset], avail, addr);
      // A known symbol forces its own label line even with no incoming
      // jump/call in this particular ROM image (e.g. E2AAH, the idle
      // prompt, is normally reached only by falling through from the
      // preceding HLT-wake code, never jumped to directly) -- otherwise a
      // real, meaningful address could go entirely unlabeled.
      if (result.labels.count(addr) || keywordAddrs.count(addr) ||
          lookupSymbol(addr, /*me1=*/false, options.userSymbols).has_value()) {
        out << labelLine(addr, options.userSymbols, keywordAddrs);
      }
      out << "\t" << renderInstruction(d) << symbolComment(d, options.userSymbols);
      appendAnnotation(out, options, image, result.base, addr, d.length);
      out << "\n";
      offset += static_cast<size_t>(d.length);
      continue;
    }

    // Data: collect a contiguous run up to the next special region, label,
    // or CodeStart boundary.
    size_t runStart = offset;
    size_t limit = (specialIdx < specials.size())
                       ? (static_cast<size_t>(specials[specialIdx].start) - result.base)
                       : total;
    while (offset < limit && result.kind[offset] != ByteKind::CodeStart) {
      // Stop early at a label too, even mid-run, so every labeled address
      // starts its own line.
      if (offset != runStart && result.labels.count(static_cast<uint16_t>(result.base + offset))) break;
      offset++;
    }
    if (offset == runStart) offset++;  // hard safety net: always make progress
    size_t runLen = offset - runStart;
    uint16_t runStartAddr = static_cast<uint16_t>(result.base + runStart);

    bool allPrintable = runLen >= 4;
    for (size_t i = 0; allPrintable && i < runLen; i++) {
      if (!isPrintable(image[runStart + i])) allPrintable = false;
    }
    if (result.labels.count(runStartAddr) || keywordAddrs.count(runStartAddr) ||
        lookupSymbol(runStartAddr, /*me1=*/false, options.userSymbols).has_value()) {
      out << labelLine(runStartAddr, options.userSymbols, keywordAddrs);
    }
    if (allPrintable) {
      std::string text(reinterpret_cast<const char*>(&image[runStart]), runLen);
      out << "\t.ascii \"" << escapeAscii(text) << "\"";
      appendAnnotation(out, options, image, result.base, runStartAddr, static_cast<int>(runLen));
      out << "\n";
    } else {
      for (size_t i = 0; i < runLen; i += 16) {
        size_t lineLen = std::min<size_t>(16, runLen - i);
        out << "\t.db ";
        for (size_t j = 0; j < lineLen; j++) {
          if (j) out << ", ";
          out << hex2(image[runStart + i + j]);
        }
        appendAnnotation(out, options, image, result.base, static_cast<uint16_t>(runStartAddr + i),
                          static_cast<int>(lineLen));
        out << "\n";
      }
    }
  }

  return out.str();
}

}  // namespace pc1500::disasm
