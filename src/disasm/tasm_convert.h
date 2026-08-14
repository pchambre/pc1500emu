// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#pragma once

#include <string>
#include <vector>

// Source-to-source converter: TASM (Telemark Assembler, tasm5801.tab)
// syntax -> this project's sdas dialect (see formatter.h's AsmDialect),
// so a hand-written PC-1500 TASM source can be fed to sdaslh5801.
//
// Scoped to what a real hand-written TASM source of the "single ORG, no
// cross-module linking" kind actually needs -- confirmed against a real
// TASM PC-1500 program (a memory-test routine: .EQU/.ORG/.DB/.END, "$"
// hex, uppercase mnemonics/registers) and cross-checked for mnemonic
// coverage against a much larger real TASM PC-1500 ROM disassembly
// (github.com/Jeff-Birt/Sharp_CE-158's CE-158_ROM_LOW.lh5801.asm), which
// is where the genuine syntax differences beyond case/hex-prefix came
// from: tasm5801.tab accepts CALL/RET/SCF as aliases for the LH5801's
// actual SJP/RTN/SEC mnemonics (evidently borrowed from Z80/8080 naming)
// -- sdaslh5801 has no such aliases, so these are real renames, not just
// a case fold. See mnemonicAliasMap in the .cpp for the exact set and
// what confirmed each one.
//
// Deliberately NOT supported (the CE-158 file also demonstrated these, but
// they're a much bigger scope -- a real preprocessor/macro-expander, not a
// syntax-level rewrite -- and nothing simpler has needed them yet):
// "#INCLUDE"/"#DEFINE"/"#IFDEF" preprocessor directives, ".EXPORT"
// (cross-module linking), and "MACRO"/"ENDM" macro definitions. Lines
// using these are reported as errors rather than silently mis-converted.
namespace pc1500::disasm {

struct TasmConvertResult {
  std::string output;
  std::vector<std::string> warnings;  // non-fatal: unrecognized token, best-effort pass-through
  std::vector<std::string> errors;    // fatal: unsupported construct, output is not usable
  bool ok() const { return errors.empty(); }
};

TasmConvertResult convertTasmToSdas(const std::string& tasmSource);

}  // namespace pc1500::disasm
