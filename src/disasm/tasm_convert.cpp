// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#include "tasm_convert.h"

#include <cctype>
#include <cstdint>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "opcode_table.h"

namespace pc1500::disasm {

namespace {

std::string toLower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
std::string toUpper(std::string s) {
  for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}
std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

// Every mnemonic decodeOne can produce, gathered directly from the real
// decoder (opcode_table.cpp's own tables, one probe per possible opcode
// byte and FD-prefixed sub-opcode byte) rather than a separately
// maintained list -- so this can never drift from what this project's own
// disassembler/sdaslh5801 actually recognize.
const std::unordered_set<std::string>& mnemonicSet() {
  static const std::unordered_set<std::string> set = [] {
    std::unordered_set<std::string> s;
    uint8_t buf[8] = {};
    for (int op = 0; op < 256; op++) {
      buf[0] = static_cast<uint8_t>(op);
      DecodedInstruction d = decodeOne(buf, sizeof(buf), 0);
      if (d.valid) s.insert(d.mnemonic);
    }
    for (int op = 0; op < 256; op++) {
      buf[0] = 0xFD;
      buf[1] = static_cast<uint8_t>(op);
      DecodedInstruction d = decodeOne(buf, sizeof(buf), 0);
      if (d.valid) s.insert(d.mnemonic);
    }
    return s;
  }();
  return set;
}

// The LH5801's fixed register-name set -- not derived from decodeOne (its
// operand kinds aren't strings), but this is the complete architectural
// register file per the datasheet, unlikely to ever need updating.
const std::unordered_set<std::string>& registerSet() {
  static const std::unordered_set<std::string> set = {"a",  "s",  "p",  "x",  "y",  "u",
                                                        "xh", "xl", "yh", "yl", "uh", "ul"};
  return set;
}

// TASM mnemonics that name the same LH5801 opcode as this project's own
// (Sharp-datasheet-derived) mnemonic table, but spelled differently --
// evidently borrowed from Z80/8080 naming for programmer familiarity.
// Confirmed against two independent real TASM PC-1500 sources: CALL/RET
// for SJP/RTN (github.com/Jeff-Birt/Sharp_CE-158's ROM disassembly -- SJP/
// RTN still dominate there, ~163/66 uses vs. 6/4, so this alias is a real
// but occasional substitution, not the norm) and SCF for SEC (a hand-
// written memory-test program). Neither is a case-fold -- see
// tasm_convert.h's own comment.
const std::unordered_map<std::string, std::string>& mnemonicAliasMap() {
  static const std::unordered_map<std::string, std::string> map = {
      {"call", "sjp"},
      {"ret", "rtn"},
      {"scf", "sec"},
  };
  return map;
}

// TASM directive name (no leading dot, uppercased) -> sdas spelling. BYTE/
// WORD/TEXT are the spellings a real hand-written TASM PC-1500 ROM
// disassembly used; DB/DW/ASCII are what the simpler sample this was built
// against (a memory-test program) used instead -- see tasm_convert.h.
const std::unordered_map<std::string, std::string>& directiveMap() {
  static const std::unordered_map<std::string, std::string> map = {
      {"EQU", ".equ"}, {"ORG", ".org"}, {"DB", ".db"},       {"BYTE", ".db"},
      {"DW", ".dw"},   {"WORD", ".dw"}, {"ASCII", ".ascii"}, {"TEXT", ".ascii"},
  };
  return map;
}

bool isIdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// Rewrites every "$XX"/"$XXXX" hex literal in `s` to "0xXX"/"0xXXXX". Safe
// to apply to a whole operand string unconditionally: TASM identifiers are
// alnum/underscore only, so "$" never appears except as this prefix.
std::string rewriteHexLiterals(const std::string& s) {
  static const std::regex hexLit(R"(\$([0-9A-Fa-f]+))");
  return std::regex_replace(s, hexLit, "0x$1");
}

// Lowercases any whole-word, case-insensitive match against the fixed
// LH5801 register-name set (word-boundary matched by hand, so e.g. a
// label like "ERR_FLAG" or "AH" can't accidentally match "A"/"AH" as a
// register operand).
std::string rewriteRegisters(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    if (isIdentChar(s[i]) && (i == 0 || !isIdentChar(s[i - 1]))) {
      size_t j = i;
      while (j < s.size() && isIdentChar(s[j])) j++;
      std::string word = s.substr(i, j - i);
      std::string lower = toLower(word);
      out += registerSet().count(lower) ? lower : word;
      i = j;
    } else {
      out += s[i];
      i++;
    }
  }
  return out;
}

class Converter {
 public:
  TasmConvertResult run(const std::string& source) {
    std::ostringstream out;
    std::istringstream in(source);
    std::string rawLine;
    static const std::regex labelRe(R"(^([A-Za-z_][A-Za-z0-9_]*)\s*:\s*(.*)$)");
    static const std::regex equRe(R"(^([A-Za-z_][A-Za-z0-9_]*)\s+\.?[Ee][Qq][Uu]\s+(.+)$)");
    static const std::regex equAfterColonRe(R"(^\.?[Ee][Qq][Uu]\b.*$)");

    while (std::getline(in, rawLine)) {
      lineNo_++;
      std::string codePart, commentPart;
      splitComment(rawLine, &codePart, &commentPart);
      std::string trimmedCode = trim(codePart);
      std::string trimmedComment = trim(commentPart);

      if (trimmedCode.empty()) {
        out << trimmedComment << "\n";
        continue;
      }
      if (trimmedCode[0] == '#') {
        err("preprocessor directive not supported: " + trimmedCode);
        continue;
      }
      if (isMacroBlockLine(trimmedCode)) {
        err("TASM macro definitions (MACRO/ENDM) are not supported: " + trimmedCode);
        continue;
      }

      std::string commentSuffix = trimmedComment.empty() ? "" : ("  " + trimmedComment);

      std::smatch m;
      if (std::regex_match(trimmedCode, m, labelRe)) {
        std::string labelName = m[1].str();
        std::string rest = trim(m[2].str());
        if (rest.empty()) {
          out << labelName << ":" << commentSuffix << "\n";
        } else if (std::regex_match(rest, equAfterColonRe)) {
          err("labeled .EQU ('" + labelName + ": " + rest +
              "') is ambiguous -- write it as '" + labelName + " .EQU value' (no colon)");
        } else {
          out << labelName << ":\n";
          convertInstructionLine(rest, commentSuffix, out);
        }
        continue;
      }

      if (std::regex_match(trimmedCode, m, equRe)) {
        std::string name = m[1].str();
        std::string value = trim(m[2].str());
        out << name << " .equ " << rewriteRegisters(rewriteHexLiterals(value)) << commentSuffix << "\n";
        continue;
      }

      convertInstructionLine(trimmedCode, commentSuffix, out);
    }

    TasmConvertResult result;
    result.output = out.str();
    result.warnings = warnings_;
    result.errors = errors_;
    return result;
  }

 private:
  int lineNo_ = 0;
  bool sawOrg_ = false;
  std::vector<std::string> warnings_;
  std::vector<std::string> errors_;

  void warn(const std::string& msg) { warnings_.push_back("line " + std::to_string(lineNo_) + ": " + msg); }
  void err(const std::string& msg) { errors_.push_back("line " + std::to_string(lineNo_) + ": " + msg); }

  // Splits `line` at the first ';' that isn't inside a double-quoted
  // string, so a string literal (e.g. a .TEXT/.ASCII operand) containing
  // ';' isn't mistaken for a comment. `comment` keeps its leading ';'.
  static void splitComment(const std::string& line, std::string* code, std::string* comment) {
    bool inQuotes = false;
    for (size_t i = 0; i < line.size(); i++) {
      char c = line[i];
      if (c == '"') {
        inQuotes = !inQuotes;
      } else if (c == ';' && !inQuotes) {
        *code = line.substr(0, i);
        *comment = line.substr(i);
        return;
      }
    }
    *code = line;
    *comment = "";
  }

  // Crude detection of TASM's native "NAME MACRO ..." / "ENDM" macro-
  // definition syntax (distinct from the #define-based macros seen in
  // real TASM PC-1500 sources, which are already rejected by the '#'
  // check) -- not a full parser, just enough to fail loudly instead of
  // mis-converting a macro body as if it were plain code.
  static bool isMacroBlockLine(const std::string& trimmedCode) {
    std::istringstream ts(trimmedCode);
    std::string t1, t2;
    ts >> t1;
    ts >> t2;
    std::string t1u = toUpper(t1);
    return t1u == "ENDM" || t1u == ".ENDM" || toUpper(t2) == "MACRO";
  }

  // Converts one already-comment-stripped, already-trimmed instruction/
  // directive line with no label prefix (e.g. "LDI A, $00" or
  // ".EQU $7C01"'s already-handled-elsewhere sibling ".ORG ENTRY").
  void convertInstructionLine(const std::string& codePartIn, const std::string& commentSuffix,
                               std::ostringstream& out) {
    std::string code = codePartIn;
    if (!code.empty() && code.back() == '\\') {
      warn("line continuation ('\\') not supported -- each physical line is converted independently");
      code.pop_back();
      code = trim(code);
      if (code.empty()) return;
    }

    size_t sp = code.find_first_of(" \t");
    std::string firstToken = (sp == std::string::npos) ? code : code.substr(0, sp);
    std::string operands = (sp == std::string::npos) ? "" : trim(code.substr(sp + 1));

    std::string bare = firstToken;
    if (!bare.empty() && bare[0] == '.') bare = bare.substr(1);
    std::string bareLower = toLower(bare);
    std::string bareUpper = toUpper(bare);

    // TASM's trailing ".END" has no sdas equivalent worth emitting -- this
    // project's own formatListing never writes one, and confirmed live:
    // the real sdaslh5801 build this converter targets actively rejects a
    // trailing ".end" ("directive/mnemonic error"), it's not just
    // unnecessary. Drop the line, keeping any trailing comment.
    if (bareUpper == "END") {
      if (!commentSuffix.empty()) out << trim(commentSuffix) << "\n";
      return;
    }

    std::string replacement;
    bool isOrg = false;
    bool isTextLiteral = false;

    if (auto aliasIt = mnemonicAliasMap().find(bareLower); aliasIt != mnemonicAliasMap().end()) {
      replacement = aliasIt->second;
    } else if (mnemonicSet().count(bareLower)) {
      replacement = bareLower;
    } else if (bareUpper == "EXPORT" || bareUpper == "MACRO" || bareUpper == "ENDM") {
      err("'" + firstToken + "' (cross-module .EXPORT / macro definitions) is not supported");
      return;
    } else {
      auto it = directiveMap().find(bareUpper);
      if (it != directiveMap().end()) {
        replacement = it->second;
        isOrg = (bareUpper == "ORG");
        isTextLiteral = (bareUpper == "TEXT" || bareUpper == "ASCII");
      } else {
        warn("unrecognized token '" + firstToken + "' -- passed through unchanged; check the output assembles");
        replacement = firstToken;
      }
    }

    // A string-literal operand (.TEXT/.ASCII) is passed through verbatim:
    // rewriting '$'/register-name tokens inside quoted content would
    // corrupt the string rather than convert syntax.
    std::string convertedOperands = isTextLiteral ? operands : rewriteRegisters(rewriteHexLiterals(operands));

    if (isOrg) {
      if (!sawOrg_) {
        out << "\t.area CODE (ABS)\n";
        sawOrg_ = true;
      } else {
        warn("a second .ORG was found -- only the first gets a synthesized '.area CODE (ABS)' wrapper; "
             "multi-segment TASM sources aren't fully supported");
      }
    }

    out << "\t" << replacement;
    if (!convertedOperands.empty()) out << " " << convertedOperands;
    out << commentSuffix << "\n";
  }
};

}  // namespace

TasmConvertResult convertTasmToSdas(const std::string& tasmSource) {
  Converter converter;
  return converter.run(tasmSource);
}

}  // namespace pc1500::disasm
