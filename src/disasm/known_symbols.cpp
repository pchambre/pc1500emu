// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#include "known_symbols.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>

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

    // -- PC-2 Assembly Language (Bruce Elliott, TRS-80 Microcomputer News,
    // 1983-84) memory-map routine table (printed p.26) cross-checked
    // against the same series' individually-labeled System Calls
    // reference (Entry/Exit/Flags per routine, printed pp.43-47) where
    // more precise -- keyboard --
    {0xE243, false, "KEYSCAN_WAIT", "scan keyboard, wait for a key; ACC=key code on return; SHIFT/DEF/SML don't return; auto-off after ~7min idle; on BREAK Carry=1, ACC=0EH (matches the user's own program comment 'Read Keyboard')"},
    {0xE42C, false, "KEYSCAN_NOWAIT", "scan keyboard, return immediately; ACC=00H if no key, else key code"},
    {0xE33F, false, "AUTO_POWER_OFF", "auto power-off routine"},

    // -- PC-2 manual, same source -- LCD / display --
    {0xE8CA, false, "DISP_BUFFER", "display through the 80-byte LCD buffer (7BB0H-7BFFH); string terminated by 0DH, Y=buffer cursor pointer, mode byte at 7880H"},
    {0xED00, false, "DISP_N_CHARS", "output n chars to LCD from current cursor; U=start addr, ACC=length (01H-1AH)"},
    {0xED3B, false, "DISP_N_CHARS0", "output n chars to LCD starting at cursor position 0; U=start addr, XL=length (01H-1AH)"},
    {0xED4D, false, "DISP_CHAR_ADV", "output one char (ACC=ASCII) to LCD, advance cursor by one position (6H), wraps to 0 past 96H (matches the user's own program comment 'Display Character')"},
    {0xED57, false, "DISP_CHAR", "output one char (ACC=ASCII) to LCD; cursor position not advanced"},
    {0xEDF6, false, "DISP_HEX_BYTE", "output one byte as two hex digits to LCD"},
    {0xED95, false, "ASCII_TO_HEX", "convert two ASCII hex chars (X=addr of first) to one byte; X+=2, ACC=result"},

    // -- PC-2 manual, same source -- string functions (BASIC keyword entry points) --
    {0xD925, false, "STRCAT", "string concatenation"},
    {0xD9B1, false, "CHR$", "BASIC CHR$ function entry point"},
    {0xD9CF, false, "STR$", "BASIC STR$ function entry point; numeric value in 7A00H-7A07H, 7894H=10H, result string at 7B10H+ (p.26's memory-map table gives D9C7H instead -- an OCR discrepancy between the two passes; D9CFH used here as the more structured/individually-labeled source, worth confirming against a real ROM1.BIN disassembly)"},
    {0xD9D7, false, "VAL", "BASIC VAL function entry point"},
    {0xD9DD, false, "ASC_LEN", "BASIC ASC (YL=60H) / LEN (YL=64H) function entry point"},
    {0xD9F3, false, "RIGHT_LEFT_MID", "BASIC RIGHT$/LEFT$/MID$ function entry point; register conventions differ per variant"},

    // -- PC-2 manual, same source -- comparisons / program structure --
    {0xD0F9, false, "STRCMP", "magnitude comparison for character strings"},
    {0xD2EA, false, "FINDLINE", "search for a BASIC program line number"},
    {0xD461, false, "FINDVAR", "find the address of a BASIC variable"},
    {0xD9D2, false, "NUMCMP", "magnitude comparison for numeric values"},

    // -- PC-2 manual, same source -- numeric functions (BCD; operands 7A00H-7A07H/7A10H-7A17H) --
    {0xEFB6, false, "FSUB", "X-Y -> X (BCD)"},
    {0xEFBA, false, "FADD", "X+Y -> X (BCD)"},
    {0xF01A, false, "FMUL", "X*Y -> X (BCD)"},
    {0xF084, false, "FDIV", "X/Y -> X (BCD)"},
    {0xF89C, false, "FPOW", "X^Y -> X (BCD)"},
    {0xF0E9, false, "FSQR", "SQR(X) -> X (BCD)"},
    {0xF161, false, "FLN", "LN(X) -> X (BCD)"},
    {0xF165, false, "FLOG", "LOG(X) -> X (BCD)"},
    {0xF1CB, false, "FEXP", "e^X -> X (BCD)"},
    {0xF1D4, false, "FPOW10", "10^X -> X (BCD)"},
    {0xF391, false, "FCOS", "COS(X) -> X (BCD)"},
    {0xF39E, false, "FTAN", "TAN(X) -> X (BCD)"},
    {0xF3A2, false, "FSIN", "SIN(X) -> X (BCD)"},
    {0xF492, false, "FACS", "ACS(X) -> X (BCD)"},
    {0xF496, false, "FATN", "ATN(X) -> X (BCD)"},
    {0xF49A, false, "FASN", "ASN(X) -> X (BCD)"},
    {0xF531, false, "FDEG", "DEG(X) -> X (BCD)"},
    {0xF564, false, "FDMS", "DMS(X) -> X (BCD)"},
    {0xF597, false, "FABS", "ABS(X) -> X (BCD)"},
    {0xF59D, false, "FSGN", "SGN(X) -> X (BCD)"},
    {0xF5BE, false, "FINT", "INT(X) -> X (BCD)"},

    // -- PC-2 manual, same source -- cassette I/O, 8000H-BFFFH expansion
    // ROM (CE-150 module required, PV low -- src/bus/bus.h's Bus::RomModule,
    // not the PC-2 manual, is the source for the PV-low requirement; the
    // manual itself only says PU, not PV, gates the printer ROM) --
    {0xBF11, false, "TAPE_MOTOR_ON", "turn cassette tape drive on; 7879H bit7=port select, bit4=Remote (CE-150 module required, PV low)"},
    {0xBF43, false, "TAPE_MOTOR_OFF", "turn cassette tape drive off (CE-150 module required, PV low)"},
    {0xBBD6, false, "TAPE_HDR_WRITE", "construct tape sync header/filename; ACC=file mode (00=object,01=program,02=reserve,04=data); header written 7B60H-7B67H, mode at 7B68H (CE-150 module required, PV low)"},
    {0xBCE8, false, "TAPE_HDR_READ", "read tape sync header/search for filename; optional filename at 7B69H-7B78H, 7879H bit7=1 (CE-150 module required, PV low)"},
    {0xBD3C, false, "TAPE_RW", "read/write a tape file (source article lists a single entry point for both; gating unclear, worth confirming against a real disassembly) (CE-150 module required, PV low)"},
    {0xBDCC, false, "TAPE_PUTC", "send one character to tape; ACC=char, must call TAPE_HDR_WRITE (BBD6H) first (CE-150 module required, PV low)"},
    {0xBDF0, false, "TAPE_GETC", "read one character from tape; ACC=result, Carry=1 on BREAK (CE-150 module required, PV low)"},

    // -- PC-2 manual, same source -- printer, 8000H-BFFFH expansion ROM
    // (CE-150 module required, PV low -- see the cassette-I/O group's own
    // comment above for why this cites bus.h rather than the manual) --
    {0xA519, false, "PRT_PEN_COLOR", "change printer pen color (CE-150 module required, PV low)"},
    {0xA769, false, "PRT_MOTOR_OFF", "printer motor off (CE-150 module required, PV low)"},
    {0xA781, false, "PRT_PUTC", "send one ASCII char to printer, no line feed (CE-150 module required, PV low)"},
    {0xA8DD, false, "PRT_LF", "send a line feed to printer (CE-150 module required, PV low)"},
    {0xAA04, false, "PRT_N_LF", "send n line feeds to printer (CE-150 module required, PV low)"},
    {0xAA09, false, "PRT_PEN_UPDOWN", "printer pen up/down (CE-150 module required, PV low)"},
    {0xABEF, false, "PRT_GRAPHIC_MODE", "switch printer from text to graphic mode (CE-150 module required, PV low)"},
};
// clang-format on

}  // namespace

const KnownSymbol* findKnownSymbol(uint16_t addr, bool me1) {
  for (const auto& s : kSymbols) {
    if (s.addr == addr && s.me1 == me1) return &s;
  }
  return nullptr;
}

bool loadUserSymbolsFile(const std::string& path, std::vector<UserSymbol>* out, std::string* error) {
  std::ifstream f(path);
  if (!f) {
    if (error) *error = "could not open '" + path + "'";
    return false;
  }

  std::string line;
  int lineNo = 0;
  while (std::getline(f, line)) {
    lineNo++;
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();

    size_t start = line.find_first_not_of(" \t");
    if (start == std::string::npos) continue;  // blank line
    if (line[start] == '#') continue;          // comment line

    size_t addrEnd = line.find_first_of(" \t", start);
    size_t nameStart = (addrEnd == std::string::npos) ? std::string::npos
                                                        : line.find_first_not_of(" \t", addrEnd);
    if (addrEnd == std::string::npos || nameStart == std::string::npos) {
      std::fprintf(stderr,
                    "pc1500disasm: symbols file '%s' line %d: expected '<addr> <name> "
                    "[comment]', skipping: %s\n",
                    path.c_str(), lineNo, line.c_str());
      continue;
    }
    std::string addrTok = line.substr(start, addrEnd - start);

    size_t nameEnd = line.find_first_of(" \t", nameStart);
    std::string nameTok = (nameEnd == std::string::npos) ? line.substr(nameStart)
                                                           : line.substr(nameStart, nameEnd - nameStart);
    std::string commentTok;
    if (nameEnd != std::string::npos) {
      size_t commentStart = line.find_first_not_of(" \t", nameEnd);
      if (commentStart != std::string::npos) commentTok = line.substr(commentStart);
    }

    char* endPtr = nullptr;
    unsigned long addrVal = std::strtoul(addrTok.c_str(), &endPtr, 16);
    if (endPtr == addrTok.c_str() || *endPtr != '\0' || addrVal > 0xFFFF) {
      std::fprintf(stderr, "pc1500disasm: symbols file '%s' line %d: bad address '%s', skipping\n",
                    path.c_str(), lineNo, addrTok.c_str());
      continue;
    }

    UserSymbol sym;
    sym.addr = static_cast<uint16_t>(addrVal);
    sym.name = std::move(nameTok);
    sym.comment = std::move(commentTok);
    out->push_back(std::move(sym));
  }
  return true;
}

std::optional<ResolvedSymbol> lookupSymbol(uint16_t addr, bool me1,
                                            const std::vector<UserSymbol>& userSymbols) {
  for (const auto& u : userSymbols) {
    if (u.addr == addr && u.me1 == me1) return ResolvedSymbol{u.name, u.comment};
  }
  const KnownSymbol* sym = findKnownSymbol(addr, me1);
  if (sym != nullptr) return ResolvedSymbol{sym->name, sym->comment};
  return std::nullopt;
}

}  // namespace pc1500::disasm
