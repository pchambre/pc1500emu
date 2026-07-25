# pc1500emu

A from-scratch emulator for the Sharp PC-1500 pocket computer, built around
the Sharp/Sanyo LH5801 CPU.

Scope for the initial version:
- LH5801 CPU core (full instruction set)
- PC-1500 memory map and bus I/O
- Keyboard matrix input
- Dot-matrix LCD output (156x7)

Out of scope for now: cassette interface.

This project exists to support later work adding LH5801/PC-1500 target
support to [SDCC](https://sdcc.sourceforge.net/) — this emulator is used to
validate hand-written and assembler/compiler-generated code before any
compiler backend work begins.

## Building

Requires SDL2 development packages (`libsdl2-dev` on Debian/Ubuntu).

```sh
cmake -B build
cmake --build build
```

## Running

```sh
./build/src/host/pc1500emu [rom.bin]
```

No PC-1500 ROM dump is bundled (it's copyrighted Sharp firmware) — without
one, the emulator runs with a zero-filled ROM area, which spins harmlessly
rather than doing anything useful. Pass a real dump's path to load it at
`C000H`.

Keyboard mapping (host key -> PC-1500 key) is defined in `src/host/main.cpp`
(`kKeyMap[]`). Digits (including the numpad), letters, arrows, F1-F6,
Enter, and Space map directly; Backspace duplicates the left arrowhead.
PC-1500 keys without an obvious host equivalent live on F7-F12 for
muscle-memory reasons (chosen over keys like Delete/Insert, which don't
map intuitively to a calculator's special keys):
- F7 = Cl, F8 = Mode, F9 = Def, F10 = Sml, F11 = Rcl
- Shift+F10 = the up/down rocker key
- **F12 = On** (it's wired directly to the CPU, not part of the keyboard
  matrix, so it's handled separately from every other key)
- Shift+F12 = Off
- **Ctrl+F12 is a host-only RESET key**, mimicking the real machine's
  recessed ALL RESET pinhole switch (also not part of the matrix) — it
  re-runs `CPU::reset()` without otherwise touching RAM.

**Shift**: both host Shift keys behave identically and are read only as
modifiers (never mapped directly to a PC-1500 key) -- real PC-1500 Shift
is a tap-to-toggle key, not a hold, and holding it for a modern
keyboard's whole keypress duration confuses the ROM's key-scan. **Tab**
sends a direct, standalone PC-1500 Shift keypress for cases not covered
by the punctuation passthrough below.

**Punctuation passthrough** (`kSymbolMap[]`): typing `!"#$%&@^<>:?;,`
directly on the host reproduces the exact PC-1500 Shift+key combo that
types each one on real hardware (confirmed by Paul), queued as a
fire-and-forget sequence (Shift tap, then the target key) that runs to
completion regardless of how long the host key is held. There's a small
(~0.25s) delay before the character appears. Some of these host keycodes
are *shared* with a plain meaning (QWERTY's `1` is plain "1" alone, or
"!" with host Shift also held) -- those correctly gate on host Shift and
fall through to a plain keypress when it isn't held. Others have no such
plain meaning at all (Insert/Delete, or `"` on a layout like AZERTY that
types it without touching Shift) -- those always send the PC-1500 Shift
tap regardless of host Shift, encoded by giving both of a mapping's
targets the same key. Insert and Delete use this mechanism for
Shift+Right and Shift+Left.

Interrupt delivery (MI/NMI/timer) is implemented, so `HLT` resumes normally
when one fires.

## Layout

- `src/cpu/` — LH5801 CPU core
- `src/bus/` — memory map, bus I/O dispatch
- `src/keyboard/` — keyboard matrix emulation
- `src/lcd/` — dot-matrix LCD controller emulation
- `src/host/` — host-side glue (windowing, input, main loop)
- `tests/` — unit tests
- `docs/` — technical reference notes (ISA, hardware) backing the implementation
