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
    {0x7874, false, "CURSOR_RESET_FLAG", "bit 0: set after a ROM call resets the cursor pointer, before returning to BASIC (PC-2 manual); Technical Reference Manual p.100 calls this whole byte CURSOR ENABLE, \"(01H) if used, (00H) if not\" -- consistent with the bit-0 finding, kept as the more specific of the two"},
    {0x784E, false, "CONTPTR_HI", "keyboard-state-machine continuation pointer, high byte"},
    {0x784F, false, "CONTPTR_LO", "keyboard-state-machine continuation pointer, low byte"},

    // -- Arithmetic registers (TRM section 5-4 system-subroutine convention) --
    {0x7A00, false, "ARITH_X", "arithmetic register X, first/only operand (7A00H-7A07H)"},
    {0x7A10, false, "ARITH_Y", "arithmetic register Y, second operand (7A10H-7A17H)"},
    {0x7B10, false, "STRBUF", "string buffer (7B10H-7B5FH)"},

    // -- BASIC program structure (src/basic/text_loader.h) --
    {0x40C5, false, "BASPROG", "BASIC program storage origin (kBasicProgramStart)"},
    {0x7867, false, "PROGEND", "cached BASIC program-end pointer (kProgramEndPointerAddr)"},

    // -- BASIC interpreter's own RAM variable table (PC-1500 Technical
    // Reference Manual, pp.100-101) -- addresses/names/functions transcribed
    // directly from that table, not independently confirmed against a real
    // disassembly the way the ROM-entry-point sections below are; treat a
    // name here as "what the manual calls it", worth double-checking if it
    // ever conflicts with something traced live. One correction made against
    // the raw OCR text: SCISSORING COUNTER XL is printed as "78E7H", which
    // breaks the otherwise strictly sequential 79E0H-79E8H pen-plotter
    // cluster it sits in the middle of (79E6H=ABSOLUTE POSITION X,
    // 79E8H=SCISSORING COUNTER XH) -- used 79E7H instead, matching that
    // sequence, since a lone 78E7H entry with no neighbors of its own reads
    // as a scan/print error rather than a real gap. 7874H (CURSOR ENABLE)
    // already has its own, more specific entry above -- not duplicated here.
    {0x786B, false, "RMT_BEEP", "remote and beep on/off pointer"},
    {0x7871, false, "WAIT_YN", "WAIT status: WAIT(0), WAIT0(3), WAIT1(2)"},
    {0x7872, false, "WAIT_COUNTER_H", "WAIT counter, high byte (7872H-7873H)"},
    {0x7873, false, "WAIT_COUNTER_L", "WAIT counter, low byte (7872H-7873H)"},
    {0x7875, false, "CURSOR_POINTER", "cursor pointer (0~155)"},
    {0x787D, false, "BLINK_CHARACTER", "character code to be blinked"},
    {0x787E, false, "BLINK_CURSOR_H", "blinking cursor position, high byte -- address of the display buffer (787EH-787FH)"},
    {0x787F, false, "BLINK_CURSOR_L", "blinking cursor position, low byte (787EH-787FH)"},
    {0x788D, false, "TRACE", "trace on/off pointer"},
    {0x788E, false, "TRACE_CONDITION", "status when trace on"},
    {0x788F, false, "OUTPUT_BUFFER_PTR", "output buffer pointer"},
    {0x7890, false, "FOR_POINTER", "FOR-NEXT stack pointer"},
    {0x7891, false, "GOSUB_POINTER", "GOSUB pointer"},
    {0x7894, false, "STRING_BUFFER_PTR", "string buffer pointer"},
    {0x7895, false, "USING_FF", "USING format flag: presence of decimal point, comma, etc."},
    {0x7896, false, "USING_M", "USING: integer part"},
    {0x7897, false, "USING_A", "USING: of character string"},
    {0x7898, false, "USING_LOWER_M", "USING: decimal part"},
    {0x7899, false, "VARIABLE_POINTER_H", "variable pointer, high byte (7899H-789AH)"},
    {0x789A, false, "VARIABLE_POINTER_L", "variable pointer, low byte (7899H-789AH)"},
    {0x789B, false, "ERL", "error number when occurred"},
    {0x789C, false, "CURRENT_LINE_H", "current line number, high byte (789CH-789DH)"},
    {0x789D, false, "CURRENT_LINE_L", "current line number, low byte (789CH-789DH)"},
    {0x789E, false, "CURRENT_TOP_H", "leading address of program of the current line, high byte (789EH-789FH)"},
    {0x789F, false, "CURRENT_TOP_L", "leading address of program of the current line, low byte (789EH-789FH)"},
    {0x78A0, false, "PREVIOUS_ADDRESS_H", "address of immediately preceding line, high byte (78A0H-78A1H)"},
    {0x78A1, false, "PREVIOUS_ADDRESS_L", "address of immediately preceding line, low byte (78A0H-78A1H)"},
    {0x78A2, false, "PREVIOUS_LINE_H", "line number immediately preceding, high byte (78A2H-78A3H)"},
    {0x78A3, false, "PREVIOUS_LINE_L", "line number immediately preceding, low byte (78A2H-78A3H)"},
    {0x78A4, false, "PREVIOUS_TOP_H", "leading address of program of the line immediately preceding, high byte (78A4H-78A5H)"},
    {0x78A5, false, "PREVIOUS_TOP_L", "leading address of program of the line immediately preceding, low byte (78A4H-78A5H)"},
    {0x78A6, false, "SEARCH_ADDRESS_H", "address of the line found during search, high byte (78A6H-78A7H)"},
    {0x78A7, false, "SEARCH_ADDRESS_L", "address of the line found during search, low byte (78A6H-78A7H)"},
    {0x78A8, false, "SEARCH_LINE_H", "line number found after search, high byte (78A8H-78A9H)"},
    {0x78A9, false, "SEARCH_LINE_L", "line number found after search, low byte (78A8H-78A9H)"},
    {0x78AA, false, "SEARCH_TOP_H", "leading address of the searched program block, high byte (78AAH-78ABH)"},
    {0x78AB, false, "SEARCH_TOP_L", "leading address of the searched program block, low byte (78AAH-78ABH)"},
    {0x78AC, false, "BREAK_ADDRESS_H", "address of breakpoint, high byte (78ACH-78ADH)"},
    {0x78AD, false, "BREAK_ADDRESS_L", "address of breakpoint, low byte (78ACH-78ADH)"},
    {0x78AE, false, "BREAK_LINE_H", "breakpoint line number, high byte (78AEH-78AFH)"},
    {0x78AF, false, "BREAK_LINE_L", "breakpoint line number, low byte (78AEH-78AFH)"},
    {0x78B0, false, "BREAK_TOP_H", "top address of the program block to which break is applied, high byte (78B0H-78B1H)"},
    {0x78B1, false, "BREAK_TOP_L", "top address of the program block to which break is applied, low byte (78B0H-78B1H)"},
    {0x78B2, false, "ERROR_ADDRESS_H", "address where error is met, high byte (78B2H-78B3H)"},
    {0x78B3, false, "ERROR_ADDRESS_L", "address where error is met, low byte (78B2H-78B3H)"},
    {0x78B4, false, "ERROR_LINE_H", "line number where error is met, high byte (78B4H-78B5H)"},
    {0x78B5, false, "ERROR_LINE_L", "line number where error is met, low byte (78B4H-78B5H)"},
    {0x78B6, false, "ERROR_TOP_H", "leading address of the program block in which error is met, high byte (78B6H-78B7H)"},
    {0x78B7, false, "ERROR_TOP_L", "leading address of the program block in which error is met, low byte (78B6H-78B7H)"},
    {0x78B8, false, "ON_ERROR_ADDRESS_H", "ON ERROR GOTO target: address to which program jumps when an error is met, high byte (78B8H-78B9H)"},
    {0x78B9, false, "ON_ERROR_ADDRESS_L", "ON ERROR GOTO target: address to which program jumps when an error is met, low byte (78B8H-78B9H)"},
    {0x78BA, false, "ON_ERROR_LINE_H", "ON ERROR GOTO target line number, high byte (78BAH-78BBH)"},
    {0x78BB, false, "ON_ERROR_LINE_L", "ON ERROR GOTO target line number, low byte (78BAH-78BBH)"},
    {0x78BC, false, "ON_ERROR_TOP_H", "leading address of the program block containing the ON ERROR GOTO target, high byte (78BCH-78BDH)"},
    {0x78BD, false, "ON_ERROR_TOP_L", "leading address of the program block containing the ON ERROR GOTO target, low byte (78BCH-78BDH)"},
    {0x78BE, false, "DATA_POINTER_H", "pointer for DATA statement, high byte (78BEH-78BFH)"},
    {0x78BF, false, "DATA_POINTER_L", "pointer for DATA statement, low byte (78BEH-78BFH)"},
    {0x79D1, false, "OPN_DV", "peripheral device select"},
    {0x79E0, false, "USER_COUNTER_XH", "counter by which X-coordinates of the pen are indicated, high byte (79E0H-79E1H)"},
    {0x79E1, false, "USER_COUNTER_XL", "counter by which X-coordinates of the pen are indicated, low byte (79E0H-79E1H)"},
    {0x79E2, false, "USER_COUNTER_YH", "counter by which Y-coordinates of the pen are indicated, high byte (79E2H-79E3H)"},
    {0x79E3, false, "USER_COUNTER_YL", "counter by which Y-coordinates of the pen are indicated, low byte (79E2H-79E3H)"},
    {0x79E4, false, "SCISSORING_COUNTER_YH", "Y-direction scissoring counter, high byte (79E4H-79E5H)"},
    {0x79E5, false, "SCISSORING_COUNTER_YL", "Y-direction scissoring counter, low byte (79E4H-79E5H)"},
    {0x79E6, false, "ABSOLUTE_POSITION_X", "X-direction absolute point counter"},
    {0x79E7, false, "SCISSORING_COUNTER_XL", "X-direction scissoring counter, low byte (79E7H-79E8H) -- printed as 78E7H in the manual, corrected here, see this section's own comment"},
    {0x79E8, false, "SCISSORING_COUNTER_XH", "X-direction scissoring counter, high byte (79E7H-79E8H)"},
    {0x79EA, false, "LINE_TYPE", "kind of line"},
    {0x79EB, false, "DOT_LINE_COUNTER", "dot line counter"},
    {0x79EC, false, "UP_DOWN", "pen up/down position select"},
    {0x79ED, false, "X_MOTOR_HOLD_COUNTER", "X motor hold counter"},
    {0x79EE, false, "PORT_C", "indicates current motor phase"},
    {0x79EF, false, "Y_MOTOR_HOLD_COUNTER", "Y motor hold counter"},
    {0x79F0, false, "GRAPH_TEXT", "printer mode select (graph=255, text=0)"},
    {0x79F2, false, "ROTATE", "printing direction select"},
    {0x79F3, false, "COLOR", "color select"},
    {0x79F4, false, "CSIZE", "printing character size select"},
    {0x79FF, false, "LOCK", "lock/unlock select"},
    {0x7B00, false, "RND_NUMBER", "random number (7B00H-7B07H)"},
    {0x7B0A, false, "AUTO_POFF_COUNTER_U", "auto power off counter, upper byte (7B0AH-7B0CH)"},
    {0x7B0B, false, "AUTO_POFF_COUNTER_M", "auto power off counter, middle byte (7B0AH-7B0CH)"},
    {0x7B0C, false, "AUTO_POFF_COUNTER_L", "auto power off counter, lower byte (7B0AH-7B0CH)"},

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
