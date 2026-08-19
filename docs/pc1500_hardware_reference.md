# PC-1500 Hardware Reference

Source: PC-1500 Technical Reference Manual
(`/home/paul/Documents/PC1500_Technical_reference_manual.pdf`), chapter 3
"LH5810/LH5811 I/O port controller" (labeled pages 67-84 / PDF 71-88), chapter
4 "PC-1500 hardware description" (labeled pages 85-110 / PDF 89-114), and
section 5-4-5 "Display" (labeled pages 131-134 / PDF 135-138).

Cassette interface is intentionally excluded (out of scope for this
emulator) — pin/register facts that only pertain to cassette are omitted
even where the manual documents them (e.g. CE-150's built-in cassette port).
Cassette *system-subroutine entry points/RAM layout* (section 5-4-7) were
still useful as a cross-check when debugging BASIC save/load's memory
layout — see the program-end-pointer note below.

Other primary sources on disk, also from Paul: `PC-2_Service_Manual.pdf`
(easier to read than the Technical Reference Manual for some topics, e.g.
the keyboard matrix) and `PC1500/CE-150.ROM` (a real CE-150 expansion ROM
dump — cassette/printer commands live here, mapped at `A000H`-`BFFFH`,
confirmed by its documented entry points, e.g. `BD3CH` "file transfer",
only falling in range at that base).

## Memory model: ME0 and ME1

The LH5801 has two independent 64KB memory spaces, ME0 and ME1, selected by
addressing mode: plain `(Rreg)`/`(ab)` forms access ME0, `#(Rreg)`/`#(ab)`
forms access ME1 (see `lh5801_opcode_reference.md`). On the PC-1500:

- **ME0** holds all normal ROM/RAM (system program, user RAM, display
  buffer). See memory map below.
- **ME1** holds only the LH5811 I/O port controller, at `F000H`-`F00FH` (16
  addresses, matching its 4-bit register-select field — see below). Nothing
  else lives in ME1 on a stock PC-1500.

## ME0 memory map

| Range | Contents |
|---|---|
| `0000H`-`3FFFH` | Option user memory (module unit RAM/ROM slot) |
| `4000H`-`47FFH` | Standard user RAM (built-in, 2KB, chip HM6116). NOT mirrored into `4800H`-`4FFFH` (confirmed real hardware). |
| `4800H`-`6FFFH` | Option user memory (module unit, further banks) -- absent on a stock unit |
| `7000H`-`77FFH` | Aliases `7600H`-`77FFH` (`addr \| 0600H`). Per the manual's own chip-select schematic (4-2-3): the first decoder stage asserts S6 for this whole window from AD11/AD12/AD13 alone; a second stage then asserts V2 (display chips 1&3) when AD8=0 and DME0=1, or V3 (chips 2&4) when AD8=1 and DME0=1 -- neither depends on AD9/AD10 at all, so any address in the window with the right AD8 hits the same display-chip latch regardless of AD9/AD10. The manual's own summary diagram labels `7000H`-`75FFH` "INHIBITED" (i.e. not a documented/supported address), but the ROM's own boot-message renderer relies on exactly this aliasing (composing glyph columns at `7400H`+ to make them appear at `7600H`+). Confirmed directly on real hardware for `7000H`, `7100H`, `7200H`, and `7400H` each aliasing `7600H`, and that this does NOT extend to `7A00H`-`7BFFH` (outside this chip-select block; see below). |
| `7600H`-`76FFH` | Display buffer, chips 1 & 3 (see LCD section) |
| `7700H`-`77FFH` | Display buffer, chips 2 & 4 |
| `7800H`-`7BFFH` | System RAM (fixed variable area) |
| `7C00H`-`7FFFH` | Duplicate of `7800H`-`7BFFH` (confirmed on real hardware). Per 4-2-3's schematic, chip-select S7 (system RAM, chip TC5514) is asserted for the whole `7800H`-`7FFFH` window from AD11/AD12/AD13 alone, same mechanism as the `7000H`-`77FFH` case above -- the actual RAM chip doesn't distinguish bit 10 within that window. |
| `8000H`-`BFFFH` | CE-150/CE-153/CE-158 system program + I/O PC (only present if that peripheral is connected) |
| `C000H`-`FFFFH` | PC-1500 system ROM (16KB, chip SC61328F) |

**System-subroutine entry points**: `src/disasm/known_symbols.cpp` catalogs
confirmed ROM routine addresses in both ranges above (keyboard scan, LCD
output, BASIC string/numeric-function entry points, cassette I/O, printer
I/O), sourced from the PC-2 Assembly Language manual (Bruce Elliott,
*TRS-80 Microcomputer News*, 1983-84) -- the disassembler
(`pc1500disasm`) renders these as inline annotations automatically. Every
routine in `8000H`-`BFFFH` requires the CE-150 module attached with PV
low (see `Bus::RomModule` in `src/bus/bus.h`) -- the manual itself doesn't
state this (it only documents PU, not PV, as the printer-ROM enable
signal), so it's cited from this project's own already-confirmed hardware
behavior instead.

**BASIC interpreter RAM variables** (`7800H`-`7BFFH`, the fixed variable
area above): `src/disasm/known_symbols.cpp` also catalogs the BASIC
interpreter's own named RAM variables in this range (WAIT counter, FOR/GOSUB
pointers, current/previous/search/break/error line+address+top fields, ON
ERROR GOTO target, USING format state, and the pen-plotter/printer variables
at `79D1H`+/`7B00H`+), transcribed from the PC-1500 Technical Reference
Manual's own table (pp.100-101) -- unlike the ROM-routine entries above,
these are transcribed directly from that table, not independently confirmed
against a real disassembly, so treat a name here as "what the manual calls
it" rather than something separately traced live.

## PC-1500A base-unit variant

The PC-1500A is a hardware variant of the PC-1500, described on the last
few pages of the Technical Reference Manual. Confirmed by deriving from
the manual's own facts (cross-checked against the max-RAM figure the
manual states for a fully-expanded PC-1500A) -- see `pc1500emu`'s
`Bus::MachineVariant`/`Bus::extRamExtBase()` for the implementation:

- Built-in user RAM grows from 2K (`4000H`-`47FFH`) to 6K
  (`4000H`-`57FFH`).
- The 40-pin expansion port is rewired: the module-select pins that are
  S1/S2/S3 on a PC-1500 become S3/S4/S5 on a PC-1500A. Each shifted pin's
  own chip-select address range moves up by `1000H` accordingly. Net
  effect: the "option user memory" window that starts at `4800H` on a
  PC-1500 (see the ME0 memory map above) starts at `5800H` on a
  PC-1500A instead, and its max span shrinks from 10K to 6K (its upper
  bound stays `6FFFH` either way, since `7000H`+ is fixed onboard
  hardware untouched by the port rewiring).
- CE-163 (32K banked RAM)'s bank-select write-trigger range shifts the
  same way: `5800H`-`5FFFH` on a PC-1500 becomes `6800H`-`6FFFH` on a
  PC-1500A.
- CE-155 (8K module: 2K isolated at exactly `3800H`-`3FFFH`, 6K filling
  the expansion window) follows the expansion window's own shift: its 6K
  portion lands at `4800H`-`5FFFH` on a PC-1500, `5800H`-`6FFFH` on a
  PC-1500A.
- Max practical PC-1500A configuration: 16K at `0000H` (CE-159/generic) +
  6K built-in + 6K expansion window = 28K total.

**Manual correction**: page A-8 of the Technical Reference Manual states
that expansion-port Pin 5 selects the `0000H`-`3FFFH` address range --
this is wrong. The real range Pin 5 selects is `6000H`-`67FFH`.

**Gotcha, learned the hard way**: `4000H`-`47FFH` being "standard user RAM"
at the chip-select level does *not* mean all of it is free scratch space.
On a bare PC-1500 (no CE-151/CE-155/CE-159 module), the BASIC ROM firmware
uses `4000H`-`40C4H` as its "reserve area" (section 5-3-6 of the manual),
and the actual BASIC program only starts at `40C5H`. Overwriting that range
(e.g. with a hand-POKEd ML routine) doesn't fail the POKE — it's still
plain RAM — but corrupts state the interpreter depends on. See
`pc1500_keyscan_probe.md` for how this bit us in practice. Safe scratch
space for small ML routines is somewhere comfortably above `40C5H` +
whatever the current BASIC program/variables occupy.

**Correction (2026-07-26)**: an earlier version of this note described the
reserve area as containing "a pointer to the BASIC program" — that's
wrong. Per the manual's own section 5-3-6 tables, `4000H`-`4007H` is an
8-byte "ROM status information" block (a `55H` signature byte, a plugged
-in module ROM's own top address/size/confidentiality bookkeeping — not
the live user program's bounds) and `4008H`-`40C4H` (189 bytes) holds the
F1-F6 reserve-key reassignment shortcuts (each key's registered BASIC
text, stored as a mix of ASCII characters and 2-byte tokenized command
codes per the section 5-2 internal code chart, `00H`-terminated per key).
Neither part is a "pointer to the BASIC program". CLOAD/CSAVE instead
find the program's end by walking its own line structure (section 5-3-5:
each line is `[2-byte line#][1-byte size][size bytes, which already
*include* the trailing 0DH terminator -- confirmed against the manual's
own worked example, "10 PRINT A" 's size byte is `04H` covering exactly
`F0 97 41 0D`, not just the 3 content bytes]`) from `40C5H` until hitting
the single `FFH` end-of-program marker byte that follows the last line —
see `pc1500emu`'s own `findBasicProgramEnd()` in `src/host/main.cpp`,
which does exactly this walk (not a naive "scan for
the first `FF` byte", since one could legitimately appear inside a line's
own tokenized content or a string literal).

**Program-end pointer, found empirically (2026-07-26)**: the ROM does not
rely solely on scanning for the `FFH` terminator at runtime -- it also
caches the terminator's own address as a 2-byte big-endian pointer at
**`7867H`/`7868H`** (system RAM), updated as a side effect of normal
line-editing. Confirmed by direct experiment: writing a byte-for-byte
correct tokenized program into `40C5H`+ (verified via memory dump) was
*not* sufficient for `LIST`/`RUN` to recognize it -- both showed nothing
until `7867H`/`7868H` was also poked to the terminator's address, at which
point `LIST` immediately worked. `pc1500emu`'s `loadBasicProgram()`
(`src/host/main.cpp`) writes this pointer after loading the raw bytes, for
exactly this reason. Other transient editing state was also observed to
differ (e.g. a raw-text redisplay/input-line buffer around `7A08H`, an
edit-cursor-like pointer at `78A6H`/`78A7H`) but wasn't needed to make
`LIST`/`RUN` work correctly -- only `7867H`/`7868H` was confirmed
necessary and sufficient for that.

**F1-F6 reserve-key assignment never committed -- root cause found and
fixed (2026-07-26)**: reassigning a function key (SHIFT+MODE, select a
key, type replacement text, ENTER) silently did nothing -- the new text
never appeared, and re-entering RESERVE mode still showed the key as
unassigned. Instruction-level tracing (the `trace` FIFO command) showed
the assignment-lookup/append routine (`CEC6H`-`CECFH`: `ldi xl,56h` then
a `lin x` / `cpa (7884h)` scan loop) never terminates within the
documented 189-byte reserve area -- it's a linear scan for either a `00H`
"end of assignments" byte or a match, and it just runs off past `40C4H`
into whatever garbage follows, because **no ROM code path ever
initializes `4008H`-`40C4H`** (confirmed by tracing straight through
power-on, `CL`, and `NEW0` -- the whole area stayed at `pc1500emu`'s
generic uninitialized-RAM fill value throughout). Real hardware always
reads `0` there for an unassigned key (confirmed by Paul via `PEEK`), so
that 189-byte structure must already be a valid, zeroed, `00H`-terminated
list before the ROM ever touches it -- there is no real-hardware
equivalent of a truly *uninitialized* reserve area for the ROM to cope
with. Fixed in `Bus`'s constructor (`src/bus/bus.h`) by seeding exactly
`4008H`-`40C4H` to `0x00` (leaving the rest of the generic `0xFF` fill
alone, since that's still correct elsewhere -- see the `79FFH` note
above). Verified end-to-end against the exact walkthrough this bug was
reported with: `CL`/`NEW0` -> `F1` shows `!` -> assign `F1`=`PRINT` ->
`F1` shows `PRINT`, `SHIFT+F1` still shows `!`, and re-entering RESERVE
mode shows `F1:PRINT`.

**Reserve-area leading address is configuration-dependent** (confirmed
against real hardware): a bare 2K-RAM PC-1500 uses `4000H` (program at
`40C5H`, as above), but a unit with extra RAM installed uses a different
base per the manual's own table (`4000H`/`3800H`/`2000H` depending on
which module) -- shifting the program-start address correspondingly.
`pc1500emu` currently only models the bare-2K case (`kBasicProgramStart`
is a fixed `0x40C5` in `main.cpp`); this would need to become configurable
(likely tied to the emulator's own extension-RAM settings) to correctly
support BASIC save/load with extension RAM enabled.

Chip-select is built from two decoders (TC40H139F, TC40H138F) gated by CPU
signals `BFO`/`AD14`/`AD15` (top-level 16KB-region select `1Y0`-`1Y3`) and
`AD11`-`AD13` with `ME0` (sub-region select `S0`-`S7` within the `4000H`-`7FFFH`
1Y1 region). Full truth table and gate-level detail is in the manual
(labeled pages 91-93) if bus-level (not just address-range) accuracy is ever
needed — not transcribed here since a memory-mapped emulator only needs the
address ranges.

## LH5810/LH5811 I/O port controller

Single-chip CMOS peripheral controller. LH5810/LH5811 are used
interchangeably in the manual ("the same chip is used for I/O of CE-150,
CE-153, and CE-158" — LH5811 is described as the version used in the
PC-1500 itself). Provides:

- Two 8-bit bidirectional ports, **PA0-7** and **PB0-7** (direction
  independently programmable per-bit via DDA/DDB registers).
- One 8-bit **output-only** port, **PC0-7**, latched on the falling edge of
  external clock `PΦ`.
- Interrupt request handling (IRQ input, PB7 input, receive/transmit flags).
- Serial data transfer control (cassette-related on PC-1500 -- modeled for
  transmit only, see below; nothing on a stock PC-1500 exercises reception
  without a cassette/CE-150 physically attached).
- CPU wait-state generation for slow memory (not needed for an emulator,
  since it only affects real-hardware timing, not observable behavior).

### Register map (selected via RS0-3, 4-bit register-select field, when CS0-2 addresses the chip)

| RS3 RS2 RS1 RS0 | Register | R (bus→reg) | W (reg→bus) |
|---|---|---|---|
| `0100` | divider reset | — | (reset only, no register) |
| `0101` | U (serial receive) | write: n/a | read: contents of U, resets RD flag |
| `0110` | serial transmit | write: starts transmission of data byte, resets TD flag | — |
| `0111` | F | store | send |
| `1000` | OPC | store | send |
| `1001` | G | store | send |
| `1010` | MSK | store | send |
| `1011` | IF | store | send |
| `1100` | DDA | store | send |
| `1101` | DDB | store | send |
| `1110` | OPA | store (drives PA when DDA bit=1) | send (reads PA when DDA bit=0) |
| `1111` | OPB | store (drives PB when DDB bit=1) | send (reads PB when DDB bit=0) |

### Serial transmit/receive (registers `0101`/`0110`, F/G clock dividers)

Confirmed **not** used by BASIC's BEEP (it bit-bangs OPC/PC6 directly in a
software delay loop -- see the OPC row above and BEEP's actual repeat-gap
wait, which polls IF bit 1 alongside PB5 -- see "uPD1990AC real-time
clock" below for what that actually is). Confirmed **is** used by the
CE-150 printer/cassette interface ROM (`CE-150.ROM`, disassembled
directly): it polls IF bit 3 clear, then writes the next byte to register
`0110` to start transmission, i.e. bit 3 is TD (transmit done).

Per the PC-2 Service Manual (chapter 3, "LH5811 I/O PORT"): format is
start bit + 8 data bits + 2 stop bits (11 bit periods total), clock rate
selectable from `{1/1, 1/2, 1/128, 1/256, 1/512, 1/1024, 1/2048, 1/4096}`
of the CPU's own clock. `pc1500emu`'s `IoPortController` models this as
`11 * divisor` cycles from write to TD-set, with F's low 3 bits selecting
the divisor in that same ascending order (`f_ & 0x07` indexes
`{1,2,128,256,512,1024,2048,4096}`).

**Not hardware-confirmed** (no bit-level register table survived either
manual's text extraction -- neither `pdftotext` nor visual PDF reading of
the Service Manual's page 11 block diagram recovered one): the F-bits-to-
divisor mapping above is a best guess from the *listed order* of rates,
not a verified bit encoding; likewise the RD (receive-done) flag's bit
position (bit 2, by analogy to TD's bit 3 -- no ROM code path exercising
real reception has been traced). Refine both against real CE-150 ROM
timing/behavior if serial reception, or exact transmit timing, ever
matters for a specific feature (e.g. cassette SAVE/LOAD).

### uPD1990AC real-time clock (PC0-PC5, PB5/PB6)

A third chip entirely, separate from both the LH5801 CPU's own internal
timer (crystal/128 divider, vector `FFFAH`, see `lh5801::CPU::tickTimer`)
and the LH5811's own serial-transmit hardware above. Confirmed via the
PC-2 Service Manual's LH5811 pin table (page 9) plus the real NEC
uPD1990AC datasheet (`Documents/PC1500/UPD1990AC.pdf`), and cross-checked
against real ROM1.BIN disassembly:

- **PC0** = DATA IN, **PC1** = STB, **PC2** = CLK, **PC3/PC4/PC5** =
  C0/C1/C2 (command bits) -- all six bit-banged from OPC. **PB5** = TP
  (the chip's timer-pulse output), **PB6** = DATA OUT (serial read-back)
  -- both live input levels, read unconditionally regardless of DDB (same
  treatment as PB7/ON-key).
- STB latches C0-C2 (independently for each of two groups, selected by
  C2) into a 3-bit command: C2=0 selects one of Register
  Hold/Shift/Time-Set&Hold/Time-Read; C2=1 selects TP's rate (64/256/2048
  Hz) or a test mode. Exact table: NEC uPD1990AC datasheet, "COMMAND
  SPECIFICATIONS".
- CLK shifts a 40-bit BCD register (month as a raw 4-bit binary
  1-12/day-of-week 0-6/BCD tens+units of day, hour, minute, second) one
  bit per edge, while in Register-Shift or Time-Set mode; bit 0 (LSB of
  seconds) is what appears on DATA OUT, matching the datasheet's Fig. 1
  note. Time-Read snapshots the live clock into this register; leaving
  Time-Set commits whatever's in the register back to the live clock.
- **This is what BASIC's BEEP actually depends on for its repeat-gap
  timing** -- confirmed by disassembling ROM1.BIN directly (E890-E8B4):
  before polling PB5 and IF bit 1, it issues exactly the "TP=64Hz Set"
  command (C2=1,C1=0,C0=0, i.e. value `0x20` through the shared "set OPC
  low 6 bits + pulse STB" ROM helper at E573). IF bit 1 is latched from
  TP's rising edge -- confirmed only for that bit; no ROM code path
  exercising the RTC's own IRQ/PB7-style latching (separate from this
  bit) has been traced.
- `pc1500emu`'s `Upd1990ac` (in `src/bus/bus.h`/`bus.cpp`) drives TP off
  real elapsed wall-clock time, not CPU cycles -- this chip has its own
  independent 32.768kHz crystal, so its rate is correct regardless of
  emulated CPU speed. The live clock itself defaults to matching host
  wall-clock time (no year field exists in the 40-bit register at all, so
  the host's current year is always used after a Time-Set).
- **TP produces no edges at all (and IF bit 1 never sets) until the ROM
  has issued at least one TP-rate-select command.** Found the hard way:
  an earlier version let TP start toggling from process launch using a
  guessed default rate, which spuriously set IF bit 1 during the boot
  ROM's own "NEW0?:CHECK" prompt sequence -- well before BEEP or anything
  else ever configures TP -- skipping straight past a prompt real
  hardware correctly stops at. The datasheet doesn't document TP's
  power-on-reset mux state, but real hardware clearly doesn't have this
  problem despite the RTC's divider presumably having run continuously
  for years off a backup battery, so a raw always-on TP-to-IF-bit-1
  mirror can't be the whole story. Gating on "has TP ever been
  configured" is the simplest fix that resolves the regression while
  keeping BEEP working (it always configures TP=64Hz before ever
  polling).

Registers relevant to a PC-1500 emulator (ignoring serial/cassette-only
ones — G, F, MSK/IF beyond basic IRQ, serial U/transmit):
- **DDA / DDB**: per-bit direction for PA/PB (`0`=input, `1`=output).
- **OPA**: buffer for PA — on PC-1500, PA0-7 are the keyboard column
  strobes (see Keyboard below), always driven as outputs.
- **OPB**: buffer for PB — PB7 is the ON-key input (see Keyboard); other
  PB bits are cassette-related on stock PC-1500, out of scope here.
- **OPC**: PC0-7 output-only — PC0-5 = timer control, PC6 = buzzer on/off,
  PC7 = not used (per PC-1500 I/O PC pin table, chapter 4-3-4).

### PC-1500 I/O PC pin assignment (chapter 4-3-4)

| Pin(s) | Signal | Function on stock PC-1500 |
|---|---|---|
| PA0-PA7 | key strobe | Keyboard column drive (8 lines) |
| PB0, PB1 | — | not used |
| PB2 | — | cassette serial in (out of scope) |
| PB3 | — | VCC (export) / GND (domestic) — read by the ROM's keyboard dispatch (E3F6H, F00FH bit 3) to gate whether the SML/Shift/Def-style status-toggle code at E40CH actually runs; confirmed on the export ROM, where it must read high (`Bus` forces this bit on readback -- see the SML-regression comment in `bus.cpp`) |
| PB4 | — | GND |
| PB5, PB6 | — | uPD1990AC RTC: TP (timer pulse) / DATA OUT -- see "uPD1990AC real-time clock" below |
| PB7 | ON key input | dedicated ON-key read (see Keyboard) |
| PC0-PC5 | — | uPD1990AC RTC: DATA IN/STB/CLK/C0/C1/C2 -- see "uPD1990AC real-time clock" below |
| PC6 | — | buzzer on/off control |
| PC7 | — | not used |
| CS0/CS1/CS2 | chip select | tied to AD12/AD13/(fixed) -- confirmed on real hardware (`F00AH`/`F00BH` and `B00AH`/`B00BH` read back identical, live values) that AD14/AD15 aren't part of the decode: `F000H` and `B000H` agree on bits 12-13 (`0011...`) and differ only in bit 14, so the controller mirrors to *any* address with bits 12-13 both set, regardless of bits 4-15 elsewhere. `F000H-F00FH` is just the conventional address the ROM uses, not the only one that works. |
| RS0-RS3 | register select | tied to AD0-AD3 (register map above) |
| ME1 | — | tied high so this chip lives in ME1, not ME0 |

## Keyboard

8 (strobe) × 8 (read) physical matrix, **split across two different chips**:

- **Columns (strobe/drive)**: PA0-PA7 of the LH5811 I/O port controller
  (ME1 `F00EH` = OPA register, with DDA set so all 8 bits are outputs).
  To scan, firmware drives one PA line active (per the manual's key-scan
  system-subroutine convention) and reads back row state.
- **Rows (read)**: **IN0-IN7 — direct CPU input pins**, not through the
  I/O port controller at all. Read via the `ITA` instruction
  (`IN0~7 → Accumulator`, opcode `FD BA`). This means keyboard row-sensing
  bypasses the LH5811 entirely; only the column strobe goes through it.
- **ON key**: wired separately from the 8×8 matrix, straight to the CPU's
  `BFI` pin (and mirrored on I/O-PC pin PB7 as "ON key input" for software
  to distinguish it from other keys once the system is already running).
  This lets a fully powered-down machine wake on ON-key press without any
  CPU/controller register state — pure hardware latch (`BFI` high → `BFO`
  goes high → VCC supplied). Emulating power-on/off transitions faithfully
  requires modeling this BFI/BFO path, not just another matrix bit.

The manual's key matrix diagram (labeled page 109) shows the physical
key-to-(PA column, IN row) mapping for every key, and a separate "key code
chart" gives the BASIC/software-level key-code numbering (used by
`KEYSTAT`-style system subroutines) — a different, higher-level numbering
than the raw matrix position, not needed for hardware-level emulation since
the real ROM does that translation itself from the raw matrix state.

### Physical key matrix (labeled page 109)

Fully confirmed via live hardware testing (see `pc1500_keyscan_probe.md`) —
a first pass transcribed directly off the manual page had several
misreads (small circle labels at scan resolution; my own OCR pass on that
page wasn't reliable enough to trust either), all caught and corrected by
pressing each key on a real PC-1500 and reading back the actual matrix
position. Columns are PA0-PA7, rows are IN0-IN7 (i.e. cell = key pressed
when that PA line is strobed and that IN line reads active).

| Row | PA0 | PA1 | PA2 | PA3 | PA4 | PA5 | PA6 | PA7 |
|---|---|---|---|---|---|---|---|---|
| IN0 | 2 | . | 1 | ) | + | = | ▶ | 3 |
| IN1 | 5 | - | 4 | L | * | ◄ | MODE | 6 |
| IN2 | 8 | OFF | 7 | O | / | P | CL | 9 |
| IN3 | H | S | J | K | D | F | A | G |
| IN4 | SHIFT | F1 | F5 | F6 | F2 | F3 | DEF | F4 |
| IN5 | Y | W | U | I | E | R | Q | T |
| IN6 | N | X | M | ( | C | V | Z | B |
| IN7 | ▲ | ▲<br>▼ | 0 | ENT | RCL | SPACE | SML | ▼ |

(ON key: not part of this grid — see BFI note above.)

Canonical key list (given by Paul, from the physical keyboard directly):
`OFF`, `ON`, `DEF`, `F1`-`F6`, `A`-`Z`, `=`, `(`, `)`, up, down, left,
right, `SML`, an up/down rocker (distinct from the plain up/down arrows,
found at IN7/PA1), `RCL`, `SPACE`, `ENTER`, `0`-`9`, `.`, `ENT`, `/`, `*`,
`+`, `-`, `MODE`, `CL`. All 64 matrix cells are now accounted for against
this list, with two loose ends:
- `ENTER` and `ENT` are confirmed as two genuinely distinct physical keys
  (`ENT` is a separate, smaller Enter key, located at IN7/PA3) — but the
  main `ENTER` key's own matrix position was never separately identified.
  Possibly it doesn't have a dedicated matrix cell at all (e.g. mapped to
  the same electrical position as `ENT` via a secondary keycap legend);
  unconfirmed.
- The letter `F` **is IN3/PA5** — corrected 2026-07-30, found via
  `pc1500emu`'s own text-entry keystroke simulation typing `F` as `O`
  instead (this cell's `Keyboard`/`Bus` code had `Colon` here, an earlier
  mistranscription). Confirmed against the clearer key-matrix diagram in
  the *PC-2 Service Manual*, p.19 (PDF p.22, "6. KEY & POWER SUPPLY
  CIRCUIT"), which plainly shows `F` at IN3/PA5 with no secondary legend
  drawn there at all -- so this was a straightforward misread of the
  PC-1500 Technical Reference Manual's own (lower-resolution) matrix
  diagram, not a shared-keycap situation. `:` is not a distinct matrix
  cell -- it's the Shift-tapped meaning of the `*` key (IN1/PA4), per
  `charToTapActions`/`kSymbolMap` in `src/host/main.cpp`, unrelated to
  this position.

**Three-column digit pad**: PA0, PA2, and PA7 each step by 3 down rows
IN0-IN2 (PA2: `1,4,7`; PA0: `2,5,8`; PA7: `3,6,9`) — the classic
calculator-style digit pad.

## BASIC line editor

Confirmed facts about the ROM's own line editor, gathered while building
`typeBasicProgramText`'s support for source lines longer than the raw
79-character input limit (`src/basic/text_loader.cpp`) -- this project
had zero documentation of BASIC line-editor UI behavior before this.

- **Hard 79-character raw-input limit on a fresh line.** Typing (or
  pasting, via this project's automation) more than 79 characters before
  Enter silently drops the 80th character onward, with no error shown.
  This is on the *raw, not-yet-tokenized* input buffer -- confirmed
  distinct from the line's final *stored* (tokenized) size, which can end
  up much larger via the multi-pass technique below.
- **Resuming edit on an already-stored line**: `LIST <line#>` + Enter
  redisplays that line's *detokenized* text as editable. Typing a bare
  line number + Enter instead **deletes** that line -- not a re-edit
  trick.
- **Cursor position on redisplay**: lands at the very start of the line's
  content (right after the line number), invisible until moved.
- **Reaching the end of a redisplayed line**: press Right Arrow
  repeatedly. Once genuinely at the end, further presses are a safe
  no-op -- confirmed on real hardware that the cursor simply stops
  advancing, "regardless of line length," so over-pressing (e.g. 90 times,
  comfortably past any line this project produces) is a safe way to reach
  the end without needing to compute or detect the exact redisplayed
  length. Holding the key down (auto-repeat) changes the cursor glyph from
  a block to an underscore; if already at true end-of-line, the underscore
  replaces the last character instead of appearing after it -- a real,
  human-visible tell, not modeled by this project's automation since it
  doesn't need to *see* the cursor, only reach the end reliably.
- **A line does not need to be a syntactically complete statement to be
  typed and tokenized.** E.g. `10 IF O=72` can be entered and Enter'd on
  its own (mid-condition, no `THEN`), then resumed via `LIST 10` and
  extended with `OR O=13`, and so on -- syntax is only checked at `RUN`
  time, not at Enter time. This means a pass can split *anywhere* a
  lexeme boundary allows (not just at colons between statements) --
  `typeLongLine`'s atom splitter (`splitIntoAtoms`) treats a quoted string,
  a run of letters/digits (keyword/identifier/number -- these aren't
  distinguished, since none are safe to split internally), or a `<=`/`>=`/
  `<>` operator as the smallest unbreakable unit, and packs whole atoms
  per pass; every other character (including `:`) is its own atom.
- **Continuation-pass budget is measured against stored size, not
  redisplayed length.** A resumed pass's own newly-typed raw characters,
  added to the line's *current stored* size (the `lineSize` byte in its
  on-disk record -- see the reserve-area note above for the record
  layout), is what's capped -- not the redisplayed/detokenized text length,
  which can already exceed 79 characters on screen once earlier keywords
  have been tokenized down to their compact 1-2-byte codes. Empirically
  (2026-08-09, headless tests against a real ROM dump) this cap sits
  around 77-78, but doesn't land on an exact, reproducible constant: two
  real cases from a 1984 listing (`Blackjack.bas`) with the same total
  (stored size + new characters = 77) landed on opposite sides -- one
  accepted in full, the other silently dropped its last character,
  mid-keyword (`GOSUB` -> `GOSU`). Since a fixed constant can't capture
  this precisely, `typeLongLine` treats its budget as an estimate only:
  after each pass it detokenizes the line and finds the longest prefix of
  what it just typed that actually landed; if short, it retypes exactly
  the missing remainder as the next pass (via the same `LIST`+navigate
  sequence) rather than trusting the estimate to be exact.
- **A pass ending with an unbalanced paren is rejected outright, not
  silently truncated.** Confirmed 2026-08-10 via a real-world listing
  (`DungeonQuest.bas` line 35): greedy 79-character packing lands mid-
  `A$(X,Y)`, right after the `(`, and Enter is refused -- nothing gets
  stored at all, unlike the silent-truncation case above where *something*
  always lands. This is a real tokenizer-level check at Enter time, not
  the "syntax only checked at RUN time" rule above -- paren balance is
  apparently verified per input pass regardless. `typeLongLine` doesn't
  special-case parens specifically (there may be other rules like this one
  not yet hit): it detects a pass rejected outright (nothing of it landed)
  and backs the packed atom count off by one, retrying until the ROM
  accepts a shorter split.
- **A single BASIC line's total stored size is capped independent of how
  many passes are used** (an emergent consequence of the budget above
  applying fresh on every pass) -- content that doesn't tokenize well
  (e.g. bare variable assignments, which stay almost 1:1 with raw
  characters) hits this ceiling in far fewer raw characters than
  keyword-dense content.
- **A program's total tokenized size is still capped by installed RAM,
  same as real hardware.** The stock PC-1500 has 2K of built-in RAM
  (`4000H`-`47FFH`, see the memory map above); a large real-world listing
  can exceed that once fully tokenized (confirmed: `Blackjack.bas` hits
  the ROM's own memory-full rejection partway through re-entering a line
  once the program has grown to just past `47D3H`). This is expected,
  correct ROM behavior, not a loader bug -- a real owner running a
  program this size needed an expansion module (Settings > Extension RAM
  (4800H) in this emulator; 4K/8K were real 1982-era options), which the
  ROM only detects at reset/cold-start, not on the fly, so it has to be
  configured *before* booting/typing, not after.

## LCD

- Physical display: **LF8082GE**, a 7×156 dot multi-display module (7 rows
  tall, 156 columns wide — matches the PC-1500's outline spec of "7×156 dot
  LCD").
- Driven by **four SC882G** 4-bit LCD driver/segment chips. Chips 1+3 share
  one chip-select and split an 8-bit data bus (chip 1 = D0-D3, chip 3 =
  D4-D7); chips 2+4 do the same on the other half of the display.
- **Memory-mapped, not through the I/O port controller**: the display
  buffer lives directly in ME0 at `7600H`-`77FFH` (256 bytes per chip-pair,
  512 bytes total) — plain LH5801 store instructions (`STA`, etc.) write it,
  no port-controller register involved.
- **Buffer format, fully confirmed via a systematic real-hardware
  POKE/observe walkthrough** (isolating one bit at a time, clearing
  between pokes): each of the four column-driver chips covers a
  **39-column quarter** of the 156-column width -- chip1 = columns 0-38,
  chip2 = 39-77, chip3 = 78-116, chip4 = 117-155. Chip1 and chip3 (which
  share one chip-select per the 4-2-3 schematic, V2) split the 8-bit data
  bus at `7600H`-`764DH`: chip1 = low nibble, chip3 = high nibble. Chip2
  and chip4 (V3) do the same at `7700H`-`774DH`. Within either range,
  a single column needs **two consecutive byte offsets**, not one: the
  even offset's relevant nibble holds rows 0-3 (bit0=row0 ... bit3=row3),
  the odd offset's relevant nibble holds rows 4-6 (bit0=row4 ... bit2=row6,
  top bit of that nibble unused). So chip-local column N lives at
  `base+2N` (rows 0-3) and `base+2N+1` (rows 4-6); 39 columns × 2 bytes =
  78 bytes, exactly filling each range's first 78 bytes -- matching the
  manual's own worked "display reverse" example (chapter 1; see
  `lh5801_test.cpp`'s `testManualDisplayReverseExample`), which inverts
  precisely those two 78-byte spans. (An earlier version of this doc
  concluded from that same example that each byte was one full 7-dot
  column with no nibble-splitting -- that was wrong; the example's 78-byte
  span boundaries are consistent with *either* model, and only the direct
  POKE walkthrough distinguished them. See `src/lcd/lcd.h`/`lcd.cpp` for
  the resulting `columnBits()` implementation, and `tests/lcd_test.cpp`'s
  `testConfirmedBitMapping` for the exact real-hardware observations this
  is built from.)
  Character-cell display routines address the buffer via a **cursor
  pointer at `7875H`**, valid range `00H`-`98H` (per section 5-4-5) — i.e.
  cursor addressing covers a subset of the 156 raw columns, reflecting
  character-cell (not raw-pixel) addressing for text output; the graphic
  subroutine (`EDEFH`, labeled page 134) writes one column's already-
  assembled data at a time (per its worked example using values like
  `2BH`/`5AH` that read directly as 7-bit row patterns) -- it's presumably
  the ROM's internal helper that does the nibble/byte-pair split described
  above before actually touching `7600H`-`774DH`.
- Two more relevant fixed addresses from section 5-4-5: `7880H` is a
  "parameter FF" byte controlling how the built-in `program display`
  subroutine renders numeric/string/program data (not needed for low-level
  emulation, only if the emulator ever needs to interpret BASIC-level
  display calls rather than raw buffer writes).

**Fixed-segment status indicators** (not part of the dot matrix -- small
text shown above the main display, confirmed by Paul): two bytes right
after the 78-column dot-matrix data, one per chip-pair half.
- `764EH`: bit0=Busy, bit1=Shift, bit2=Japanese (two katakana characters),
  bit3=Small, bit4=III, bit5=II, bit6=I, bit7=Def
- `764FH`: bit0=De, bit1=G, bit2=Rad (De+G together likely spell "Deg"),
  bit3=unused, bit4=Reserve, bit5=Pro, bit6=Run, bit7=unused

Not yet rendered by `Lcd`/`main.cpp` -- only the 156x7 dot matrix is drawn
today.

Still not researched: the SC882G's own command/addressing protocol (i.e.
whether the CPU's plain memory writes to `7600H-77FFH` go straight to
SC882G-internal display RAM with no separate "set column address" style
command, or whether there's a thin latch/counter in between) — moot for
the emulator's purposes now that the byte-to-column mapping above is
solidly evidenced, but would matter if real display-refresh *timing*
ever needs to be modeled.

## Other hardware noted but out of scope / deferred

- Cassette tape interface (SD0/SD1/CL0/CL1, PB2 on I/O-PC): explicitly out
  of scope per project direction. (The uPD1990AC RTC, previously listed
  here as deferred, is now implemented -- see "uPD1990AC real-time clock"
  above.)
