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

Requires SDL2 and SDL2_ttf development packages (`libsdl2-dev`,
`libsdl2-ttf-dev` on Debian/Ubuntu). [Dear ImGui](https://github.com/ocornut/imgui)
(the menu bar UI) is vendored directly in `third_party/imgui/` — nothing
extra to install for that.

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

## Menu bar

An in-window menu bar (Dear ImGui, rendered as an SDL2 overlay — not a
native OS menu) sits above the display:

- **File > Load/Save BASIC...** — a BASIC program (filename only). Finds
  the program's bounds itself by walking the line structure from `40C5H`
  to the `FFH` end marker (see `findBasicProgramEnd()` in `main.cpp` and
  `docs/pc1500_hardware_reference.md`'s reserve-area note) — matches real
  `CLOAD`/`CSAVE` (without `M`) semantics, no address/length needed.
- **File > Load/Save Binary...** — a raw `[address, address+length)` ME0
  byte range plus a filename, matching real `CLOAD M`/`CSAVE M` semantics.
  Load has an optional "call after load" checkbox (sets the CPU's `P`
  register to the loaded address, like a hand-triggered `CALL`). Useful
  for loading `sdas`/SDCC-assembled output directly for testing.
- **Settings > Extension RAM (4800H)** — None (default) / 4K / 8K / 10K of
  emulated module RAM at `4800H`. 4K/8K were the real 1982-era hardware
  options; 10K is the window's full physical span (`4800H`-`6FFFH`) --
  not a real period-correct module, but easy to emulate.
- **Settings > Extension RAM (0000H)** — None (default) / 16K of emulated
  module RAM at `0000H`-`3FFFH`. Not a real 1982-era option at all (nothing
  plugged in there back then), but physically possible now.

While a menu dialog has keyboard focus, keystrokes go to the dialog's text
fields, not the emulated PC-1500 keyboard.

## Scriptable command interface

A running instance also reads commands from a named pipe at
`/tmp/pc1500emu.cmd`, one per line, letting an external process (or a
script) drive it directly instead of relaying everything through the GUI
by hand. Query commands write their result to `/tmp/pc1500emu.response`.

```sh
echo 'type 10 PRINT "HELLO"' > /tmp/pc1500emu.cmd
echo 'key enter' > /tmp/pc1500emu.cmd
echo 'display' > /tmp/pc1500emu.cmd; cat /tmp/pc1500emu.response   # ASCII-art dump of the LCD
echo 'status' > /tmp/pc1500emu.cmd; cat /tmp/pc1500emu.response    # registers, flags, indicator bits
echo 'peek 4000' > /tmp/pc1500emu.cmd; cat /tmp/pc1500emu.response
echo 'poke 4000 ff' > /tmp/pc1500emu.cmd
echo 'dump 4000 40ff' > /tmp/pc1500emu.cmd; cat /tmp/pc1500emu.response
```

Commands:
- `type <text>` — queues each character as a keypress (letters, digits,
  space, and a handful of symbols needing a PC-1500 Shift-tap: `" : < >`).
  Unmapped characters are skipped with a stderr warning — use `key` instead.
- `key <name>` — a named key with no natural printable form: `enter`, `cl`,
  `mode`, `def`, `sml`, `rcl`, `shift`, `off`, `up`/`down`/`left`/`right`,
  `f1`-`f6`, `space`.
- `peek <addr>` / `poke <addr> <val>` — addresses and values in hex.
- `dump <start> <end>` — hex bytes, 16 per line, address-prefixed.
- `status` — CPU registers/flags and the fixed-segment indicator bits.
- `display` — the 156x7 dot matrix as ASCII art (`#`/`.`).
- `savebasic <path>` / `loadbasic <path>` / `savebinary <addr> <len> <path>`
  / `loadbinary <addr> <path>` — the same functions the File menu's dialogs
  call, invokable directly without going through the GUI.

`type`/`key` commands queue onto the same mechanism real typing uses, so
scripted and live keyboard input interleave safely rather than racing.

## Layout

- `src/cpu/` — LH5801 CPU core
- `src/bus/` — memory map, bus I/O dispatch
- `src/keyboard/` — keyboard matrix emulation
- `src/lcd/` — dot-matrix LCD controller emulation
- `src/host/` — host-side glue (windowing, input, main loop, menu bar)
- `third_party/imgui/` — vendored Dear ImGui (menu bar UI), MIT licensed
- `tests/` — unit tests
- `docs/` — technical reference notes (ISA, hardware) backing the implementation

## License

Licensed under the Apache License, Version 2.0 -- see `LICENSE`.
