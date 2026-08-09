# Changelog

All notable changes to this project are documented here. Versions follow
`CMakeLists.txt`'s `project(pc1500emu VERSION ...)`, bumped on every push
per this project's own convention (not just milestones).

## [0.6.0] - Unreleased

### Added
- "Load BASIC Text" now supports source lines longer than the ROM's
  79-character raw-input limit, using the same multi-pass LIST-and-append
  technique real PC-1500 owners used: type up to the ROM's own limit,
  Enter, then resume editing the same line to append more, repeating as
  needed. See `docs/pc1500_hardware_reference.md`'s "BASIC line editor"
  section for the confirmed mechanics this is built on.
- Enter/Escape keyboard shortcuts for dialogs: Enter triggers the primary
  action (Load, Save, etc.) on every dialog except "Load BASIC Text";
  Escape triggers Cancel on all dialogs.

### Fixed
- macOS: indicator font path no longer hardcodes a Linux-only location;
  tries a candidate list of real macOS system fonts instead.
- macOS: the app window now requests foreground activation on launch,
  instead of opening behind other windows.
- Checking "Auto-Save State on Exit" with no state file ever explicitly
  loaded/saved now defaults to `pc1500emu.state` in the current directory,
  instead of silently doing nothing at exit.

## [0.5.1] - 2026-08-09

### Fixed
- macOS build/run fixes (font path, window activation, auto-save-on-exit).

## [0.5.0] and earlier

Versions before 0.5.1 were not individually tracked in this file. See
`git log` for the full history -- notable earlier work includes the
LH5801 CPU/bus/keyboard/LCD emulation core, BASIC program load/save
(binary and text), interactive DAP debugger, LH5801 disassembler, and
save-state support.
