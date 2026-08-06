# pc1500emu

A from-scratch emulator for the Sharp PC-1500 pocket computer, built around
the Sharp/Sanyo LH5801 CPU.

Author: Paul Chambre using Claude Code and CLion

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

Requires SDL2 and SDL2_ttf development packages.

**Linux**: `libsdl2-dev`, `libsdl2-ttf-dev` on Debian/Ubuntu.
[Dear ImGui](https://github.com/ocornut/imgui) (the menu bar UI) is vendored
directly in `third_party/imgui/` — nothing extra to install for that.

```sh
cmake -B build
cmake --build build
```

**Windows**: install SDL2 and SDL2-ttf via [vcpkg](https://github.com/microsoft/vcpkg)
(`vcpkg install sdl2 sdl2-ttf`), then point CMake at its toolchain file:

```sh
cmake -B build -DCMAKE_TOOLCHAIN_FILE=<path-to-vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config RelWithDebInfo
```

The indicator row (see the Keyboard mapping section below) needs a
CJK-capable font for its katakana glyphs; Windows ships one
(`C:\Windows\Fonts\msgothic.ttc`) by default since Windows 8, so nothing
extra to install there either.

## Running

```sh
./build/src/host/pc1500emu [rom.bin]          # Linux
build\src\host\RelWithDebInfo\pc1500emu.exe [rom.bin]   # Windows
```

No PC-1500 ROM dump is bundled (it's copyrighted Sharp firmware) — without
one, the emulator runs with a zero-filled ROM area, which spins harmlessly
rather than doing anything useful. Pass a real dump's path to load it at
`C000H`.

Keyboard mapping (host key -> PC-1500 key) is defined in `src/host/main.cpp`
(`kKeyMap[]`). Letters, numpad digits, arrows, F1-F6, Enter, and Space map
directly; Backspace duplicates the left arrowhead. PC-1500 keys without an
obvious host equivalent live on F7-F12 for muscle-memory reasons (chosen
over keys like Delete/Insert, which don't map intuitively to a
calculator's special keys):
- F7 = Cl, F8 = Mode, F9 = Def, F10 = Sml, F11 = Rcl
- Shift+F10 = the up/down rocker key
- **F12 = On** (it's wired directly to the CPU, not part of the keyboard
  matrix, so it's handled separately from every other key). While a
  program is running, the same key doubles as **BREAK**: it latches IF
  register bit `0x02` (via PB7, the LH5811 pin ON is wired to, configured
  as an interrupt input -- confirmed shared with the RTC's TP edge, see
  `IoPortController::setOnKeyLine()`) and requests the CPU's maskable
  interrupt (vector `FFF8H`); both together are what gets a running BASIC
  program to actually stop and show `BREAK IN <line>`, in addition to
  ON's own wake-from-off role.
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

**Digits and punctuation** (`charToTapActions()`, driven by `SDL_TEXTINPUT`
rather than keycodes): typing a digit or one of `!"#$%&@^<>:?;,.+-/=()*`
on the host reproduces the exact PC-1500 keypress (or Shift+key combo)
that types each one on real hardware (confirmed by Paul), queued as a
fire-and-forget sequence that runs to completion regardless of how long
the host key is held. There's a small (~0.25s) delay before the character
appears. `.`, `-`, `/`, and `=` used to have their own direct keycode
entries in `kKeyMap[]` as well, which (same root cause as the digit-row
bug below) ignored host Shift entirely -- confirmed broken on AZERTY,
where the key that types unshifted `=`/shifted `+` always produced `=`
regardless of Shift. Removed in favor of the same `SDL_TEXTINPUT` path
everything else here uses.

Because this is driven by `SDL_TEXTINPUT` (the OS's own composed text)
instead of raw keycodes, it's host-keyboard-layout-aware automatically --
e.g. on a French AZERTY host, the unshifted top-row key reproduces `&`
(what's printed on the keycap) and Shift+that key reproduces `1`, matching
the host layout rather than assuming QWERTY. (An earlier keycode-based
design didn't have this property, and was additionally broken on Windows
specifically: SDL's Windows backend hardcodes the top row's *keycode* to
always report digits regardless of the real OS layout, so AZERTY typed
"1" instead of "&" no matter what -- SDL_TEXTINPUT isn't affected by that
quirk.) Insert and Delete are the two exceptions -- not printable
characters, so they can't come from `SDL_TEXTINPUT` -- handled directly by
keycode instead, always sending Shift+Right / Shift+Left regardless of
host Shift state.

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
- **File > Load/Save BASIC Text...** — a BASIC program as a plain-text
  listing (e.g. a magazine transcription), rather than the tokenized
  binary format above. *Load* drives the ROM's own line editor via
  simulated keystrokes (so it's the real ROM tokenizing, not a
  reimplementation) — type or paste a listing into the text box, or load
  it from a file first, then click Load; any line the ROM doesn't accept
  is reported by number/content rather than silently dropped. *Save*
  detokenizes the current program into the text box using this project's
  own keyword table (`src/basic/`); its spacing is our own readable
  convention, not necessarily byte-for-byte identical to a real device's
  `LIST` output. Either box can be copied to/from the clipboard.
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
  module RAM at `0000H`-`3FFFH`.
- **Settings > Automation Mode** — when checked, real host keyboard input
  (typing, arrow keys, F-keys, etc.) is ignored entirely; only the
  scriptable command interface below can drive the emulator. An orange
  `[AUTOMATION MODE]` notice appears in the menu bar whenever it's on, so
  it's never silently active. Meant for scripted/automated test sessions
  where an accidental real keypress landing on the window would otherwise
  corrupt whatever state is being driven over the FIFO. Toggle from the
  FIFO itself with `automation on` / `automation off`.

When "adding" RAM, it is necessary to reset the emulator (Ctrl+F12), then
press CL and execute NEW0 to update the emulator to be aware of the
additional memory.

While a menu dialog has keyboard focus, keystrokes go to the dialog's text
fields, not the emulated PC-1500 keyboard.

## Scriptable command interface

A running instance also reads commands from a named pipe, one per line,
letting an external process (or a script) drive it directly instead of
relaying everything through the GUI by hand. Query commands write their
result to a response file for the caller to read back afterward.

**Linux**: a POSIX FIFO at `/tmp/pc1500emu.cmd`, writable with plain shell
redirection; the response file is `/tmp/pc1500emu.response`.

```sh
echo 'type 10 PRINT "HELLO"' > /tmp/pc1500emu.cmd
echo 'key enter' > /tmp/pc1500emu.cmd
echo 'display' > /tmp/pc1500emu.cmd; cat /tmp/pc1500emu.response   # ASCII-art dump of the LCD
echo 'status' > /tmp/pc1500emu.cmd; cat /tmp/pc1500emu.response    # registers, flags, indicator bits
echo 'peek 4000' > /tmp/pc1500emu.cmd; cat /tmp/pc1500emu.response
echo 'poke 4000 ff' > /tmp/pc1500emu.cmd
echo 'dump 4000 40ff' > /tmp/pc1500emu.cmd; cat /tmp/pc1500emu.response
```

**Windows**: there's no filesystem-visible equivalent of a FIFO, so this is
a named pipe (`\\.\pipe\pc1500emu.cmd`) instead, which plain shell
redirection can't write to — use `tools/send-command.ps1`. The response
file is `%TEMP%\pc1500emu.response`.

```powershell
powershell -File tools\send-command.ps1 'type 10 PRINT "HELLO"'
powershell -File tools\send-command.ps1 'key enter'
powershell -File tools\send-command.ps1 'status'; Get-Content $env:TEMP\pc1500emu.response
```

Commands:
- `type <text>` — queues each character as a keypress (letters, digits,
  space, and a handful of symbols needing a PC-1500 Shift-tap: `" : < >`).
  Unmapped characters are skipped with a stderr warning — use `key` instead.
- `key <name>` — a named key with no natural printable form: `enter`, `cl`,
  `mode`, `def`, `sml`, `rcl`, `shift`, `off`, `up`/`down`/`left`/`right`,
  `f1`-`f6`, `space`. Prefix with `shift+` (e.g. `key shift+mode`) to send
  a genuine PC-1500 Shift-tap before it, same mechanism as typing a
  host-Shift symbol.
- `peek <addr>` / `poke <addr> <val>` — addresses and values in hex.
- `dump <start> <end>` — hex bytes, 16 per line, address-prefixed.
- `status` — CPU registers/flags and the fixed-segment indicator bits.
- `display` — the 156x7 dot matrix as ASCII art (`#`/`.`).
- `displaytext` — the ROM's own LCD text buffer (`7BB0H`-`7BFFH`, per the
  PC-2 Assembly Language manual's "DISPLAY THROUGH A BUFFER" section) read
  directly as ASCII up to its `0DH` terminator -- e.g. `ERROR 1` or
  `NEW0? :CHECK`. Prefer this over `display` for anything that's plain
  text: it's exact, where eyeballing the dot-matrix ASCII art is not.
- `savebasic <path>` / `loadbasic <path>` / `savebinary <addr> <len> <path>`
  / `loadbinary <addr> <path>` — the same functions the File menu's dialogs
  call, invokable directly without going through the GUI.
- `savebasictext <path>` / `loadbasictext <path>` — the text-listing
  equivalents. `loadbasictext` drives the ROM's line editor internally (see
  File menu section above) by stepping CPU/bus cycles directly rather than
  the real-time `type`/`key` queue, so it completes in well under a second
  regardless of program size; a returned `ERROR: N line(s) rejected...`
  lists which lines the ROM didn't accept. Any line longer than 79
  characters is rejected up front with a clear error instead of being
  typed — the ROM's own line editor has a hard 79-character input limit
  and silently drops everything past it with no error shown, so without
  this check a too-long line would appear to load successfully while
  actually being truncated.
- `break [cycles]` — scriptable equivalent of pressing the physical ON key
  (F12) while a program is running: sets the ON-key line (which latches IF
  register bit `0x02`, confirmed shared with the RTC's TP edge -- see
  `IoPortController::setOnKeyLine()`) and requests the CPU's maskable
  interrupt (vector `FFF8H`) -- both are needed together to actually break
  a running program; either alone does nothing. If `cycles` is given,
  traces that many cycles synchronously in the same call (like
  `calltrace`) so the dispatch can actually be observed -- otherwise the
  live frame loop's own background stepping will most likely have already
  handled and returned from the interrupt by the time a separate `trace`
  call gets to look.
- `call <addr>` — sets the CPU's `P` register directly (hex address).
- `presskey <name>` / `releasekey <name>` — direct, synchronous key
  press/release (same names as `key`), bypassing the queue `type`/`key`
  use. Needed before `run`/`trace` for deterministic testing of a specific
  key's effect, since the queue only drains via the normal frame loop.
- `run <cycles>` — steps the CPU exactly `<cycles>` cycles synchronously
  (decimal), independent of the normal ~60fps frame loop. Useful for
  driving execution deterministically in a test script.
- `trace <cycles>` — like `run`, but writes one line per instruction
  executed (`PC opcode-byte A=.. X=.... Y=....`, hex) to the response file.
  For finding exactly what the CPU does over a short, specific window
  (e.g. right after a keypress) instead of only diffing memory before/after.

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
