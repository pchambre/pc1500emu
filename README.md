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
(`kKeyMap[]`). Digits, letters, arrows, F1-F6, Enter, and Space map
directly; a few PC-1500 keys without an obvious host equivalent use nearby
substitutes (Tab->Mode, Backspace->Def, End->Off, Delete->Cl, Insert->Rcl,
PageUp/PageDown->the up/down rocker key). **F12 is the ON key** (it's wired
directly to the CPU, not part of the keyboard matrix, so it's handled
separately from every other key). **F11 is a host-only RESET key**, mimicking
the real machine's recessed ALL RESET pinhole switch (also not part of the
matrix) — it re-runs `CPU::reset()` without otherwise touching RAM.

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
