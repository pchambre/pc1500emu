# PC-1500 keyboard matrix scan probe

A tiny BASIC + machine-language tool to run on a real PC-1500 to empirically
fill in the rest of the key matrix table in `pc1500_hardware_reference.md`,
rather than reading it off the (illegible-in-places) manual scan.

## Type this into the PC-1500 in BASIC

```basic
5 FOR I=0 TO 47:READ D:POKE 16384+I,D:NEXT I
6 DATA 181,255,253,174,240,12,74,0,88,64,90,40,85,253,174,240,14,253,186,183
7 DATA 255,137,9,64,78,8,153,16,181,255,74,255,174,64,129,4,174,64,128,154
8 DATA 254,253,251,247,239,223,191,127
10 CALL 16384
20 PRINT PEEK(16512);PEEK(16513)
30 GOTO 10
```

(An earlier version of this put the strobe table at a separate address
[`4090H`], which needed two POKE loops to land each block correctly. The
table now sits immediately after the code at `4028H`, so it's one
contiguous 48-byte block and one loop — only the `LDI YL` operand byte
changed, from `90H` to `28H`, i.e. DATA item 12 went from `144` to `40`.)

Run it (`RUN`), then press keys one at a time. Each press prints two numbers:
**column index (0-7, matching PA0-PA7)** and **row byte** (the raw `IN0-IN7`
value with the pressed row's bit low — e.g. `254` = bit0/IN0 low, `253` =
bit1/IN1 low, `251` = bit2/IN2, `247` = bit3/IN3, `239` = bit4/IN4,
`223` = bit5/IN5, `191` = bit6/IN6, `127` = bit7/IN7). If no key is pressed
during a pass it prints `255 255`.

To stop: `BREAK`/`ON` should interrupt it (standard PC-1500 BASIC behavior),
since `GOTO 10` loops forever otherwise.

**If it never shows anything but `255 255` while a key is held**, the strobe
polarity assumption is backwards for this hardware. Fix: change the last 8
values in line 8's `DATA` (currently `254,253,251,247,239,223,191,127`) to
`1,2,4,8,16,32,64,128` (active-high strobe) and swap `PEEK(16513)`
comparisons/expectations accordingly — but try it as-is first; active-low
with pulled-up rows is the standard convention for this class of chip.

Runs in a tight loop, so a held key prints repeatedly — that's expected,
just move to the next key once you've noted one reading.

## What it loads (annotated, for reference/verification)

Loaded at `4000H` (16384 decimal) — safely below the BASIC program area,
which starts at `40C5H` per the manual's `NEW` command documentation.
Results land at `4080H`/`4081H` (16512/16513); the strobe table immediately
follows the code at `4028H`.

```
4000: B5 FF          LDI  A,0FFH
4002: FD AE F0 0C    STA  #(0F00CH)     ; DDA = FFH  (PA0-7 all output)
4006: 4A 00          LDI  XL,00H        ; XL = column counter
4008: 58 40          LDI  YH,40H
400A: 5A 28          LDI  YL,28H        ; Y = strobe table (4028H)
400C: 55             LIN  Y             ; L1: A = strobe byte, Y++
400D: FD AE F0 0E    STA  #(0F00EH)     ; OPA = A  (drive this column)
4011: FD BA          ITA                ; A = IN0..7
4013: B7 FF          CPI  A,0FFH
4015: 89 09          BZR  +9            ; -> FOUND if A != FFH (key hit)
4017: 40             INC  XL
4018: 4E 08          CPI  XL,08H
401A: 99 10          BZR  -10H          ; -> L1 if XL != 8 (keep scanning)
401C: B5 FF          LDI  A,0FFH        ; no key found this pass
401E: 4A FF          LDI  XL,0FFH
4020: AE 40 81       STA  (4081H)       ; FOUND: save row byte
4023: 04             LDA  XL
4024: AE 40 80       STA  (4080H)       ; save column index
4027: 9A             RTN

4028: FE FD FB F7 EF DF BF 7F   ; strobe table: bit c=0, others=1, c=0..7
```

Every opcode above is taken from `lh5801_opcode_reference.md` (already
cross-verified against two manuals), so this should hand-assemble
correctly as written — but it's untested on real hardware. If `CALL 16384`
does something unexpected (hangs, garbage output), that's the first place
to check.
