# Changelog

All notable changes to this project are documented here. Versions follow
`CMakeLists.txt`'s `project(pc1500emu VERSION ...)`, bumped on every push
per this project's own convention (not just milestones).

## [0.6.2] - 2026-08-11

### Fixed
- `WAIT n` (and BEEP's gap-timer) could run many times slower than its
  true `n/64` seconds -- confirmed live (WAIT 64 took ~10 real seconds
  instead of 1). A rendered frame's entire CPU cycle budget executes in
  well under a millisecond of real host time, so reading the real clock
  directly on every register access meant WAIT's poll loop saw the same
  frozen timestamp for an entire frame's burst, missing most RTC ticks.
  Fixed by advancing the RTC's clock smoothly per instruction (scaled by
  its own cycle cost), re-anchored to the real clock once per frame.
- After any WAIT/BEEP had run once, the screen could clear spuriously at
  the idle READY prompt from then on, for the rest of the session -- the
  real-time clock's TP output never stopped oscillating once configured
  (nothing ever turned it back off), so its ticks kept leaking into a bit
  shared with BREAK detection, which the idle loop misread as BREAK
  presses. Fixed by disabling TP once WAIT/BEEP's own poll loop is done
  with it.

## [0.6.1] - 2026-08-11

### Added
- `pc1500disasm` now cross-references BASIC keyword-table entries back
  onto their implementation address (e.g. `LE86AH:  ; WAIT keyword`),
  instead of requiring a manual keyword-table lookup to identify a
  keyword's own entry point in a listing.

### Fixed
- `WAIT n` (and BEEP's identical gap-timer) could exit far too early with
  a spurious "BREAK IN <line>" error, or otherwise complete noticeably
  faster than its requested duration, due to a hardware-timing race
  between two status registers (`OPB`/`IF`) both derived from the
  real-time clock's tick signal. `WAIT n` now counts down its full
  duration at the correct, linear 64Hz rate.
- "Load BASIC Text": a long line whose greedy per-pass packing happened to
  land mid-parenthesized-expression (e.g. right after the "(" in `A$(X,Y)`)
  was rejected outright by the ROM's tokenizer and failed the whole load,
  instead of being recognized as a rejection and retried with a shorter
  pass. See `docs/pc1500_hardware_reference.md`'s "BASIC line editor"
  section for the confirmed ROM behavior this is built on.
- "Load BASIC Text": a validation error message could run off the edge of
  the dialog instead of wrapping.

### Removed
- "Load BASIC Text": the redundant "Paste from Clipboard" button (the text
  box already supports the platform's normal paste shortcut).

## [0.6.0] - 2026-08-09

### Added
- "Load BASIC Text" now supports source lines longer than the ROM's
  79-character raw-input limit, using the same multi-pass LIST-and-append
  technique real PC-1500 owners used: type up to the ROM's own limit,
  Enter, then resume editing the same line to append more, repeating as
  needed. See `docs/pc1500_hardware_reference.md`'s "BASIC line editor"
  section for the confirmed mechanics this is built on.
- "Load BASIC Text" shows a "Loading..." indicator (and a wait cursor)
  while a long listing is being typed in, instead of appearing to hang.
- Enter/Escape keyboard shortcuts for dialogs: Enter triggers the primary
  action (Load, Save, etc.) on every dialog except "Load BASIC Text";
  Escape triggers Cancel on all dialogs. Escape also closes an open menu.
- Keyboard shortcuts for actions that previously required the mouse:
  Ctrl+Alt+O/S for Load/Save BASIC Text, Ctrl+Alt+A for Automation Mode,
  Ctrl+Alt+P for the status panel.
- Extension RAM size (both the 4800H and 0000H windows) is now remembered
  across restarts, via the same conf file as the other Settings-menu
  options.

### Fixed
- macOS: indicator font path no longer hardcodes a Linux-only location;
  tries a candidate list of real macOS system fonts instead.
- macOS: the app window now requests foreground activation on launch,
  instead of opening behind other windows.
- Checking "Auto-Save State on Exit" with no state file ever explicitly
  loaded/saved now defaults to `pc1500emu.state` in the current directory,
  instead of silently doing nothing at exit.
- "Load BASIC Text": the button that reads the Filename field's path into
  the text box is now labeled "Read File" (previously "Load File into
  Text") and stays disabled until a filename is entered. Clicking Load
  after typing or pasting a filename, without pressing Read File first,
  now reads that file automatically.

## [0.5.1] - 2026-08-09

### Fixed
- macOS build/run fixes (font path, window activation, auto-save-on-exit).

## [0.5.0] and earlier

Versions before 0.5.1 were not individually tracked in this file. See
`git log` for the full history -- notable earlier work includes the
LH5801 CPU/bus/keyboard/LCD emulation core, BASIC program load/save
(binary and text), interactive DAP debugger, LH5801 disassembler, and
save-state support.
