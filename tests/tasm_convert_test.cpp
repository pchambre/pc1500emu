// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#include <cstdio>
#include <string>

#include "tasm_convert.h"

namespace {

using pc1500::disasm::convertTasmToSdas;
using pc1500::disasm::TasmConvertResult;

int g_failures = 0;

#define CHECK(cond)                                                \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      g_failures++;                                                 \
    }                                                                \
  } while (0)

// Covers the syntax actually exercised by a real hand-written TASM PC-1500
// program (a memory-test routine): .EQU/.ORG/.DB/.END, "$" hex, uppercase
// mnemonics/registers, a label sharing a line with its instruction, and an
// indirect operand referencing a label (which must NOT get case-folded --
// only register names do). One clean pass should produce no warnings/
// errors and a synthesized ".area CODE (ABS)" ahead of the first ".org".
void testBasicConversion() {
  const std::string src =
      "; a comment\n"
      "ENTRY      .EQU    $7C01\n"
      "BOTTOM_H   .EQU    $7867\n"
      "\n"
      "            .ORG    ENTRY\n"
      "ERR_FLAG:   .DB     $00         ; result byte\n"
      "MEMTEST:\n"
      "            LDI     A, $00\n"
      "            STA     (ERR_FLAG)\n"
      "LOOP1:      LDA     UH\n"
      "            STA     (X)\n"
      "            BZR     MEMTEST\n"
      "            .END\n";

  TasmConvertResult r = convertTasmToSdas(src);
  CHECK(r.ok());
  CHECK(r.warnings.empty());
  const std::string& out = r.output;

  CHECK(out.find("ENTRY .equ 0x7C01") != std::string::npos);
  CHECK(out.find("\t.area CODE (ABS)\n\t.org ENTRY\n") != std::string::npos);
  // Label sharing a source line with its instruction splits onto two lines.
  CHECK(out.find("ERR_FLAG:\n\t.db 0x00") != std::string::npos);
  CHECK(out.find("MEMTEST:\n") != std::string::npos);
  CHECK(out.find("\tldi a, 0x00\n") != std::string::npos);
  // Register inside parens is case-folded; a label inside parens is not.
  CHECK(out.find("\tsta (ERR_FLAG)\n") != std::string::npos);
  CHECK(out.find("\tsta (x)\n") != std::string::npos);
  CHECK(out.find("LOOP1:\n\tlda uh\n") != std::string::npos);
  CHECK(out.find("\tbzr MEMTEST\n") != std::string::npos);
  // ".END" is dropped, not translated -- see tasm_convert.cpp's own
  // comment on why (a real sdaslh5801 build rejects a trailing ".end").
  CHECK(out.find(".end") == std::string::npos);
  CHECK(out.find("END") == std::string::npos);
}

// CALL/RET/SCF are TASM aliases for the LH5801's actual SJP/RTN/SEC
// mnemonics (confirmed against two independent real TASM PC-1500 sources
// -- see tasm_convert.h) -- a real rename, not just a case fold.
void testMnemonicAliases() {
  TasmConvertResult r = convertTasmToSdas("    .ORG $8000\n    CALL SUB1\n    SCF\n    RET\nSUB1:\n    RTN\n");
  CHECK(r.ok());
  CHECK(r.output.find("\tsjp SUB1\n") != std::string::npos);
  CHECK(r.output.find("\tsec\n") != std::string::npos);
  CHECK(r.output.find("\trtn\n") != std::string::npos);
  CHECK(r.output.find("call") == std::string::npos);
  CHECK(r.output.find("CALL") == std::string::npos);
  CHECK(r.output.find("\tret\n") == std::string::npos);
}

// A second .ORG doesn't get its own synthesized .area (this converter
// only supports single-segment TASM sources -- see tasm_convert.h) and is
// reported as a warning, not silently dropped or duplicated.
void testSecondOrgWarns() {
  TasmConvertResult r = convertTasmToSdas("    .ORG $8000\n    NOP\n    .ORG $9000\n    NOP\n");
  CHECK(r.ok());
  CHECK(!r.warnings.empty());
  size_t firstArea = r.output.find(".area");
  CHECK(firstArea != std::string::npos);
  CHECK(r.output.find(".area", firstArea + 1) == std::string::npos);
}

// An unrecognized token is a best-effort pass-through with a warning, not
// a hard failure -- distinct from genuinely unsupported constructs below.
void testUnrecognizedTokenWarns() {
  TasmConvertResult r = convertTasmToSdas("    .ORG $8000\n    FROB A, B\n");
  CHECK(r.ok());
  CHECK(!r.warnings.empty());
  CHECK(r.output.find("FROB") != std::string::npos);
}

// TASM preprocessor directives, .EXPORT, and MACRO/ENDM blocks are out of
// scope (a real preprocessor/macro-expander, not a syntax rewrite -- see
// tasm_convert.h) and must fail loudly rather than silently mis-convert.
void testUnsupportedConstructsError() {
  {
    TasmConvertResult r = convertTasmToSdas("#DEFINE FOO 1\n    .ORG $8000\n");
    CHECK(!r.ok());
  }
  {
    TasmConvertResult r = convertTasmToSdas("    .ORG $8000\n.EXPORT FOO\n");
    CHECK(!r.ok());
  }
  {
    TasmConvertResult r = convertTasmToSdas("    .ORG $8000\nFOO MACRO n\n    .DB n\nENDM\n");
    CHECK(!r.ok());
  }
}

}  // namespace

int main() {
  testBasicConversion();
  testMnemonicAliases();
  testSecondOrgWarns();
  testUnrecognizedTokenWarns();
  testUnsupportedConstructsError();

  if (g_failures == 0) {
    std::printf("All tests passed.\n");
    return 0;
  }
  std::printf("%d check(s) failed.\n", g_failures);
  return 1;
}
