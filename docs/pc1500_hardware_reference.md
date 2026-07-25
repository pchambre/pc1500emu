# PC-1500 Hardware Reference

Source: PC-1500 Technical Reference Manual
(`/home/paul/Documents/PC1500_Technical_reference_manual.pdf`), chapter 3
"LH5810/LH5811 I/O port controller" (labeled pages 67-84 / PDF 71-88), chapter
4 "PC-1500 hardware description" (labeled pages 85-110 / PDF 89-114), and
section 5-4-5 "Display" (labeled pages 131-134 / PDF 135-138).

Cassette interface is intentionally excluded (out of scope for this
emulator) — pin/register facts that only pertain to cassette are omitted
even where the manual documents them (e.g. CE-150's built-in cassette port).

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

**Gotcha, learned the hard way**: `4000H`-`47FFH` being "standard user RAM"
at the chip-select level does *not* mean all of it is free scratch space.
On a bare PC-1500 (no CE-151/CE-155/CE-159 module), the BASIC ROM firmware
uses `4000H`-`40C4H` as its "reserve area" (section 5-3-6 of the manual) —
a status marker, a pointer to the BASIC program, and the F1-F6
key-reassignment table live there, and the actual BASIC program only
starts at `40C5H`. Overwriting that range (e.g. with a hand-POKEd ML
routine) doesn't fail the POKE — it's still plain RAM — but corrupts state
the interpreter depends on. See `pc1500_keyscan_probe.md` for how this bit
us in practice. Safe scratch space for small ML routines is somewhere
comfortably above `40C5H` + whatever the current BASIC program/variables
occupy.

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
- Serial data transfer control (cassette-related on PC-1500 — out of scope).
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
| PB3 | — | VCC (export) / GND (domestic) — no logical function |
| PB4 | — | GND |
| PB5, PB6 | — | timer control |
| PB7 | ON key input | dedicated ON-key read (see Keyboard) |
| PC0-PC5 | — | timer control |
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
| IN3 | H | S | J | K | D | : | A | G |
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
- The letter `F` never showed up as its own matrix cell either — only
  `F1`-`F6`. Likely a secondary legend sharing a keycap with something
  already placed (mirroring how `F1`-`F6` themselves visually share
  keycaps with the digit/`SHIFT`/`SML` row per the manual's separate
  "key code chart"), rather than a distinct 65th position that a strict
  8×8=64 matrix couldn't have anyway.

**Three-column digit pad**: PA0, PA2, and PA7 each step by 3 down rows
IN0-IN2 (PA2: `1,4,7`; PA0: `2,5,8`; PA7: `3,6,9`) — the classic
calculator-style digit pad.

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

- Timer IC **µPD1990AC** (32.768kHz crystal) — drives real-time clock and
  timer interrupts. Controlled via I/O-PC pins PB5/PB6/PC0-5. Not detailed
  further here; revisit when implementing interrupts/timer if BASIC-level
  timer behavior needs to be accurate.
- Buzzer: single on/off control bit, I/O-PC pin PC6. Trivial to add once
  the I/O port controller's OPC register is emulated — no separate research
  needed.
- Cassette tape interface (SD0/SD1/CL0/CL1, PB2 on I/O-PC): explicitly out
  of scope per project direction.
