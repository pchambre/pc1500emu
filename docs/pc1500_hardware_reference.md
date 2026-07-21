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
| `4000H`-`47FFH` | Standard user RAM (built-in, 2KB, chip HM6116) |
| `4800H`-`67FFH` | Option user memory (module unit, further banks) |
| `6800H`-`6FFFH` | Unused ("do not use" per manual) |
| `7000H`-`75FFH` | Inhibited |
| `7600H`-`76FFH` | Display buffer, chips 1 & 3 (see LCD section) |
| `7700H`-`77FFH` | Display buffer, chips 2 & 4 |
| `7800H`-`7BFFH` | System RAM (fixed variable area) |
| `7C00H`-`7FFFH` | (within the same chip-select block as system RAM; exact use not detailed) |
| `8000H`-`BFFFH` | CE-150/CE-153/CE-158 system program + I/O PC (only present if that peripheral is connected) |
| `C000H`-`FFFFH` | PC-1500 system ROM (16KB, chip SC61328F) |

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
| CS0/CS1/CS2 | chip select | tied to AD12/AD13/(fixed), decode to F000H-F00FH in ME1 |
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

Transcribed directly off the manual by Paul (my own OCR pass on this page
wasn't reliable enough to trust — small circle labels at scan resolution).
`?` marks a position neither of us could read confidently; fill in once a
sharper scan or a real PC-1500 photo is available. Columns are PA0-PA7,
rows are IN0-IN7 (i.e. cell = key pressed when that PA line is strobed and
that IN line reads active).

| Row | PA0 | PA1 | PA2 | PA3 | PA4 | PA5 | PA6 | PA7 |
|---|---|---|---|---|---|---|---|---|
| IN0 | 2 | ? | 1 | ? | + | - | ▶ | 3 |
| IN1 | 5 | ? | 4 | ? | * | ◄ | MODE | E |
| IN2 | 8 | OFF | 7 | C | ? | P | CL | ? |
| IN3 | H | S | J | K | D | : | A | ? |
| IN4 | SHIFT | F1 | F5 | F6 | F2 | F3 | DEF | F4 |
| IN5 | Y | W | ? | ? | ? | ? | ? | ? |
| IN6 | ? | X | M | ? | ? | ? | / | ? |
| IN7 | ? | ? | ? | ENTER | RCL | ? | SML | . |

(ON key: not part of this grid — see BFI note above.)

Canonical key list (given by Paul, from the physical keyboard directly —
use this to sanity-check the matrix table above): `OFF`, `ON`, `DEF`,
`F1`-`F6`, `A`-`Z`, `=`, `(`, `)`, up, down, left, right, `SML`, an
up/down rocker (distinct from the plain up/down arrows), `RCL`, `SPACE`,
`ENTER`, `0`-`9`, `.`, `ENT`, `/`, `*`, `+`, `-`, `MODE`, `CL`. Note `ENTER`
and `ENT` are both listed as distinct — not yet reconciled; could be a
duplicate mention or two genuinely different keys (e.g. a scientific-
notation exponent key). Worth clarifying before finalizing.

**One pattern worth checking against the actual page**: columns PA2 and
PA0 both step by 3 down rows IN0-IN2 (PA2: `1,4,7`; PA0: `2,5,8`) — the
classic three-column digit-pad layout. PA7 fits the same pattern for its
first entry (IN0/PA7 = `3`), which would predict IN1/PA7 = `6` and
IN2/PA7 = `9` to complete `3,6,9`. IN2/PA7 is currently blank (`?`), so
`9` is a reasonable guess there. IN1/PA7 is currently recorded as `E`
though, not `6` — worth a second look at that one specific cell, since
`E` and `6` are easy to confuse at scan resolution and this is the only
cell breaking an otherwise clean pattern. Not changing either cell based
on inference alone; flagging for direct confirmation.

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
- **Buffer format**: confirmed via the manual's "Graphic display" BASIC
  system-subroutine documentation (entry `EDEFH`, labeled page 134) — each
  byte written is one **column** of dots; bit 0 = topmost of the 7 visible
  rows, bit 6 = bottommost (bit 7 unused, only 7 rows exist). This is
  consistent with 156 columns × 7 rows = the panel's native resolution.
  Character-cell display routines address the buffer via a **cursor
  pointer at `7875H`**, valid range `00H`-`98H` (per section 5-4-5) — i.e.
  cursor addressing covers a subset of the 156 raw columns, reflecting
  character-cell (not raw-pixel) addressing for text output; the graphic
  subroutine writes raw columns directly.
- Two more relevant fixed addresses from section 5-4-5: `7880H` is a
  "parameter FF" byte controlling how the built-in `program display`
  subroutine renders numeric/string/program data (not needed for low-level
  emulation, only if the emulator ever needs to interpret BASIC-level
  display calls rather than raw buffer writes).

Not yet researched: the SC882G's own command/addressing protocol (i.e.
whether the CPU's plain memory writes to `7600H-77FFH` go straight to
SC882G-internal display RAM with no separate "set column address" style
command, or whether there's a thin latch/counter in between). Given the
manual describes these addresses as plain memory (chip-selected exactly
like RAM, in the ME0 chip-select truth table), the working assumption is
**direct-mapped**: writing byte N to `7600H + col` (or `7700H + col`) sets
that column's 7 dots on chip 1/2 respectively (packed nibble scheme:
`7600H-76FFH` byte = {chip3 high nibble, chip1 low nibble} for the
first half of the display, `7700H-77FFH` similarly for chips 2/4).
**This nibble-packing detail is an inference from the chip-pairing text
in chapter 4-2-1, not independently confirmed against a chip datasheet —
worth validating once bring-up testing is possible against real ROM
behavior (task: Integration).**

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
