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
- **`Upd1990ac`'s `tpConfigured_` is a one-way latch by default — nothing
  in the datasheet's own command semantics ever turns TP back off, and
  WAIT/BEEP is the *only* caller that ever configures it.** Confirmed:
  once any WAIT/BEEP call has run, TP oscillates in the background for
  the rest of the process's life unless something explicitly disables it,
  and *any* later, unrelated read of IF (the idle READY-prompt loop's own
  BREAK check, `KEYSCAN_WAIT`/`LE269`; the generic statement-boundary
  break-check `LC42A`) picks up genuine RTC ticks and misreads them as
  BREAK, since both share the same bit with no way to tell the source
  apart — symptom confirmed live: the screen visibly cleared periodically
  at the idle prompt long after any WAIT had finished. Fixed by resetting
  `tpConfigured_` (and the TP level/edge state) whenever a Group 0
  (register-mode, C2=0) RTC command is latched — confirmed via
  disassembly that both WAIT's poll-loop abort path (`LE8B4`) *and* its
  normal successful-completion path (`LE8C3`-`LE8C5`) issue this exact
  command as their own cleanup, so this reliably fires either way.
- **A rendered frame's entire CPU cycle budget executes in well under a
  millisecond of real time on any modern host** (a simple interpreted
  8-bit core easily retires tens of millions of instructions/sec) — so
  code that reads the real host clock directly on every register access,
  once per rendered frame, sees essentially the *same* timestamp for the
  whole frame's burst. `main.cpp`'s frame loop bursts through
  `kCyclesPerFrame` cycles near-instantly, then sleeps out the rest of
  the ~16.7ms frame via a trailing `SDL_Delay` — so anything needing
  finer-than-one-frame real-time resolution (WAIT/BEEP's poll loop,
  needing ~7.8ms resolution at 64Hz) must not read the host clock
  directly. Fixed by promoting the RTC's test-only manual/frozen clock
  (`Upd1990ac::useManualClock()`/`advanceManualClock()`, formerly
  `testFreezeClock()`/`testAdvanceSeconds()`) to production use too: the
  main loop re-anchors it to the real clock once per frame, then advances
  it after every CPU instruction by that instruction's own cycle cost —
  smooth, monotonic progress within a frame, still re-synced to real time
  every frame so it can't drift over a long session. Confirmed live via
  direct `U`-register-decrement-rate measurement (poll `status` at known
  `Stopwatch` intervals — see the live-testing section above), not just
  by re-running the headless suite: the headless tests already modeled
  smooth per-instruction time advancement and were passing throughout,
  so they could not have caught this class of bug on their own.

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

## Driving the live emulator for manual verification

Headless `ctest` coverage is not a substitute for actually running the
live `pc1500emu.exe` and checking real behavior — see the "Debugging
timing-sensitive emulator code" section above for a case where the
headless suite passed cleanly while the live build was still measurably
broken (the headless RTC clock advances smoothly per instruction; the
live build's actual CPU stepping is bursty, once per rendered frame, and
only production code exercises that difference). Drive the live build via
the FIFO/pipe command interface (`tools/send-command.ps1` on Windows,
plain FIFO redirection on Linux — see `README.md`'s "Scriptable command
interface" section for the full command list) rather than trying to
automate real keypresses. Mistakes made (and fixed) doing this:

1. **Always send `automation on` first.** Without it, a stray real
   keypress landing on the window (or just normal window-focus churn)
   can interleave with and corrupt a scripted command sequence.
2. **A fresh boot (or after a `reset` command) needs `CL` then `NEW0`
   (typed, then Enter) before anything else** — typing/loading a program
   or expecting clean state without this first can get silently rejected
   or behave oddly. `reset` in particular does not reliably leave the
   machine in a state a second `loadbasictext` can load cleanly into
   (confirmed: reloading over an existing line 1 without a fresh `CL`
   +`NEW0` first got rejected) — when in doubt, kill and relaunch the
   process instead of trusting `reset` alone.
3. **Changing extension RAM size also needs `reset`, `CL`, and `NEW0`**
   to actually take effect.
4. **`RUN` only works from RUN mode, not PRO mode** (the mode the
   machine is in right after loading/typing a program). Press `key mode`
   to toggle into RUN mode before typing `RUN` and pressing Enter —
   typing `RUN` while still in PRO mode does nothing useful (BASIC either
   ignores it or treats it as program-text input, not a command).
5. **To measure real timing precisely, poll `status` and read a specific
   register's value at known wall-clock intervals (via a `Stopwatch`),
   rather than waiting for a single terminal condition to appear.**
   Watching a register count down at 3-4 sampled points and computing the
   rate directly is far more reliable, and much faster to get an answer
   from, than polling `displaytext` in a loop hoping to catch the exact
   moment something completes.

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
