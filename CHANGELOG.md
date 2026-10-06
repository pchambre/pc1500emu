# Changelog

All notable changes to this project are documented here. Versions follow
`CMakeLists.txt`'s `project(pc1500emu VERSION ...)`, bumped on every push
per this project's own convention (not just milestones).

## [0.13.0] - 2026-10-06

### Added
- Wi-Fi in the expansion mock (the firmware's `EXP_COMMAND_WIFI_*`):
  pretend networks in place of the CYW43's radio, by default one open
  network called `HOST`; connecting remembers a network with its password.
  Tests: `testWifiScanConnectAndPassword` (WFSCAN, the masked password
  prompt, remembered networks, WEP refused), `testWifiConnectByNameAndForget`
  and `testWifiStatAndMlogClear`.
- `testBlkbdPairsKeyboard` checks that BLKBD now turns the keyboard driver
  on after pairing (the hook, BASWORD's flag, the loop served), on a boot
  that had MCONF BLKBD=0.
- `waitForIdle` also recognises the keyboard driver's idle point.
- A Debug build's CRT/STL assertions go to stderr instead of a dialog, so
  an unattended test run reports them instead of waiting for a click.

### Fixed
- The mock's keyboard pairing status copied its code from two different
  `"123456"` literals ("transposed pointer range" in Debug builds).

## [0.12.4] - 2026-10-06

### Added
- The expansion mock knows the firmware's ninth setting, `MCONF POWMANDELAY`
  (default -1). `testMconfShowsAndSetsSettings` sets and shows it, including
  -1, and rejects values out of range.

## [0.12.3] - 2026-10-06

### Added
- `testBlkeyLoadSaveTranslation`: with `MCONF BLKBD=1`, a BASIC program's
  `INKEY$` loads as the module's `BLKEY$`, and a save always writes
  `INKEY$`; quoted text is left alone.
- `testBlkeyReadsEitherKeyboard`: `BLKEY$` sees a key from the external
  keyboard and from the PC-1500's own.

### Changed
- `testBasicXlateChunks` passes the translator's new swaps argument.

## [0.12.2] - 2026-10-06

### Changed
- The mock's keyboard install follows the firmware's `kbd_loop_install()`,
  which now says why it refused a loop.

### Added
- `testExternalKeyboardOldRom`: an older PC-1500 ROM (a real machine's
  dump, `S1500ROM.BIN`), whose 79D4H keyboard hook jumps through X instead
  of its vector. With `MCONF BLKBD=1` the loop is refused, the hook stays
  unarmed, the machine's own keyboard still works, and `BLKBD` shows
  `BLKBD: NOT ON THIS ROM`.

## [0.12.1] - 2026-10-05

### Changed
- The mock's `BLKBD ?` answer carries the firmware's new "where pairing
  failed" bytes (zero).

### Added
- `testExternalKeyboardDriver`: `MCONF BLKBD=0` unhooks the keyboard
  driver at once, so a key still "held" in the external keyboard's byte
  no longer matters (on hardware every character typed was being wiped);
  `testBlkbdPairsKeyboard` checks `BLKBD ?`.

## [0.12.0] - 2026-10-05

### Added
- **Link pairing** (the expansion firmware's `BLPAIR`/`BLUNPAIR`,
  `RP2350/BLE_PROTOCOL.md` sec.7): every link is authenticated with a key
  from a one-time pairing.
  - `LinkCore` pairs and authenticates in both roles, using the firmware's
    `link_secure.c` (Monocypher).
  - The emulator's identity and pairings are kept in this user's settings
    folder, never the files folder peers can read: DPAPI-encrypted on
    Windows, owner-only elsewhere.
  - Pipe commands: `ble pair [yes|no]` (a PC-1500's pairing waiting here),
    `ble pairings`, `ble forget ID|all`.
- **The external keyboard** (the expansion firmware's `MCONF BLKBD`, `BLKBD`):
  - the mock compiles the firmware's `kbd_seq.c`. It sequences keys on the
    emulated clock, and Bus serves the window bytes the module ROM's keyboard
    driver reads (key at 87EFH, ON count at 87F5H);
  - the mock answers `KBD_INSTALL` as the firmware does, so the boot hook's
    copy of ROM1's keyboard wait loop gets patched in, and Bus serves the
    patched bytes;
  - it answers `BLKBD`'s pairing with a pretend keyboard;
  - `kbdReport()` takes a keyboard's HID boot reports;
  - pipe commands: `kbdtype <text>` (`\r` = ENTER) and `kbdbreak`.
- Tests:
  - `testBlePairing` and `ble_link_core_test`'s `testPairing`;
  - `testExternalKeyboardDriver`: boot install with and without AUTOSTAGE,
    typing, shifted symbols, BREAK at INPUT, OFF/ON, HID reports;
  - `testBlkbdPairsKeyboard`;
  - `testMconfShowsAndSetsSettings` covers `BLKBD`.

## [0.11.0] - 2026-10-01

### Added
- **The CE-150 printer/plotter, as the expansion firmware stands in for it**
  (`COLOR`, `CSIZE`, `GRAPH`, `GLCURSOR`, `LCURSOR`, `LF`, `LINE`, `LLIST`,
  `LPRINT`, `RLINE`, `ROTATE`, `SORGN`, `TAB`, `TEST`, `TEXT`), drawn over BLE
  as `PLOT` frames:
  - the mock compiles the firmware's `plotter.c`, `plot_text.c` and
    `hershey_simplex.c`; its fake peer draws the frames on a `PlotPaper`;
  - `LinkCore` sends `PLOT` as a connector and draws the frames it
    receives as a server;
  - a plotter panel (Settings > Bluetooth > Show Plotter Panel) shows the
    paper: from a connected PC-1500 with host Bluetooth, otherwise from the
    emulated one;
  - the detokenizer knows the module's E1C0-E1C6 codes for the CE-150's
    seven E6xx keywords;
  - a BASIC program's `SAVE`/`LOAD` passes through the firmware's
    `basic_xlate.c`, so files hold the CE-150's codes.
- Tests:
  - `testBasicXlateChunks`;
  - `testCe150CodesSaveLoadAndHandOver`, with the real `CE-150.ROM`
    attached at A000H;
  - `LLIST "label"`, `LPRINT USING` and GRAPH mode's bare `LPRINT`, in
    `testCe150Plotter`.
- Tests:
  - `testCe150Plotter`;
  - `testCe150Globe`, which runs `GLOBE.BAS` end to end, only when asked for
    by name;
  - `ble_link_core_test`'s `testPlot`.
- `expansion_keyword_test [name]` runs only the tests whose names contain it.

### Changed
- The expansion keyword tests run on a machine with the 26K of RAM the
  module adds (16K at 0000H, 10K at 4800H).

## [0.10.0] - 2026-09-30

### Added
- **BLE messaging** (the expansion firmware's `BLSEND`/`BLRECV`, `MSG` frames).
  - The mock's fake peer can send messages (optionally a few polls late),
    record what was sent, and fake a full inbox.
  - `LinkCore` keeps the 8-message inbox in both roles over host
    Bluetooth: a new link empties it, a dropped one doesn't.
- **Keywords as BASIC functions:** `BLSTAT` (messages waiting) and
  `SDEOF(n)` (end of an SD channel). The mock routes their `FN_*`
  commands to the firmware's `kw_function()`, and answers
  `SD_CHANNEL_EOF` from its own channels.
- Tests:
  - `testBleMessaging` and `testSdeofEndsReadLoop`: expressions, inside
    another keyword, in a running program (`IF ... THEN LET`), error
    numbers;
  - `ble_link_core_test`'s `testMessages`.
- The boot-hook test now checks that `STAGE RAM` refreshes a copy that's
  already staged, while a reset still skips it.

### Note
- `expansion_keyword_test` takes about 11 minutes: the mock keeps the MCU
  to real time.

## [0.9.1] - 2026-09-28

### Added
- **`SDSAVE` with no name** (the expansion firmware's keywords.c): it saves
  as the last BASIC program `SDLOAD` loaded, asking before overwriting.
  With nothing loaded, it's still ERROR 1.
- `ExpansionMock` calls the firmware's new `kw_reset()` when a module is
  loaded, so what the keywords remember starts clean, as on a real MCU at
  power-up (and doesn't leak between tests).
- Test: `testSdsaveBareSavesAsLastLoaded`.

## [0.9.0] - 2026-09-28

### Added
- **Peer-to-peer files between two PC-1500s** (the expansion firmware's
  BLE milestone 2; `RP2350/BLE_PROTOCOL.md` "Peer-to-peer files"): the
  emulated module answers the new BLE commands for `BLADV`, `BLPUT` and
  `BLGET`, so the emulator can be the second PC-1500 to a real one.
  - The protocol core (`ble_link_core`) holds and answers `FILE_OFFER` /
    `FILE_ANSWER` in both roles, and takes the emulated PC-1500's own
    keywords while another PC-1500 is connected to it.
  - The fake peer can play a PC-1500 in tests: it connects to a `BLADV`,
    answers a `BLPUT`'s offer, and offers files to `BLGET`.
  - Tests: keyword tests for all three keywords, and a loopback test of
    offers and transfers in both directions.
- **`MCONF AUTOSTAGE`** in the mock (default 0: the boot hook stages the
  ROM only when it's 1), plus the MCU executor's other changes: `BLCONNECT`
  is now `BLCON`, and BLE transfers show `SENDING...` / `RECEIVING...` /
  `SAVING...` / `LOADING...`.

### Fixed
- **Windows host Bluetooth:** a link the peer ended left the emulator's
  connector session open (with `MaintainConnection` on). A later link as
  the advertiser then sent every frame down the dead one ("The object has
  been closed"), and Windows kept holding on to the device.
- A failed connect now says whether the peer's services couldn't be read
  (with the GATT status) or the Link service wasn't among them.

## [0.8.0] - 2026-09-28

### Added
- **Host Bluetooth:** the emulated expansion module's BLE keywords can now
  use this computer's own Bluetooth, which makes the emulator a real BLE
  peer for a PC-1500 (see the README's "Bluetooth" section).
  - **Roles:** it can advertise the Link service as a server, which a real
    PC-1500 has connected to with `BLSCAN`, and it can connect out to other
    Link servers.
  - **Platforms:**
    - Windows: C++/WinRT, MSVC only.
    - macOS: CoreBluetooth. Not yet built or tested.
    - Linux: BlueZ over D-Bus via libsystemd. Compile-checked only.
  - When host Bluetooth isn't available, the Bluetooth panel and
    `ble status` say why.
- The protocol core (`src/bus/ble_link_core.*`) sits behind a
  `BleBackend` seam in `ExpansionMock`. It has loopback tests
  (`ble_link_core_test`).
- **BLE fake peer:** the default backend, with no radio, used by the tests.
- **Settings > Bluetooth menu and Bluetooth panel.**
- **Pipe commands:** `ble backend|advertise|status|text|log`.
- **AppConfig fields:** `bleHostBluetooth`, `bleFilesDir`,
  `showBluetoothWindow`.
- **`MCONF HOSTNAME`:** the name the PC-1500 gives on the BLE link, in the
  expansion mock (default `PC-1500 EMU`).
- **Expansion mock:** runs the board's keyword executor, has flash-backed
  settings (all six MCONF settings, LOGSIZE) and STAGE remap support.

### Changed
- `ExpansionMock` commands run on a background thread, paced so the
  emulated CPU and the mock MCU stay in step with real time.
- All expansion keyword tests are enabled.

## [0.7.3] - 2026-08-21

### Fixed
- `ERROR 13` on RESERVE-key assignment could still occur under a non-bare
  RAM configuration even with 0.7.2's `reseedReserveArea()` fix, if the
  saved state file being auto-loaded on startup predated that fix (or was
  saved under a different RAM configuration): `Bus::loadState` restored
  `me0_` and the RAM-config scalars directly, bypassing the setters (and
  therefore `reseedReserveArea()`) entirely. `Bus::loadState` now refuses
  to load a state file whose saved RAM configuration doesn't match the
  Bus's current configuration at load time, leaving the Bus completely
  untouched -- raw RAM contents (including the reserve area) are only
  meaningful relative to the specific memory-map shape they were saved
  under, the same way real PC-1500 RAM (short of a battery-backed module)
  doesn't survive a hardware reconfiguration either. `main.cpp`'s startup
  sequence now applies `AppConfig`'s persisted hardware settings
  unconditionally, before attempting any state-file load, so there's
  always a well-defined "current config" for this check.

### Added
- Rather than silently discarding a saved session that fails the new
  config-mismatch check above, `pc1500emu` now shows a startup popup
  explaining what didn't match and offering to apply the saved
  configuration and retry the restore, in addition to just starting fresh
  with the current settings -- the session is still sitting right there
  in the state file, just not currently loadable.

### Changed
- Help > Special Keys' Tab row shortened to "SHIFT (toggles SHIFT and
  clears on next keypress)" -- clearer about which physical key is meant
  and less text than 0.7.2's rewording.

## [0.7.2] - 2026-08-21

### Fixed
- The PC-1500A's F1-F6 "reserve key" assignment feature (`SHIFT+MODE`)
  threw `ERROR 13` ("insufficient space for RESERVE") under any non-bare
  RAM configuration (e.g. 16K at `0000H`, CE-155). Root cause: the
  reserve area's required zero-seeding (the ROM never initializes it
  itself -- see the 2026-07-26 fix below) was a one-time, hardcoded
  `4008H`-`40C4H` applied only at `Bus` construction, never re-applied
  when the live reserve area moves along with `basicProgramStart()`'s own
  RAM-config-dependent origin. `Bus::reserveAreaBase()`/
  `reseedReserveArea()` now compute and re-seed the correct live location
  on construction and on every RAM-config-changing setter.
- Text sent through any FIFO/pipe text-typing command (`type`,
  `typeline`/`typelinetrace`/`typelinenoenter`/`typelinepartialidle`/
  `typelinepartialidletrace`/`typelinewatch`, `loadbasictext`) with SML
  (lowercase mode) active could silently drop spaces, digits, and
  punctuation immediately following a case change -- the shared
  `SmlAwareTyper` helper was toggling SML around *every* character whose
  literal-lowercase-ness didn't match the current mode, including
  characters with no case-dependent representation at all. Real/live
  keyboard typing never had this bug (a human only presses SML around an
  actual letter). Now scoped to letters only.
- The PC-1500A has real, independent 1K RAM at `7C00H`-`7FFFH`, unlike
  the base PC-1500 (a mirror of `7800H`-`7BFFH`, unchanged).
  `Bus::effectiveAddr` is now machine-variant-aware for this range.
- The in-app Help > Special Keys popup implied host Shift itself toggles
  PC-1500 SHIFT -- it doesn't (only Tab does, host Shift is inert); the
  table's wording was just misleading, not the underlying keyboard
  handling. Reworded.

### Changed
- Extracted the previously-duplicated SML-tracking logic (independently
  reimplemented in `main.cpp`'s `type` handler and
  `typeBasicProgramText`) into one shared `SmlAwareTyper` class
  (`src/basic/text_loader.h`), now used by every text-sending path
  instead of two copies plus several gaps.

## [0.6.6] - 2026-08-11

### Added
- `pc1500disasm` now annotates individual bits of `STATUS1`/`STATUS2`
  (`764EH`/`764FH`) when a `bii`/`ani`/`ori` instruction's immediate mask
  touches one, e.g. `bii (0x764E),0x02` now shows `[bit: SHIFT]` alongside
  the byte-level name, instead of every instruction touching that byte
  showing an identical generic annotation. Bit names are from the PC-1500
  Technical Reference Manual's own bit-layout table (p.98).

## [0.6.5] - 2026-08-11

### Fixed
- `NUMCMP` (numeric comparison entry point) was cataloged at `D9D2H` --
  corrected to `D0D2H` (likely a 9/0 transcription error in the original
  source), confirmed against both a real `ROM1.BIN` disassembly and the
  PC-1500 Technical Reference Manual's own system-subroutine table.
- Two `CE-150` printer entry points were mislabeled: `A8DDH` (was
  `PRT_LF`, is actually printer motor drive) and `AA09H` (was
  `PRT_PEN_UPDOWN`, an address the manual doesn't list at all) -- the
  real `PRT_LF` is `A9F1H` and the real `PRT_PEN_UPDOWN` is `AAE3H`, both
  confirmed against this project's own `CE-150.ROM`.
- The cassette/printer `CE-150` module entries' own comments said they
  load at `8000H` (per the PC-2 Assembly manual's prose) -- corrected to
  `A000H`, confirmed against both the Technical Reference Manual's own
  memory map and this project's `CE-150.ROM` disassembly.

### Added
- Three new confirmed `CE-150`/ROM entry points from the PC-1500
  Technical Reference Manual's own system-subroutine table (p.120-121):
  `PRT_TEXT_MODE` (`ACBBH`), `DISP_GRAPHIC` (`EDEFH`), and
  `TAPE_IO_CONTROL` (`BBF5H`).

## [0.6.4] - 2026-08-11

### Added
- `pc1500disasm` now annotates the BASIC interpreter's own named RAM
  variables (WAIT counter, FOR/GOSUB pointers, current/previous/search/
  break/error line+address+top fields, ON ERROR GOTO target, USING format
  state, pen-plotter/printer variables) in `7800H`-`7BFFH`, transcribed
  from the PC-1500 Technical Reference Manual's own table (pp.100-101) --
  e.g. `ori (0x78B8),0x80` now shows `; ON_ERROR_ADDRESS_H -- ...`.

## [0.6.3] - 2026-08-11

### Fixed
- BREAK (F12) stopped working to interrupt a running program. The MI
  interrupt handler itself reads the IF register to check an unrelated
  bit as part of its own dispatch logic, and an earlier fix cleared
  BREAK's own flag as an unintended side effect of *any* read of that
  register -- so every BREAK-triggered interrupt silently consumed its
  own flag before the interpreter's break-check ever saw it.
- Several less-common ways of driving the CPU (the `break`/`run`/`trace`
  FIFO commands, debugger single-stepping, and a few internal keystroke-
  typing helpers) didn't advance the real-time clock the same way the
  main loop does, so a `WAIT`/`BEEP` in progress during one of those could
  stall or run at the wrong rate.
- Escape now dismisses the "Special Keys" and "About" dialogs, matching
  every other dialog.

### Added
- `--no-state` command-line flag: boot cold this run without touching the
  configured state file (skips both auto-load and auto-save-on-exit for
  the session), for testing/reproduction runs that need a known starting
  point.

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
