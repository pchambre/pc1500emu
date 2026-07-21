# PC-1500 keyboard matrix scan probe

A tiny BASIC + machine-language tool to run on a real PC-1500 to empirically
fill in the rest of the key matrix table in `pc1500_hardware_reference.md`,
rather than reading it off the (illegible-in-places) manual scan.

## Type this into the PC-1500 in BASIC

```basic
1 WAIT 0
5 FOR I=0 TO 47:READ D:POKE 17408+I,D:NEXT I
6 DATA 181,255,253,174,240,12,74,0,88,68,90,40,85,253,174,240,14,253,186,183
7 DATA 255,137,9,64,78,8,153,16,181,255,74,255,174,68,129,4,174,68,128,154
8 DATA 254,253,251,247,239,223,191,127
10 CALL 17408
20 PRINT 255-PEEK(17537);PEEK(17536)
30 GOTO 10
```

Line 1 (`WAIT 0`) disables the default post-`PRINT` pause that Sharp
pocket-computer BASICs use to give you time to read the tiny single-line
display — without it, each `PRINT` was returning to immediate mode after
one pass rather than looping. `WAIT 0` should be a persistent setting once
executed, so it only needs to run once at the top.

Line 20 prints **row value, then column** (row first). `255-PEEK(17537)`
turns the raw row byte (one bit low, rest high) back into a plain power of
two: `1,2,4,8,16,32,64,128` for `IN0`-`IN7` respectively — much easier to
eyeball than the complemented byte, and needs no changes to the machine
code or the `DATA` already typed in. (For no key pressed it prints `0`,
since `255-255=0`.)

There's also a machine-code version that computes the row index directly
(no BASIC-side math at all) — see "What it loads" below. It's documented
for reference but wasn't the one actually used, since it would have meant
retyping the already-entered `DATA` lines for no real benefit once the
one-line `PRINT` fix above works just as well.

(Two earlier revisions of this had bugs. First, the strobe table was at a
separate address [`4090H`], needing two POKE loops — fixed by putting it
right after the code at `4028H`. Second, and more seriously: the whole
block was loaded at `4000H`/16384, which on a bare PC-1500 with no memory
module is the **leading address of the "reserve area"** — live system data
the ROM's BASIC interpreter depends on (a status marker, a pointer to your
program, the F1-F6 key-reassignment table), not free RAM. Overwriting it
doesn't fail the POKE/PEEK itself — it's still normal RAM — but it corrupts
that structure, which is why it ran once and then died. Manual reference:
section 5-3-6 "Structure of reserve area," which gives the reserve area's
leading address as `4000H` for a PC-1500-only configuration (no CE-151/
CE-155/CE-159). Everything now loads at `4400H` instead — comfortably
clear of that region and of wherever the tiny loader program itself ends
up (`40C5H` onward), with headroom below the top of the 2KB RAM (`47FFH`)
too. Only 3 of the 48 bytes actually differ from the `4000H` version — the
page-high-byte, everywhere it's used as part of an address rather than as
an instruction opcode.)

Run it (`RUN`), then press keys one at a time. Each press prints two
numbers: **a power of two (1,2,4,8,16,32,64,128, matching IN0-IN7
respectively)**, then **column index (0-7, matching PA0-PA7)**. If no key
is pressed during a pass it prints `0 0`.

To stop: `BREAK`/`ON` should interrupt it (standard PC-1500 BASIC behavior),
since `GOTO 10` loops forever otherwise.

**If it never shows anything but `0 0` while a key is held**, the strobe
polarity assumption is backwards for this hardware. Fix: change the last 8
values in line 8's `DATA` (currently `254,253,251,247,239,223,191,127`) to
`1,2,4,8,16,32,64,128` (active-high strobe) — but try it as-is first;
active-low with pulled-up rows is the standard convention for this class
of chip.

Runs in a tight loop, so a held key prints repeatedly — that's expected,
just move to the next key once you've noted one reading.

## What the running version loads (annotated, for reference/verification)

Loaded at `4400H` (17408 decimal) — clear of the reserve area (`4000H`-
`40C4H`) and of the BASIC program area starting at `40C5H`. Results land at
`4480H`/`4481H` (17536/17537); the strobe table immediately follows the
code at `4428H`.

```
4400: B5 FF          LDI  A,0FFH
4402: FD AE F0 0C    STA  #(0F00CH)     ; DDA = FFH  (PA0-7 all output)
4406: 4A 00          LDI  XL,00H        ; XL = column counter
4408: 58 44          LDI  YH,44H
440A: 5A 28          LDI  YL,28H        ; Y = strobe table (4428H)
440C: 55             LIN  Y             ; L1: A = strobe byte, Y++
440D: FD AE F0 0E    STA  #(0F00EH)     ; OPA = A  (drive this column)
4411: FD BA          ITA                ; A = IN0..7
4413: B7 FF          CPI  A,0FFH
4415: 89 09          BZR  +9            ; -> FOUND if A != FFH (key hit)
4417: 40             INC  XL
4418: 4E 08          CPI  XL,08H
441A: 99 10          BZR  -10H          ; -> L1 if XL != 8 (keep scanning)
441C: B5 FF          LDI  A,0FFH        ; no key found this pass
441E: 4A FF          LDI  XL,0FFH
4420: AE 44 81       STA  (4481H)       ; FOUND: save row byte
4423: 04             LDA  XL
4424: AE 44 80       STA  (4480H)       ; save column index
4427: 9A             RTN

4428: FE FD FB F7 EF DF BF 7F   ; strobe table: bit c=0, others=1, c=0..7
```

This is the version actually in use, confirmed working on real hardware
(once `WAIT 0` was added at the BASIC level).

## Alternate machine-code version (row index computed on-chip, unused)

Documented for reference — not the version actually typed in, since it
would have meant retyping the already-entered `DATA` lines for no benefit
once the one-line `PRINT` fix above works just as well. Loaded at the same
`4400H` base, but 11 bytes longer (adds a `ROR`-based bit-scan), so the
table and result addresses shift: results at `443BH`/`443CH`
(17467/17468), table at `4433H`.

```
4400: B5 FF          LDI  A,0FFH
4402: FD AE F0 0C    STA  #(0F00CH)     ; DDA = FFH  (PA0-7 all output)
4406: 4A 00          LDI  XL,00H        ; XL = column counter
4408: 58 44          LDI  YH,44H
440A: 5A 33          LDI  YL,33H        ; Y = strobe table (4433H)
440C: 55             LIN  Y             ; L1: A = strobe byte, Y++
440D: FD AE F0 0E    STA  #(0F00EH)     ; OPA = A  (drive this column)
4411: FD BA          ITA                ; A = IN0..7
4413: B7 FF          CPI  A,0FFH
4415: 89 0B          BZR  +0BH          ; -> FOUND if A != FFH (key hit)
4417: 40             INC  XL
4418: 4E 08          CPI  XL,08H
441A: 99 10          BZR  -10H          ; -> L1 if XL != 8 (keep scanning)
441C: 4A FF          LDI  XL,0FFH       ; no key found this pass
441E: 6A FF          LDI  UL,0FFH       ; row-index sentinel (skip bit-scan)
4420: 8E 08          BCH  +08H          ; -> STORE
4422: 6A 00          LDI  UL,00H        ; FOUND: A = row byte, XL = column
L2:
4424: D1             ROR                ; rotate A right through C
4425: 81 03          BCR  +03H          ; -> STORE if C=0 (found the 0 bit)
4427: 62             INC  UL
4428: 9E 06          BCH  -06H          ; -> L2 (keep scanning bits)
STORE:
442A: 04             LDA  XL
442B: AE 44 3B       STA  (443BH)       ; save column index
442E: 24             LDA  UL
442F: AE 44 3C       STA  (443CH)       ; save row index
4432: 9A             RTN

4433: FE FD FB F7 EF DF BF 7F   ; strobe table: bit c=0, others=1, c=0..7
```

Every opcode above is taken from `lh5801_opcode_reference.md` (already
cross-verified against two manuals). `4400H` worked in practice (once
`WAIT 0` was added) for placement, so the earlier "haven't independently
confirmed how much RAM is free above the BASIC program" caveat is
resolved for this program's size.
