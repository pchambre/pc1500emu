// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#include "known_symbols.h"

namespace pc1500::disasm {

namespace {

// clang-format off
constexpr KnownSymbol kSymbols[] = {
    // -- Status/control (src/bus/bus.cpp, src/host/main.cpp's `status`) --
    {0x764E, false, "STATUS1", "status byte 1: busy/shift/small/def flags"},
    {0x764F, false, "STATUS2", "status byte 2: run/pro/reserve flags"},
    {0x7B0E, false, "KEYGATE", "key-dispatch gate, bit 0 (ROM's own countdown clears it)"},
    {0x7874, false, "CURSOR_RESET_FLAG", "bit 0: set after a ROM call resets the cursor pointer, before returning to BASIC (PC-2 manual)"},
    {0x784E, false, "CONTPTR_HI", "keyboard-state-machine continuation pointer, high byte"},
    {0x784F, false, "CONTPTR_LO", "keyboard-state-machine continuation pointer, low byte"},

    // -- Arithmetic registers (TRM section 5-4 system-subroutine convention) --
    {0x7A00, false, "ARITH_X", "arithmetic register X, first/only operand (7A00H-7A07H)"},
    {0x7A10, false, "ARITH_Y", "arithmetic register Y, second operand (7A10H-7A17H)"},
    {0x7B10, false, "STRBUF", "string buffer (7B10H-7B5FH)"},

    // -- BASIC program structure (src/basic/text_loader.h) --
    {0x40C5, false, "BASPROG", "BASIC program storage origin (kBasicProgramStart)"},
    {0x7867, false, "PROGEND", "cached BASIC program-end pointer (kProgramEndPointerAddr)"},

    // -- ROM entry points confirmed this session --
    {0xE2AA, false, "IDLE", "BASIC's stable idle/ready-prompt address (HLT-based)"},
    {0xE366, false, "SML_DISPATCH", "SML/Small-mode lowercase-toggle key dispatch"},
    {0xC01E, false, "KWORD_INDEX", "base ROM's built-in keyword table: 52-byte first-letter index"},
    {0xC054, false, "KWORD_TABLE", "base ROM's built-in keyword table: first entry (AREAD)"},

    // -- BASWORD hook points (PC1500_BASIC_Keyword_Extension_Mechanism.md) --
    {0x79D4, false, "BASWORD_FLAG", "enhanced-keyboard-driver-installed flag (BASWORD: POKE &79D4,&55)"},
    {0x79FC, false, "BASWORD_TBLPTRS", "BASWORD's own user-keyword table start/end pointers (79FCH-79FEH)"},
    {0x785B, false, "BASWORD_HOOK", "keyboard-hook vector BASWORD repoints at its own resident dispatcher"},
    {0xE2B9, false, "BASWORD_GATE", "compatibility gate: BASWORD requires PEEK &E2B9 = 86 decimal (0x56)"},

    // -- I/O controller, ME1 space only (docs/pc1500_hardware_reference.md) --
    {0xF007, true, "IOREG_F", "I/O controller register F"},
    {0xF008, true, "IOREG_OPC", "PC0-7 output-only: PC0-5=timer control, PC6=buzzer on/off"},
    {0xF009, true, "IOREG_G", "I/O controller register G"},
    {0xF00A, true, "IOREG_MSK", "I/O controller interrupt mask register"},
    {0xF00B, true, "IOREG_IF", "I/O controller interrupt flag register"},
    {0xF00C, true, "IOREG_DDA", "PA direction register (keyboard column strobe, always all-output)"},
    {0xF00D, true, "IOREG_DDB", "PB direction register"},
    {0xF00E, true, "IOREG_OPA", "keyboard column strobe, PA0-7"},
    {0xF00F, true, "IOREG_OPB", "PB7=ON-key input, PB3=VCC(export)/GND(domestic) gates SML dispatch, PB5/6=RTC TP/DATA OUT"},
};
// clang-format on

}  // namespace

const KnownSymbol* findKnownSymbol(uint16_t addr, bool me1) {
  for (const auto& s : kSymbols) {
    if (s.addr == addr && s.me1 == me1) return &s;
  }
  return nullptr;
}

}  // namespace pc1500::disasm
