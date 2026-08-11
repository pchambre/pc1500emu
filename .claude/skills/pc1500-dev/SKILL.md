---
name: pc1500-dev
description: Use for any work on the pc1500emu repo, or anything else touching LH5801 CPU / Sharp PC-1500 internals (ROM disassembly, hardware quirks, emulator bugs, or writing/testing machine-language programs on real PC-1500 hardware). Covers where the primary sources live, confirmed hardware quirks not obvious from the manual, and the methodology for safely testing ML code on real hardware.
---

# LH5801 / PC-1500 development

Context for the `pc1500emu` project: a from-scratch C++ emulator for the
Sharp PC-1500 (LH5801 CPU), built as the first stage of a longer plan —
emulator → assembler → SDCC backend — so this emulator is the validation
target for all of that later work.

## Primary sources (check these before web research)

- `/home/paul/Documents/PC1500_Technical_reference_manual.pdf` — hardware
  reference: memory map, I/O port controller, keyboard matrix, LCD.
- `/home/paul/Documents/PC1500/PC2AssemblyLanguage.pdf` — PC-2 (the
  US/export name for the PC-1500) assembly manual: interrupt architecture,
  timer divider ratio, system subroutine conventions. The PC-2 and PC-1500
  are the same hardware under different names — don't dismiss a PC-2 source
  as off-topic.
- Both manuals have OCR/scan errors in places (e.g. the keyboard matrix
  diagram) — real-hardware testing has already caught and corrected several
  of these; prefer the corrected facts in `docs/` over re-reading the raw
  PDF where they conflict.

## Repo-local references (source of truth for confirmed facts)

- `docs/pc1500_hardware_reference.md` — memory map (including several
  mirroring/aliasing quirks not stated correctly in the manual), I/O port
  controller register map, keyboard matrix, LCD buffer format. Everything
  here is either sourced from the manual or confirmed on real hardware —
  it says which for each fact.
- `docs/pc1500_keyscan_probe.md` — a worked example of the real-hardware ML
  testing methodology (see below), used to fill in the keyboard matrix.
- `docs/lh5801_opcode_reference.md` — full LH5801 instruction set reference.
- `README.md` — build/run instructions, host keyboard mapping.

Treat these docs as authoritative and keep them in sync with reality: when
you confirm a new hardware fact (especially via real-hardware testing),
update the relevant doc rather than leaving the finding only in
conversation. When code comments reference *why* a decision was made,
that's usually because it traces to a real-hardware finding — don't
"simplify away" behavior that looks redundant or oddly specific
(`src/bus/bus.cpp`/`bus.h` in particular) without first reading the
comment explaining it.

## Confirmed hardware facts worth knowing up front

- **RAM powers up as `0xFF`, not `0x00`.** The ROM's MODE-key handler
  depends on this undocumented bias (`Bus`'s `me0_.fill(0xFF)`).
- **Timer interrupt rate is exactly crystal/128**, i.e. 64 machine cycles
  per tick, independent of exact crystal frequency — this is baked into
  the LH5801 silicon per the PC-2 manual, not an approximation.
- **MI (maskable interrupt) only covers BREAK-key and expansion-port IRQ.**
  There is no dedicated keyboard interrupt; ordinary key detection is
  purely timer-interrupt-driven polling (confirmed via full disassembly of
  `E171H`-`E260H` plus the PC-2 manual's own description of the two MI
  sources).
- **Keyboard rows (IN0-7) are direct CPU inputs**, not routed through the
  LH5811 I/O controller — only the column strobe (PA0-7/OPA) goes through
  it. Reinforces why there's no keyboard-specific interrupt.
- **`7B0EH` bit 0** is the ROM's own "key already dispatched" gate. It only
  self-clears via an ~8-timer-period countdown (legitimate for cursor-key
  rollover), which does *not* match observed hardware responsiveness for
  ordinary keys typed at a natural pace. The emulator patches this
  directly in `Bus` (see the extensive comments in `bus.h`/`bus.cpp`)
  since the real fast-path mechanism was never found — treat that as
  intentional, hardware-observation-driven behavior, not a workaround to
  "clean up."
- **Memory mirroring is chip-select-driven, not a documented feature**:
  `7000H-77FFH` aliases `7600H-77FFH`, and `7C00H-7FFFH` aliases
  `7800H-7BFFH`. `4000H-47FFH` is *not* mirrored anywhere. See
  `docs/pc1500_hardware_reference.md` for the exact bit-masking mechanism.
- **`4000H-40C4H` is live BASIC firmware state**, not free RAM, despite
  being in the "user RAM" chip-select region — see the "reserve area
  gotcha" in `docs/pc1500_hardware_reference.md`.
- **OPB (F00FH bit 5) and IF (F00BH bit 1) are two different views of the
  same RTC TP signal** — OPB is a live, continuous level; IF is a
  one-shot edge latch — and WAIT/BEEP's shared poll loop (`LE89C`-`LE8BC`)
  reads OPB first, falling back to IF only when OPB reads low, treating
  "IF set while OPB was low" as BREAK. Confirmed live: the ROM's own poll
  loop has a real, measured ~61us gap between the two actual register
  reads (not just a few CPU cycles — a `vmj` plus a helper routine sits in
  between), so any emulation of this needs to keep the two answers
  consistent across that gap or WAIT spuriously aborts. See `Upd1990ac`'s
  class comment in `src/bus/bus.h` for the fix (an OPB-anchored debounce,
  not a shared time grid) and why the naive fix makes it worse.

## Debugging timing-sensitive emulator code

- **A deterministic clock plus a fixed-cycle poll loop can turn a
  "rare" hardware race into a "fails every single time" bug**, and vice
  versa: fixing it by snapping timestamps to a shared fixed grid can make
  it *worse*, not better, since "does this iteration straddle the grid
  line" becomes itself a fixed yes/no that repeats identically on every
  tick. If two related signals derived from the same lazily-resynced
  clock need to agree when read close together, anchor debouncing to
  "time since whichever signal is authoritative last actually resampled,"
  not to a fixed time grid — see the RTC fact above for a worked example.
- **Don't infer the timing gap between two points from a trace hook's own
  "cycles since I last printed" delta** — that measures the cost of
  executing the instruction that landed at the print site, not the true
  gap to whatever happens next. Cost real time in this project once
  (assumed ~6-8 cycles between two register reads from instruction
  counting; the real gap, measured by instrumenting the actual internal
  timestamp directly, was ~79 cycles / 61us). When a gap matters, print
  the actual internal state/timestamp at each point, not a step counter.

## Writing and testing ML programs on real PC-1500 hardware

This project's methodology for confirming hardware behavior beyond what
the manuals document: write a small ML routine, `POKE` it in via a short
BASIC loader, and have the user run it and report back `PEEK` results.
Real mistakes made (and fixed) doing this, worth avoiding:

1. **Strobe columns one at a time.** Strobing all 8 columns simultaneously
   (`OPA=0x00`) works fine in the emulator's software model but causes
   real electrical ghosting on actual hardware. Always use the proven
   one-column-at-a-time strobe table (`FE,FD,FB,F7,EF,DF,BF,7F`), as in
   `docs/pc1500_keyscan_probe.md`.
2. **Subroutines must only be reached via `SJP`, never by fall-through.**
   Falling into a subroutine on first execution means its `RTN` pops a
   return address that was never pushed, unwinding the whole program
   instantly. Put subroutines after the main sequential logic (which
   should end in its own `RTN` or halt), reachable only by explicit calls.
3. **Double-check branch direction.** `BZS`/`BZR`/`BCS`/`BCR` encode
   direction in the opcode itself (`0x8x` = forward `+i`, `0x9x` = backward
   `-i`) — verify the target is actually on the side the chosen opcode
   assumes, don't just eyeball the displacement.
4. **Triple-check decimal/hex address conversions** before giving a user a
   `PEEK`/`POKE` address — a `0x4780` vs `0x4480` mixup has happened here
   before and silently produces plausible-looking wrong answers rather
   than an obvious error.
5. **A `CALL`ed ML routine monopolizes the CPU.** If what you're trying to
   measure depends on the ROM's own mainline/idle loop continuing to run
   in the background (e.g. `7B0E` bit 0's self-clear countdown), a called
   subroutine can't observe it — the ROM's background processing is
   paused for as long as your routine holds the CPU. Recognize this class
   of measurement as structurally impossible via ML and fall back to
   black-box comparison (matching observed behavior in the emulator
   directly) instead.
6. **Always validate before sending to hardware**: trace the program
   through the emulator first (it's the same CPU core), and give the user
   a checksum snippet (`PRINT` a sum over the POKE'd range) to run before
   relying on the result, so stale/partially-typed `DATA` lines are
   caught rather than misread as a behavioral finding.
7. **`WAIT 0`** disables BASIC's default post-`PRINT` pause — needed for
   any test loop that prints and re-runs without waiting for a keypress
   each time.

## Layout

- `src/cpu/` — LH5801 CPU core
- `src/bus/` — memory map, bus I/O dispatch, keyboard-timing compensation
- `src/keyboard/` — keyboard matrix emulation
- `src/lcd/` — dot-matrix LCD controller emulation
- `src/host/` — SDL2 windowing/input/main loop
- `tests/` — unit tests (`ctest` via the `build/` CMake tree)

Build/test: `cmake -B build && cmake --build build`, then
`cd build && ctest --output-on-failure`.
