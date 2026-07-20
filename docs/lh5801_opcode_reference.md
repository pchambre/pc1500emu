# LH5801 Opcode Reference

Source: PC-1500 Technical Reference Manual (`/home/paul/Documents/PC1500_Technical_reference_manual.pdf`),
section 2-4 (behavioral descriptions, PDF pages 28-53 / labeled 24-49), section 2-5
"Command list" (PDF pages 59-66 / labeled 55-62: the byte/cycle/flags tables and the
mnemonic-sorted "LH5801 MNEMONIC / MACHINE LANGUAGE" opcode table).

## Cross-check pass against the manual (2026-07-20)

The opcode bytes below were cross-checked mnemonic-by-mnemonic against the manual's
byte-value table (labeled pages 60-62) and, for most instructions, independently
against the bit-level bytes in the section 2-5 command-list tables (labeled pages
55-59). Two real errors in the original draft were found and fixed:

1. **EAI is Exclusive OR, not OR.** The manual (labeled page 30, item 13) titles it
   "EAI (Exclusive or Acc and Immediate)" and gives the operation as `A ⊕ i → A`.
   The opcode byte itself (`BD i`) was already correct — only the description said
   "OR" when it should say XOR.
2. **Branch `+`/`-` is displacement sign, not condition polarity.** For BCS, BCR,
   BHS, BHR, BZS, BZR, BVS, BVR: each mnemonic has a *fixed* condition (S = branch
   when the flag is Set, R = branch when Reset/clear) per the manual (labeled pages
   46-47, confirmed by the condition-bit table on labeled page 59). The `+i` and
   `-i` forms of a *given* mnemonic both test the *same* condition — `+`/`-` only
   selects whether the displacement is added to or subtracted from P. E.g. both
   `BCS+i` and `BCS-i` branch when C=1; they differ only in branch direction. The
   original draft's framing ("each has a branch-if-true and branch-if-false form")
   was wrong, though the opcode byte assignments happened to still be correct.

**Second source used for cross-check:** `/home/paul/Documents/PC1600TechnicalReference.pdf`,
appendix 10.5 "MNEMONIC CODES OF LH-5803" (printed pages 297-301, PDF pages 305-309).
The PC-1600's main CPU is a Z80 (SC-7852); the LH-5803 is a sub-CPU handling
keyboard/LCD/timer, and per that manual's section 7.1.2: "LH-5803 is an 8-bit
CMOS CPU, which is an upper version of LH5801. Therefore, LH-5803 supports
almost all LH5801 machine language instructions, except that the SDP, RDP and
OFF instructions of LH5801 operate as a NOP instruction in LH5803." Its
appendix 10.5 command-list tables are a byte-for-byte reprint of the same
LH5801 instruction tables (better scan quality in places), making it a good
independent check — the SDP/RDP/OFF NOP-on-5803 caveat doesn't affect opcode
bytes or their meaning on a real LH5801 (PC-1500).

Confidence levels for what follows:
- **High confidence, fully resolved**: every opcode byte in the tables below,
  cross-validated against three independent tables (two in the PC-1500 manual,
  one in the PC-1600 manual), plus internal family consistency (e.g. the shared
  `1011` top nibble across all `Bx i` immediate-to-accumulator ops).
- `LDI A,i` = `B5 i` — confirmed via the PC-1600 manual's cleaner scan (reads
  unambiguously as `1011 0101`). Matches the original draft; my own PC-1500-scan
  re-read of `85 i` on one pass was a misread, not a real discrepancy.
- Conditional branch cycle counts (BCS/BCR/BHS/BHR/BZS/BZR/BVS/BVR) = **8/10/11**
  (not-taken / taken-forward / taken-backward), confirmed via the PC-1600 manual's
  cleaner scan — uniform across all eight mnemonics. My earlier read of an
  outlier "13" for BVR from the PC-1500 scan was a misread of that lower-quality
  copy, not a real difference.

## Notation used in the manual (from section 2-4-1, page 24 label)

| Symbol | Meaning |
|---|---|
| `RL` | XL, YL, or UL (low byte of X/Y/U reg) |
| `RH` | XH, YH, or UH (high byte of X/Y/U reg) |
| `Rreg` | Xreg, Yreg, or Ureg (16-bit) |
| `(Rreg)` | memory pointed to by Rreg, accessed via ME0 |
| `#(Rreg)` | memory pointed to by Rreg, accessed via ME1 |
| `(ab)` | memory at absolute 16-bit address `ab` (a=high byte, b=low byte), ME0 |
| `#(ab)` | same, but ME1 |
| `i` | 8-bit immediate operand byte |
| `i,j` | 16-bit immediate (i=high byte, j=low byte) |
| `a b` | absolute address bytes that follow the opcode in the instruction stream |

**Opcode byte format:** most instructions are a single opcode byte. Instructions
whose mnemonic/addressing-mode needs a second opcode space are prefixed with
`FD` (a genuine 2-byte opcode, `FD xx`), similar in spirit to the Z80's DD/FD
prefix mechanism. Where the table shows `a b` or `i` etc. after the opcode
byte(s), those are operand bytes that follow in memory, not part of the opcode
itself.

---

## Add / Subtract / Logical / Compare

### ADC (ADd with Carry)
| Operand | Opcode |
|---|---|
| XL | `02` |
| YL | `12` |
| UL | `22` |
| XH | `82` |
| YH | `92` |
| UH | `A2` |
| (X) | `03` |
| (Y) | `13` |
| (U) | `23` |
| (ab) | `A3 a b` |
| #(X) | `FD 03` |
| #(Y) | `FD 13` |
| #(U) | `FD 23` |
| #(ab) | `FD A3 a b` |

### ADI (ADd Immediate)
| Operand | Opcode |
|---|---|
| A, i | `B3 i` |
| (X), i | `4F i` |
| (Y), i | `5F i` |
| (U), i | `6F i` |
| (ab), i | `EF a b i` |
| #(X), i | `FD 4F i` |
| #(Y), i | `FD 5F i` |
| #(U), i | `FD 6F i` |
| #(ab), i | `FD EF a b i` |

### ADR (ADd Rreg)
| Operand | Opcode |
|---|---|
| Xreg | `FD CA` |
| Yreg | `FD DA` |
| Ureg | `FD EA` |

### AEX (Accumulator EXchange)
Swaps the high and low order nibbles in the accumulator.

| Opcode |
|---|
| `F1` |

### AND
| Operand | Opcode |
|---|---|
| (X) | `09` |
| (Y) | `19` |
| (U) | `29` |
| (ab) | `A9 a b` |
| #(X) | `FD 09` |
| #(Y) | `FD 19` |
| #(U) | `FD 29` |
| #(ab) | `FD A9 a b` |

### ANI (ANd Immediate)
| Operand | Opcode |
|---|---|
| A, i | `B9 i` |
| (X), i | `49 i` |
| (Y), i | `59 i` |
| (U), i | `69 i` |
| (ab), i | `E9 a b i` |
| #(X), i | `FD 49 i` |
| #(Y), i | `FD 59 i` |
| #(U), i | `FD 69 i` |
| #(ab), i | `FD E9 a b i` |

### AM0, AM1 (Accumulator to (timer) register, MSB 0/1)
AM0 transfers the contents of the 8-bit accumulator to the 9-bit timer register
with a 0 in the MSB. AM1 does the same, but sets a 1 for the MSB.
| Mnemonic | Opcode |
|---|---|
| AM0 | `FD CE` |
| AM1 | `FD DE` |

### ATP, ATT
ATT transfers the accumulator to the T (status) register. ATP is "access to
port": it sends the contents of the accumulator out on the data bus.
| Mnemonic | Opcode |
|---|---|
| ATP | `FD CC` |
| ATT | `FD EC` |

### Branch instructions: BCH, BCS, BCR, BHS, BHR (condition branches)
BCH is unconditional. For BCS/BCR/BHS/BHR (and BVS/BVR/BZS/BZR below), each
mnemonic tests a *fixed* condition — S = branch when the flag is Set, R =
branch when Reset/clear. **Both** the `+i` and `-i` forms of a given mnemonic
test the *same* condition; `+`/`-` only selects whether the 8-bit displacement
`i` is added to or subtracted from P (branch direction), not which condition
polarity is tested. E.g. `BCS+i` and `BCS-i` both branch when C=1, differing
only in direction. No flags change for any of these.

| Mnemonic | +i | −i |
|---|---|---|
| BCH | `8E i` | `9E i` |
| BCS | `83 i` | `93 i` |
| BCR | `81 i` | `91 i` |
| BHS | `87 i` | `97 i` |
| BHR | `85 i` | `95 i` |

### BII (Bit test Immediate)
The contents of the accumulator or memory are ANDed with the immediate value,
with the result reflected in the Z status flag.
| Operand | Opcode |
|---|---|
| A | `BF i` |
| (X) | `4D i` |
| (Y) | `5D i` |
| (U) | `6D i` |
| (ab) | `ED a b i` |
| #(X) | `FD 4D i` |
| #(Y) | `FD 5D i` |
| #(U) | `FD 6D i` |
| #(ab) | `FD ED a b i` |

### BIT
| Operand | Opcode |
|---|---|
| (X) | `0F` |
| (Y) | `1F` |
| (U) | `2F` |
| (ab) | `AF a b` |
| #(X) | `FD 0F` |
| #(Y) | `FD 1F` |
| #(U) | `FD 2F` |
| #(ab) | `FD AF a b` |

### BVS, BVR, BZS, BZR (more condition branches, `+`/`-` forms)
| Mnemonic | +i | −i |
|---|---|---|
| BVS | `8F i` | `9F i` |
| BVR | `8D i` | `9D i` |
| BZS | `8B i` | `9B i` |
| BZR | `89 i` | `99 i` |

### CDV, CIN
CIN is "compare and increment": the contents of memory at the X register
address are compared with the accumulator, status flags show the result, and
X is incremented. CDV clears the divider that provides the CPU clock,
resetting the clock.
| Mnemonic | Opcode |
|---|---|
| CDV | `FD 8E` |
| CIN | `F7` |

### CPA (ComPare Accumulator)
The contents of the accumulator are compared with a register or external
memory, with the result shown in flags C, V, Z, and H.
| Operand | Opcode |
|---|---|
| XL | `06` |
| YL | `16` |
| UL | `26` |
| XH | `86` |
| YH | `96` |
| UH | `A6` |
| (X) | `07` |
| (Y) | `17` |
| (U) | `27` |
| (ab) | `A7 a b` |
| #(X) | `FD 07` |
| #(Y) | `FD 17` |
| #(U) | `FD 27` |
| #(ab) | `FD A7 a b` |

### CPI (ComPare Immediate)
| Operand | Opcode |
|---|---|
| A, i | `B7 i` |
| XL, i | `4E i` |
| YL, i | `5E i` |
| UL, i | `6E i` |
| XH, i | `4C i` |
| YH, i | `5C i` |
| UH, i | `6C i` |

### DCA (DeCimal Add) / DCS (DeCimal Subtract)
| Mnemonic | Operand | Opcode |
|---|---|---|
| DCA | (X) | `8C` |
| DCA | (Y) | `9C` |
| DCA | (U) | `AC` |
| DCA | #(X) | `FD 8C` |
| DCA | #(Y) | `FD 9C` |
| DCA | #(U) | `FD AC` |
| DCS | (X) | `0C` |
| DCS | (Y) | `1C` |
| DCS | (U) | `2C` |
| DCS | #(X) | `FD 0C` |
| DCS | #(Y) | `FD 1C` |
| DCS | #(U) | `FD 2C` |

## Increment / Decrement / Register moves

### DEC
| Operand | Opcode |
|---|---|
| A | `DF` |
| XL | `42` |
| YL | `52` |
| UL | `62` |
| XH | `FD 42` |
| YH | `FD 52` |
| UH | `FD 62` |
| X | `46` |
| Y | `56` |
| U | `66` |

### INC
| Operand | Opcode |
|---|---|
| A | `DD` |
| XL | `40` |
| YL | `50` |
| UL | `60` |
| XH | `FD 40` |
| YH | `FD 50` |
| UH | `FD 60` |
| X | `44` |
| Y | `54` |
| U | `64` |

### DRL, DRR (Digit Rotate Left/Right)
DRL: the low order 4 bits of external memory are moved to the high order 4
bits of external memory, the high order 4 bits of external memory are moved
to the high order 4 bits of the accumulator, and the low order 4 bits of the
accumulator are moved to the low order 4 bits of external memory.

DRR is in the other direction: the low order 4 bits of external memory are
moved to the low order 4 bits of the accumulator, the high order 4 bits of
external memory are moved to the low order 4 bits of external memory, and the
high order 4 bits of external memory are moved to the high order 4 bits of
the accumulator.

| Mnemonic | Operand | Opcode |
|---|---|---|
| DRL | (X) | `D7` |
| DRL | #(X) | `FD D7` |
| DRR | (X) | `D3` |
| DRR | #(X) | `FD D3` |

### EAI (Exclusive or Acc and Immediate)
Immediate value is XORed with the accumulator and the result stored in the
accumulator. Only flag Z changes.

| Opcode |
|---|
| `BD i` |

### EOR (Exclusive OR)
| Operand | Opcode |
|---|---|
| (X) | `0D` |
| (Y) | `1D` |
| (U) | `2D` |
| (ab) | `AD a b` |
| #(X) | `FD 0D` |
| #(Y) | `FD 1D` |
| #(U) | `FD 2D` |
| #(ab) | `FD AD a b` |

### HLT (Halt)
| Opcode |
|---|
| `FD B1` |

### ITA (Input port Transfer to Accumulator)
Transfers the contents of the input port to the accumulator.
| Opcode |
|---|
| `FD BA` |

### JMP (absolute JuMP)
| Opcode |
|---|
| `BA i j` (16-bit target address) |

## Load / Store

### LDA (LoaD Accumulator or register)
| Operand | Opcode |
|---|---|
| XL | `04` |
| YL | `14` |
| UL | `24` |
| XH | `84` |
| YH | `94` |
| UH | `A4` |
| (X) | `05` |
| (Y) | `15` |
| (U) | `25` |
| (ab) | `A5 a b` |
| #(X) | `FD 05` |
| #(Y) | `FD 15` |
| #(U) | `FD 25` |
| #(ab) | `FD A5 a b` |

### LDI (LoaD Immediate)
| Operand | Opcode |
|---|---|
| A, i | `B5 i` |
| XL, i | `4A i` |
| YL, i | `5A i` |
| UL, i | `6A i` |
| XH, i | `48 i` |
| YH, i | `58 i` |
| UH, i | `68 i` |
| S, i,j | `AA i j` |

### LDE (LoaD then decrEment)
The accumulator is loaded from external memory using the R register address,
and R is then decremented.
| Operand | Opcode |
|---|---|
| X | `47` |
| Y | `57` |
| U | `67` |

### LDX (LoaD X register from Rreg)
Loads the X register from the source register R, where R is encoded in the
upper nibble of the second opcode byte (lower nibble fixed at `8`): `0`=X,
`1`=Y, `2`=U, `4`=S, `5`=P. So `FD 08` is X→X (effectively a no-op), `FD 18`
is Y→X, `FD 28` is U→X, `FD 48` is S→X, `FD 58` is P→X. The "Source" column
below is the source register R, not the destination (which is always X).

| Source (R) | Opcode |
|---|---|
| X | `FD 08` |
| Y | `FD 18` |
| U | `FD 28` |
| S | `FD 48` |
| P | `FD 58` |

### LIN (Load and INcrement)
The contents of external memory addressed by the R register are transferred
to the accumulator, and the register is incremented.
| Operand | Opcode |
|---|---|
| X | `45` |
| Y | `55` |
| U | `65` |

### LOP (LOoP)
Decrements UL. If a borrow is not triggered, jumps back `i` bytes from the
stack pointer (a loop-decrement-and-branch instruction). If a borrow is
triggered (UL underflows past 0), execution just continues to the next
instruction — i.e. the loop exits.
| Operand | Opcode |
|---|---|
| UL, i | `88 i` |

### NOP
| Opcode |
|---|
| `38` |

### OFF (BF flip-flop reset)
Toggles the state of BFI, which has something to do with input vs. output
(rather than system power-off, despite the mnemonic).
| Opcode |
|---|
| `FD 4C` |

### ORA (OR Accumulator with memory)
| Operand | Opcode |
|---|---|
| (X) | `0B` |
| (Y) | `1B` |
| (U) | `2B` |
| (ab) | `AB a b` |
| #(X) | `FD 0B` |
| #(Y) | `FD 1B` |
| #(U) | `FD 2B` |
| #(ab) | `FD AB a b` |

### ORI (OR Immediate)
| Operand | Opcode |
|---|---|
| A, i | `BB i` |
| (X), i | `4B i` |
| (Y), i | `5B i` |
| (U), i | `6B i` |
| (ab), i | `EB a b i` |
| #(X), i | `FD 4B i` |
| #(Y), i | `FD 5B i` |
| #(U), i | `FD 6B i` |
| #(ab), i | `FD EB a b i` |

### POP
| Operand | Opcode |
|---|---|
| A | `FD 8A` |
| X | `FD 0A` |
| Y | `FD 1A` |
| U | `FD 2A` |

### PSH (PuSH)
| Operand | Opcode |
|---|---|
| A | `FD C8` |
| X | `FD 88` |
| Y | `FD 98` |
| U | `FD A8` |

### RDP (Reset Display flip-flop)
Resets the display on/off control flip-flop.

| Opcode |
|---|
| `FD C0` |

### REC (REset Carry)
Resets the carry flag.
| Opcode |
|---|
| `F9` |

### RIE (Reset Interrupt Enable)
Resets the interrupt enable flag. Once reset, maskable and timer interrupts
are disabled.

| Opcode |
|---|
| `FD BE` |

### ROL, ROR (ROtate Left/Right)
| Mnemonic | Opcode |
|---|---|
| ROL | `DB` |
| ROR | `D1` |

### RPU (Reset PU flip-flop)
Resets the general purpose flip-flop PU (counterpart to SPU).

| Opcode |
|---|
| `E3` |

### RPV (Reset PV flip-flop)
Resets the general purpose flip-flop PV (counterpart to SPV).

| Opcode |
|---|
| `B8` |

### RTI, RTN (ReTurn from Interrupt / ReTurn)
| Mnemonic | Opcode |
|---|---|
| RTI | `8A` |
| RTN | `9A` |

### SBC (SuBtract with Carry)
| Operand | Opcode |
|---|---|
| XL | `00` |
| YL | `10` |
| UL | `20` |
| XH | `80` |
| YH | `90` |
| UH | `A0` |
| (X) | `01` |
| (Y) | `11` |
| (U) | `21` |
| (ab) | `A1 a b` |
| #(X) | `FD 01` |
| #(Y) | `FD 11` |
| #(U) | `FD 21` |
| #(ab) | `FD A1 a b` |

### SBI (SuBtract Immediate)
| Operand | Opcode |
|---|---|
| A, i | `B1 i` |

### SDE (Store then DEcrement)
The accumulator is stored at the address referenced by R, and the R register
is then decremented.
| Operand | Opcode |
|---|---|
| X | `43` |
| Y | `53` |
| U | `63` |

### SDP (Set Display flip-flop)
Sets the display on/off control flip-flop (counterpart to RDP).
| Opcode |
|---|
| `FD C1` |

### SEC (SEt Carry)
| Opcode |
|---|
| `FB` |

### SHL, SHR (SHift Left/Right)
| Mnemonic | Opcode |
|---|---|
| SHL | `D9` |
| SHR | `D5` |

### SIE (Set Interrupt Enable)
| Opcode |
|---|
| `FD 81` |

### SIN (Store then INcrement)
Like SDE, but increments the R register instead of decrementing.
| Operand | Opcode |
|---|---|
| X | `41` |
| Y | `51` |
| U | `61` |

### SJP (Subroutine JumP / call)
| Opcode |
|---|
| `BE i j` (16-bit target address) |

### SPU (Set PU flip-flop)
Sets the general purpose flip-flop PU (counterpart to RPU).

| Opcode |
|---|
| `E1` |

### SPV (Set PV flip-flop)
Sets the general purpose flip-flop PV (counterpart to RPV).

| Opcode |
|---|
| `A8` |

### STA (STore Accumulator or register)
Uses a 2-bit R field (X/Y/U select) embedded in the opcode for the
register-indexed forms (XL/YL/UL, XH/YH/UH, (X)/(Y)/(U), #(X)/#(Y)/#(U)) —
the same 2-bit R model that STX uses.
| Operand | Opcode |
|---|---|
| XL | `0A` |
| YL | `1A` |
| UL | `2A` |
| XH | `08` |
| YH | `18` |
| UH | `28` |
| (X) | `0E` |
| (Y) | `1E` |
| (U) | `2E` |
| (ab) | `AE a b` |
| #(X) | `FD 0E` |
| #(Y) | `FD 1E` |
| #(U) | `FD 2E` |
| #(ab) | `FD AE a b` |

### STX (STore X register to Rreg)
Similar in spirit to LDX — the opcode encodes a destination register R — but
here only 2 bits (not a full nibble) are used for the R field. STA follows
this same 2-bit R model (see note there).
| Operand | Opcode |
|---|---|
| X | `FD 4A` |
| Y | `FD 5A` |
| U | `FD 6A` |
| S | `FD 4E` |
| P | `FD 5E` |

### TIN (Transfer and INcrement)
Transfers the memory value at the address in the X register to the location
addressed by the Y register, then increments both registers. A useful
building block for memcpy-style operations.
| Opcode |
|---|
| `F5` |

### TTA (Transfer T to Accumulator)
Transfers the contents of the T status register to the accumulator (the
opposite direction of ATT).
| Opcode |
|---|
| `FD AA` |

### VCS, VCR (conditional Vector Subroutine jump on Carry Set/Reset)
Conditional vector subroutine jump: same operation as VMJ (jump to `FF00+i`,
pushing the return address to the stack) but only taken if the condition
holds; otherwise execution just continues to the next instruction. VCS
triggers if C=1; VCR triggers if C=0. Unlike VEJ, the vector low byte `i` is
a separate immediate operand byte, not the opcode itself, so `i` can be any
value `00`-`F6` (even), not tied to which of these mnemonics is used.
| Mnemonic | Opcode |
|---|---|
| VCS | `C3 i` |
| VCR | `C1 i` |

### VEJ (VEctor subroutine Jump, one-byte)
A one-byte subroutine call: pushes the program counter to the stack, then
jumps to the address indicated by a two-byte vector — high byte fixed at
`FF`, low byte equal to the opcode byte itself (the operand IS the opcode).
The Z flag is reset. Since the opcode doubles as the vector index, this gives
28 possible fixed subroutine-vector-table addresses in the range `FFC0`-`FFF6`
(only even values). RTN returns from the subroutine as usual. Opcode values
(all even, `C0`-`F6`):
`C0, C2, C4, C6, C8, CA, CC, CE, D0, D2, D4, D6, D8, DA, DC, DE, E0, E2, E4, E6, E8, EA, EC, EE, F0, F2, F4, F6`

### VMJ (Vector 2-byte subroutine JMp) and conditional variants (VVS, VZS, VZR, VHR, VHS)
VMJ is an unconditional subroutine call: pushes the return address to the
stack, then jumps to the two-byte vector address `FF00+i` (`i` is a separate
immediate operand byte, an even value `00`-`F6`). The Z flag is reset.

VVS, VZS, VZR, VHR, VHS are the conditional counterparts — same operation as
VMJ, but only taken if the named condition holds; otherwise execution
continues to the next instruction:
- VVS: jump if V=1
- VZS: jump if Z=1
- VZR: jump if Z=0
- VHR: jump if H=0
- VHS: jump if H=1

(Together with VCS/VCR above, that's a full set of conditional vector calls
across C, Z, V, and H.)

| Mnemonic | Opcode |
|---|---|
| VMJ | `CD i` |
| VVS | `CF i` |
| VZS | `CB i` |
| VZR | `C9 i` |
| VHR | `C5 i` |
| VHS | `C7 i` |

---

## Flags affected, byte length, and cycle counts

Transcribed from the manual's "8-bit CPU command list (1)-(5)" tables (PC-1500
manual labeled pages 55-59 / PDF 59-63; cross-checked against the PC-1600
manual's appendix 10.5 reprint, PDF pages 306-309 — both agree on every value
below). Flags column lists which of C, V, H, Z change (IE noted separately
where relevant); "—" means no flag change. Cycle counts with two or three
values are `not-taken/taken` or `not-taken/taken-forward/taken-backward` for
branch-type instructions.

### Arithmetic / logical

| Mnemonic | Operand | Flags | Bytes | Cycles |
|---|---|---|---|---|
| ADC | RL / RH | C,V,H,Z | 1 | 6 |
| ADC | (R) | C,V,H,Z | 1 | 7 |
| ADC | (ab) | C,V,H,Z | 3 | 13 |
| ADC | #(R) | C,V,H,Z | 2 | 11 |
| ADC | #(ab) | C,V,H,Z | 4 | 17 |
| ADI | A,i | C,V,H,Z | 2 | 7 |
| ADI | (R),i | C,V,H,Z | 2 | 13 |
| ADI | (ab),i | C,V,H,Z | 4 | 19 |
| ADI | #(R),i | C,V,H,Z | 3 | 17 |
| ADI | #(ab),i | C,V,H,Z | 5 | 23 |
| DCA | (R) | C,V,H,Z | 1 | 15 |
| DCA | #(R) | C,V,H,Z | 2 | 19 |
| ADR | Rreg | C,V,H,Z | 2 | 11 |
| SBC | RL / RH | C,V,H,Z | 1 | 6 |
| SBC | (R) | C,V,H,Z | 1 | 7 |
| SBC | (ab) | C,V,H,Z | 3 | 13 |
| SBC | #(R) | C,V,H,Z | 2 | 13 |
| SBC | #(ab) | C,V,H,Z | 4 | 17 |
| SBI | A,i | C,V,H,Z | 2 | 7 |
| DCS | (R) | C,V,H,Z | 1 | 13 |
| DCS | #(R) | C,V,H,Z | 2 | 17 |
| AND | (R) | Z | 1 | 7 |
| AND | (ab) | Z | 3 | 13 |
| AND | #(R) | Z | 2 | 11 |
| AND | #(ab) | Z | 4 | 17 |
| ANI | A,i | Z | 2 | 7 |
| ANI | (R),i | Z | 2 | 13 |
| ANI | (ab),i | Z | 4 | 19 |
| ANI | #(R),i | Z | 3 | 17 |
| ANI | #(ab),i | Z | 5 | 23 |
| ORA | (R) | Z | 1 | 7 |
| ORA | (ab) | Z | 3 | 13 |
| ORA | #(R) | Z | 2 | 11 |
| ORA | #(ab) | Z | 4 | 17 |
| ORI | A,i | Z | 2 | 7 |
| ORI | (R),i | Z | 2 | 13 |
| ORI | (ab),i | Z | 4 | 19 |
| ORI | #(R),i | Z | 3 | 17 |
| ORI | #(ab),i | Z | 5 | 23 |
| EOR | (R) | Z | 1 | 7 |
| EOR | (ab) | Z | 3 | 13 |
| EOR | #(R) | Z | 2 | 11 |
| EOR | #(ab) | Z | 4 | 17 |
| EAI | i | Z | 2 | 7 |
| INC | A / RL | C,V,H,Z | 1 | 5 |
| INC | RH | C,V,H,Z | 2 | 9 |
| INC | R (16-bit) | — | 1 | 5 |
| DEC | A / RL | C,V,H,Z | 1 | 5 |
| DEC | RH | C,V,H,Z | 2 | 9 |
| DEC | R (16-bit) | — | 1 | 5 |

### Compare and bit test

| Mnemonic | Operand | Flags | Bytes | Cycles |
|---|---|---|---|---|
| CPA | RL / RH | C,V,H,Z | 1 | 6 |
| CPA | (R) | C,V,H,Z | 1 | 7 |
| CPA | (ab) | C,V,H,Z | 3 | 13 |
| CPA | #(R) | C,V,H,Z | 2 | 11 |
| CPA | #(ab) | C,V,H,Z | 4 | 17 |
| CPI | RL,i / RH,i / A,i | C,V,H,Z | 2 | 7 |
| BIT | (R) | Z | 1 | 7 |
| BIT | (ab) | Z | 3 | 13 |
| BIT | #(R) | Z | 2 | 11 |
| BIT | #(ab) | Z | 4 | 17 |
| BII | A,i | Z | 2 | 7 |
| BII | (R),i | Z | 2 | 10 |
| BII | (ab),i | Z | 4 | 16 |
| BII | #(R),i | Z | 3 | 14 |
| BII | #(ab),i | Z | 5 | 20 |

### Load and store

| Mnemonic | Operand | Flags | Bytes | Cycles |
|---|---|---|---|---|
| LDA | RL / RH | Z | 1 | 5 |
| LDA | (R) | Z | 1 | 6 |
| LDA | (ab) | Z | 3 | 12 |
| LDA | #(R) | Z | 2 | 10 |
| LDA | #(ab) | Z | 4 | 16 |
| LDE | R | Z | 1 | 6 |
| LIN | R | Z | 1 | 6 |
| LDI | RL,i | — | 2 | 6 |
| LDI | RH,i | — | 2 | 5 |
| LDI | A,i | Z | 2 | 6 |
| LDI | S,i,j | — | 3 | 12 |
| LDX | R / S / P | — | 2 | 11 |
| STA | RL,RH,(R),(ab),#(R),#(ab) | — | 1/1/1/3/2/4 | 5/5/6/12/10/16 |
| SDE | R | — | 1 | 6 |
| SIN | R | — | 1 | 6 |
| STX | R / S / P | — | 2 | 11 |
| PSH | A | — | 2 | 11 |
| PSH | R | — | 2 | 14 |
| POP | A | Z | 2 | 12 |
| POP | R | — | 2 | 15 |
| ATT | | C,V,H,Z,IE | 2 | 9 |
| TTA | | Z | 2 | 9 |
| TIN | | — | 1 | 7 |
| CIN | | C,V,H,Z | 1 | 7 |

### Rotate, shift, and CPU control

| Mnemonic | Flags | Bytes | Cycles |
|---|---|---|---|
| ROL | C,V,H,Z | 1 | 8 |
| ROR | C,V,H,Z | 1 | 9 |
| SHL | C,V,H,Z | 1 | 6 |
| SHR | C,V,H,Z | 1 | 9 |
| DRL (X) / #(X) | — | 1 / 2 | 12 / 16 |
| DRR (X) / #(X) | — | 1 / 2 | 12 / 16 |
| AEX | — | 1 | 6 |
| AM0 / AM1 | — | 2 | 9 |
| CDV | — | 2 | 8 |
| ATP | — | 2 | 9 |
| SDP / RDP | — | 2 | 8 |
| SPU / RPU / SPV / RPV | — | 1 | 4 |
| ITA | Z | 2 | 9 |
| RIE | IE→0 | 2 | 8 |
| SIE | IE→1 | 2 | 8 |
| HLT | — | 2 | 9 |
| OFF | — | 2 | 8 |
| NOP | — | 1 | 5 |
| SEC | C→1 | 1 | 4 |
| REC | C→0 | 1 | 4 |

### Jump, call, and return

| Mnemonic | Flags | Bytes | Cycles |
|---|---|---|---|
| JMP | — | 3 | 12 |
| BCH | — | 2 | 8 (+i) / 9 (-i) |
| BCS / BCR / BHS / BHR / BZS / BZR / BVS / BVR | — | 2 | 8 (not taken) / 10 (taken +i) / 11 (taken -i) |
| LOP | — | 2 | 8 (borrow) / 11 (no borrow, branch taken) |
| SJP | — | 3 | 19 |
| VEJ | Z→0 | 1 | 17 |
| VCS / VCR / VHS / VHR / VZS / VZR / VVS | Z→0 when taken | 2 | 8 (not taken) / 21 (taken) |
| VMJ | Z→0 | 2 | 20 |
| RTN | — | 1 | 11 |
| RTI | C,V,H,Z,IE restored | 1 | 14 |

Notes:
- STA's byte/cycle row is one line per operand form in encoding order
  (RL, RH, (R), (ab), #(R), #(ab)) since they share identical flag behavior
  (none) — same pattern as LDA above it, just written compactly.
- LOP's cycle mapping (which of 8/11 is "borrow" vs "no borrow") is transcribed
  directly from the manual; worth a sanity-check once the LH5801 core has a
  test harness, since it's the one place I inferred the taken/not-taken
  labeling from context rather than reading an explicit label.
