// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
//
// Commands this extension contributes:
//   - Disassemble to ASM   -- run pc1500disasm on a ROM/BIN file.
//   - Assemble to BIN      -- run sdaslh5801 + sdld + makebin on a .asm file.
//   - Build C to BIN       -- run sdcc-pc1500's build-lh5801.sh on a .c file.
//   - Run in pc1500emu     -- load an assembled/built program into a (possibly
//                             freshly-launched) pc1500emu instance and run it.
//   - Debug in pc1500emu   -- same, but via the lh5801 Debug Adapter.
//
// Plain CommonJS, no build step -- this extension is installed by copying
// the folder directly into %USERPROFILE%\.vscode\extensions\ (see
// README.md), not via `vsce package`, so anything here has to run as-is
// with no compile step.
//
// package.json declares "extensionKind": ["ui"] -- this extension always
// runs in the local (Windows) extension host, never a remote one (e.g.
// Remote-WSL), since its whole job is spawning Windows-native processes
// (wsl.exe, pc1500emu.exe) and talking to a Windows named pipe, none of
// which make sense running inside WSL itself. Without that declaration,
// opening a WSL-side folder (e.g. sdcc-pc1500's own source tree via
// Remote-WSL) makes VS Code try to run the extension in the remote host
// instead, where it was never installed -- it silently never activates
// and its menus don't appear (confirmed: this is exactly what happened
// when a sdcc-pc1500 example folder was opened via Remote-WSL).
'use strict';

const vscode = require('vscode');
const { execFile, spawn } = require('child_process');
const path = require('path');
const fs = require('fs');
const os = require('os');
const net = require('net');

// Mirrors pc1500disasm's own --mode values and default --base (see
// src/disasm/disasm_main.cpp) -- keep these two in sync if that ever
// changes.
const MODES = [
  { label: 'Base ROM', description: 'e.g. ROM1.BIN', mode: 'base', defaultBase: '0xC000' },
  { label: 'Module ROM', description: 'expansion/plug-in ROM, e.g. CE-150.ROM', mode: 'module', defaultBase: '0x8000' },
  { label: 'Standalone program', description: 'a BASIC-POKEd/CALLed ML routine (.bin)', mode: 'program', defaultBase: '' },
];

// Matches main.cpp's/emulator_client.cpp's own command-pipe paths exactly
// on each platform: a Windows named pipe (+ %TEMP%-based response file,
// queried dynamically the same way GetTempPathA does), or a POSIX FIFO at
// a fixed /tmp path (response file path is a literal in main.cpp too, NOT
// derived from $TMPDIR/os.tmpdir() -- Mac's os.tmpdir() in particular is
// normally nowhere near /tmp, so this must stay a hardcoded literal to
// match).
const IS_WINDOWS = process.platform === 'win32';
const WIN_PIPE_PATH = '\\\\.\\pipe\\pc1500emu.cmd';
const POSIX_FIFO_PATH = '/tmp/pc1500emu.cmd';
const POSIX_RESPONSE_PATH = '/tmp/pc1500emu.response';
function responsePath() {
  return IS_WINDOWS ? path.join(os.tmpdir(), 'pc1500emu.response') : POSIX_RESPONSE_PATH;
}

let diagnostics;

function activate(context) {
  diagnostics = vscode.languages.createDiagnosticCollection('lh5801');
  context.subscriptions.push(diagnostics);
  context.subscriptions.push(
    vscode.commands.registerCommand('lh5801-asm.disassemble', (clickedUri) => disassemble(clickedUri)),
    vscode.commands.registerCommand('lh5801-asm.assemble', (clickedUri) => assembleCommand(clickedUri)),
    vscode.commands.registerCommand('lh5801-asm.buildC', (clickedUri) => buildCCommand(clickedUri)),
    vscode.commands.registerCommand('lh5801-asm.run', (clickedUri) => runCommand(clickedUri)),
    vscode.commands.registerCommand('lh5801-asm.debugInEmulator', (clickedUri) => debugCommand(clickedUri))
  );
}

async function disassemble(clickedUri) {
  const inputUri = await resolveInputUri(clickedUri, /\.(rom|bin)$/i, { 'ROM/Binary': ['rom', 'bin'], 'All files': ['*'] });
  if (!inputUri) return;  // cancelled, or nothing to disassemble

  const disasmCommand = resolveCommandSetting('lh5801.disasmCommand', inputUri);
  if (!disasmCommand) {
    await complainMissingSetting('lh5801.disasmCommand', "point it at your built pc1500disasm(.exe) first.");
    return;
  }

  const modePick = await vscode.window.showQuickPick(MODES, { placeHolder: 'pc1500disasm mode' });
  if (!modePick) return;

  // Always shown, even when a default exists -- a wrong --base silently
  // produces a nonsensical disassembly rather than an error, so this is
  // worth one extra confirmation click rather than a silent default.
  const base = await vscode.window.showInputBox({
    prompt: modePick.mode === 'program'
      ? 'Load address (required -- no universal convention for a standalone program)'
      : `Load address (default ${modePick.defaultBase})`,
    value: modePick.defaultBase,
    validateInput: (v) => (/^(0x)?[0-9A-Fa-f]{1,4}$/.test(v.trim()) ? null : 'Enter a hex address, e.g. 0x4268'),
  });
  if (base === undefined) return;  // cancelled

  const defaultOutUri = vscode.Uri.file(inputUri.fsPath.replace(/\.[^./\\]+$/, '') + '.asm');
  const outUri = await vscode.window.showSaveDialog({
    defaultUri: defaultOutUri,
    filters: { 'LH5801 Assembly': ['asm'] },
    saveLabel: 'Disassemble to',
  });
  if (!outUri) return;

  const args = ['--mode', modePick.mode, '--base', base.trim(), inputUri.fsPath, '-o', outUri.fsPath];
  const cwd = workspaceCwdFor(inputUri);

  await withProgress(`Disassembling ${path.basename(inputUri.fsPath)}...`, async () => {
    const { error, stderr } = await execPlainCommand(disasmCommand, args, cwd);
    if (error) {
      vscode.window.showErrorMessage(`pc1500disasm failed: ${(stderr || error.message).trim()}`);
      return;
    }
    if (stderr && stderr.trim()) {
      // Non-fatal warnings (e.g. low-confidence keyword-table candidates
      // -- see --seed in README.md) -- the output was still written, but
      // this is worth surfacing, not discarding.
      const firstLine = stderr.trim().split('\n')[0];
      vscode.window.showWarningMessage(`pc1500disasm: ${firstLine}`);
    }
    const doc = await vscode.workspace.openTextDocument(outUri);
    await vscode.window.showTextDocument(doc);
  });
}

// ---------------------------------------------------------------------
// Assemble to BIN (sdaslh5801 + sdld + makebin)
// ---------------------------------------------------------------------

// Assembles inputUri (a .asm/.s file) to a flat .bin beside it, running
// entirely in the source file's own directory so the outputs match the
// ${fileDirname}/${fileBasenameNoExtension}.{bin,lst} convention
// .vscode/launch.json's templates already assume.
//
// Returns { binPath, lstPath, loadAddressHex } on success, or null if the
// user cancelled/a setting was missing (already reported). Throws on an
// actual tool failure with diagnostics already populated -- callers
// running as part of a bigger workflow (Run/Debug) should let that
// propagate and stop, since there's nothing useful left to load.
async function assembleAsm(inputUri) {
  const sdas = resolveCommandSetting('lh5801.sdasCommand', inputUri);
  const link = resolveCommandSetting('lh5801.linkCommand', inputUri);
  const makebin = resolveCommandSetting('lh5801.makebinCommand', inputUri);
  if (!sdas) { await complainMissingSetting('lh5801.sdasCommand', 'point it at your built sdaslh5801 first.'); return null; }
  if (!link) { await complainMissingSetting('lh5801.linkCommand', 'point it at your built sdld first.'); return null; }
  if (!makebin) { await complainMissingSetting('lh5801.makebinCommand', 'point it at your built makebin first.'); return null; }

  const dir = path.dirname(inputUri.fsPath);
  const srcName = path.basename(inputUri.fsPath);
  const base = srcName.replace(/\.[^./\\]+$/, '');
  const relName = base + '.rel';
  const lstPath = path.join(dir, base + '.lst');
  const ihxPath = path.join(dir, base + '.ihx');
  const binPath = path.join(dir, base + '.bin');
  const lnkPath = path.join(dir, base + '.lnk');

  diagnostics.delete(inputUri);

  return withProgress(`Assembling ${srcName}...`, async () => {
    // --- assemble ---------------------------------------------------
    const asmResult = await runToolCommand(sdas, ['-l', '-o', relName, srcName], dir);
    const errors = parseSdasErrors(asmResult.stderr + '\n' + asmResult.stdout, inputUri);
    if (errors.length) diagnostics.set(inputUri, errors);
    if (asmResult.error || errors.length) {
      const msg = errors.length
        ? `sdaslh5801: ${errors.length} error(s) -- see Problems panel`
        : `sdaslh5801 failed: ${(asmResult.stderr || asmResult.error.message).trim()}`;
      vscode.window.showErrorMessage(msg);
      throw new Error(msg);
    }

    // --- link (single .rel, so no explicit area basing is needed --
    // this project's .asm files always fix their own load address via
    // an ABS area + .org, see docs and ce150.asm) --------------------
    fs.writeFileSync(lnkPath, `-muwx\n-i ${base}\n${relName}\n\n-e\n`);
    const ldResult = await runToolCommand(link, ['-nf', base], dir);
    if (ldResult.error || !fs.existsSync(ihxPath)) {
      const msg = `sdld failed: ${(ldResult.stderr || (ldResult.error && ldResult.error.message) || 'no .ihx produced').trim()}`;
      vscode.window.showErrorMessage(msg);
      throw new Error(msg);
    }

    const loadAddressHex = readIhxLoadAddress(ihxPath);
    if (!loadAddressHex) {
      const msg = `sdld produced ${base}.ihx with no data records -- nothing to load.`;
      vscode.window.showErrorMessage(msg);
      throw new Error(msg);
    }

    // --- makebin: strip everything before the real load address, and
    // trim trailing padding (-p) so the .bin is exactly the program's
    // own bytes, loadable with `loadbinary <loadAddressHex> <binPath>`.
    const mbResult = await runToolCommand(makebin, ['-p', '-o', '0x' + loadAddressHex, base + '.ihx', base + '.bin'], dir);
    if (mbResult.error || !fs.existsSync(binPath)) {
      const msg = `makebin failed: ${(mbResult.stderr || (mbResult.error && mbResult.error.message) || 'no .bin produced').trim()}`;
      vscode.window.showErrorMessage(msg);
      throw new Error(msg);
    }

    return { binPath, lstPath: fs.existsSync(lstPath) ? lstPath : undefined, loadAddressHex };
  });
}

async function assembleCommand(clickedUri) {
  const inputUri = await resolveInputUri(clickedUri, /\.(asm|s)$/i, { 'LH5801 Assembly': ['asm', 's'], 'All files': ['*'] });
  if (!inputUri) return;
  try {
    const result = await assembleAsm(inputUri);
    if (result) vscode.window.showInformationMessage(`Assembled ${path.basename(result.binPath)} (load at 0x${result.loadAddressHex}).`);
  } catch {
    // already reported to the user above
  }
}

// sdaslh5801's error format (confirmed against real output and
// .vscode/tasks.json's own problemMatcher, which targets the same
// pattern): "<file>:<line>: Error: <message>". The file segment can be
// whatever path form was passed on the command line (e.g. a WSL
// /mnt/c/... path) -- always attributed back to `sourceUri` (the real,
// known input) instead of trying to round-trip that string.
function parseSdasErrors(text, sourceUri) {
  const found = [];
  const re = /^.*?:(\d+): Error: (.*)$/gm;
  let m;
  while ((m = re.exec(text))) {
    const line = Math.max(0, parseInt(m[1], 10) - 1);
    const range = new vscode.Range(line, 0, line, 1000);
    found.push(new vscode.Diagnostic(range, m[2], vscode.DiagnosticSeverity.Error));
  }
  return found;
}

// Reads the load address (4 uppercase hex digits, no 0x) baked into an
// Intel HEX file's first data record (type 00) -- i.e. where an ABS-area
// .org placed the code, independent of the linker's own area-base
// bookkeeping (see assembleAsm's comment on why no -b is needed).
function readIhxLoadAddress(ihxPath) {
  const lines = fs.readFileSync(ihxPath, 'utf8').split(/\r?\n/);
  for (const line of lines) {
    const m = /^:[0-9A-Fa-f]{2}([0-9A-Fa-f]{4})00/.exec(line);
    if (m) return m[1].toUpperCase();
  }
  return null;
}

// ---------------------------------------------------------------------
// Build C to BIN (build-lh5801.sh)
// ---------------------------------------------------------------------

async function buildC(inputUri) {
  const buildScript = resolveCommandSetting('lh5801.buildCCommand', inputUri);
  if (!buildScript) { await complainMissingSetting('lh5801.buildCCommand', 'point it at sdcc-pc1500\'s build-lh5801.sh first.'); return null; }
  // build-lh5801.sh is a bash script -- on Windows that always needs WSL
  // (no native bash), but on Linux/Mac it runs directly (a plain path, or
  // "bash <path>"), so this check is Windows-only.
  if (IS_WINDOWS && !/^\s*wsl(\.exe)?(\s|$)/i.test(buildScript)) {
    vscode.window.showErrorMessage("lh5801.buildCCommand must start with 'wsl' on Windows -- build-lh5801.sh is a bash script.");
    return null;
  }

  const baseHex = await vscode.window.showInputBox({
    prompt: 'Load address, hex, no 0x prefix (must be above 0x40C4 -- the BASIC firmware reserve area)',
    value: '4400',
    validateInput: (v) => (/^[0-9A-Fa-f]{1,4}$/.test(v.trim()) ? null : 'Enter a hex address, e.g. 4400'),
  });
  if (baseHex === undefined) return null;

  const dir = path.dirname(inputUri.fsPath);
  const srcName = path.basename(inputUri.fsPath);

  return withProgress(`Building ${srcName}...`, async () => {
    const result = await runToolCommand(buildScript, [srcName, baseHex.trim()], dir);
    if (result.error) {
      const msg = `build-lh5801.sh failed: ${(result.stderr || result.error.message).trim()}`;
      vscode.window.showErrorMessage(msg);
      throw new Error(msg);
    }
    // Script prints the output path on its own first line, then
    // "  size: N bytes, load at 0xBASE, call at 0xCALL" on the second --
    // see build-lh5801.sh's own final echo lines. Under the (always-WSL,
    // see the setting's own validation above) case, that first line is a
    // /mnt/c/... path from the script's own $(pwd) -- confirmed live it
    // is NOT usable as-is from the Windows side, so it's rebuilt from the
    // known Windows `dir` instead of trying to reverse-translate it.
    const outLines = result.stdout.trim().split('\n');
    const outPathRaw = (outLines[0] || '').trim();
    const infoLine = outLines[1] || '';
    const loadMatch = /load at 0x([0-9A-Fa-f]+)/.exec(infoLine);
    const callMatch = /call at 0x([0-9A-Fa-f]+)/.exec(infoLine);
    const outPath = outPathRaw ? path.join(dir, path.basename(outPathRaw)) : '';
    if (!outPath || !fs.existsSync(outPath) || !loadMatch || !callMatch) {
      const msg = `build-lh5801.sh: couldn't parse its own output:\n${result.stdout}\n${result.stderr}`;
      vscode.window.showErrorMessage(msg);
      throw new Error(msg);
    }
    return {
      binPath: outPath,
      loadAddressHex: loadMatch[1].toUpperCase().padStart(4, '0'),
      entryHex: callMatch[1].toUpperCase().padStart(4, '0'),
    };
  });
}

async function buildCCommand(clickedUri) {
  const inputUri = await resolveInputUri(clickedUri, /\.c$/i, { 'C source': ['c'], 'All files': ['*'] });
  if (!inputUri) return;
  try {
    const result = await buildC(inputUri);
    if (result) {
      vscode.window.showInformationMessage(
        `Built ${path.basename(result.binPath)} (load at 0x${result.loadAddressHex}, call at 0x${result.entryHex}).`
      );
    }
  } catch {
    // already reported to the user above
  }
}

// ---------------------------------------------------------------------
// Run / Debug in pc1500emu
// ---------------------------------------------------------------------

// Resolves clickedUri (or the active editor, or a picked file) to a
// loadable program, assembling/building it first if it's source. Returns
// { binPath, loadAddressHex, entryHex, lstPath?, sourceUri? } or null if
// cancelled/failed (already reported).
async function resolveProgram(clickedUri) {
  const inputUri = await resolveInputUri(clickedUri, /\.(asm|s|c|bin|rom)$/i, {
    'LH5801 program': ['asm', 's', 'c', 'bin', 'rom'],
    'All files': ['*'],
  });
  if (!inputUri) return null;

  if (/\.c$/i.test(inputUri.fsPath)) {
    const built = await buildC(inputUri);
    return built && { ...built, sourceUri: inputUri };
  }
  if (/\.(asm|s)$/i.test(inputUri.fsPath)) {
    const assembled = await assembleAsm(inputUri);
    return assembled && { ...assembled, entryHex: assembled.loadAddressHex, sourceUri: inputUri };
  }

  // Already a .bin/.rom -- if we assembled it ourselves earlier, a
  // sibling .ihx tells us its real load address; otherwise ask.
  const siblingIhx = inputUri.fsPath.replace(/\.[^./\\]+$/, '.ihx');
  let loadAddressHex = fs.existsSync(siblingIhx) ? readIhxLoadAddress(siblingIhx) : null;
  if (!loadAddressHex) {
    const entered = await vscode.window.showInputBox({
      prompt: `Load address for ${path.basename(inputUri.fsPath)}, hex, no 0x prefix`,
      value: '4400',
      validateInput: (v) => (/^[0-9A-Fa-f]{1,4}$/.test(v.trim()) ? null : 'Enter a hex address, e.g. 4400'),
    });
    if (entered === undefined) return null;
    loadAddressHex = entered.trim().toUpperCase().padStart(4, '0');
  }
  return { binPath: inputUri.fsPath, loadAddressHex, entryHex: loadAddressHex, sourceUri: inputUri };
}

// Prompts for how to load a program: as a plain application (loadbinary)
// or as a CE-150/153/158-style plug-in ROM module (loadrommodule, one of
// four independent slots), plus that mode's own flags. Returns
// { mode: 'binary' } or { mode: 'rommodule', slot, requirePv, usePuBank },
// or null if cancelled.
async function pickLoadMode() {
  const modePick = await vscode.window.showQuickPick(
    [
      { label: 'Application', description: 'loadbinary -- write directly into memory', mode: 'binary' },
      { label: 'Extension ROM -- Slot 1', mode: 'rommodule', slot: 0 },
      { label: 'Extension ROM -- Slot 2', mode: 'rommodule', slot: 1 },
      { label: 'Extension ROM -- Slot 3', mode: 'rommodule', slot: 2 },
      { label: 'Extension ROM -- Slot 4', mode: 'rommodule', slot: 3 },
    ],
    { placeHolder: 'Load as' }
  );
  if (!modePick) return null;
  if (modePick.mode === 'binary') return { mode: 'binary' };

  const flags = await vscode.window.showQuickPick(
    [
      { label: 'requirePv', description: 'module requires the PV flag', picked: false },
      { label: 'usePuBank', description: 'module is PU-banked (file must be even-sized)', picked: false },
    ],
    { placeHolder: 'Extension ROM flags (toggle any that apply, then confirm with no selection change if none)', canPickMany: true }
  );
  if (flags === undefined) return null;
  return {
    mode: 'rommodule',
    slot: modePick.slot,
    requirePv: flags.some((f) => f.label === 'requirePv'),
    usePuBank: flags.some((f) => f.label === 'usePuBank'),
  };
}

// Resolves lh5801.emulatorPath/romPath plus the extension-RAM settings
// (lh5801.extRam4800Bytes/extRam0000Bytes) into { emulatorPath, args }
// for spawn() (Run) or the debug launch config's emulatorArgs (Debug).
// When both extRAM settings are 0 (the default), this is just
// [romPath] -- unchanged from before these settings existed. When
// either is non-zero, a temp conf JSON is written (AppConfig's own
// extRam4800Bytes/extRam0000Bytes fields, see
// src/hoststate/app_config.h) and spawned via --conf/--no-state
// instead: --no-state is required because that conf is only applied
// "before cpu.reset() on a fresh boot", not a state restore (confirmed
// via app_config.h's own comment), so without it a resumed saved state
// could silently keep the old RAM size. Returns null (after reporting
// the error) if emulatorPath is missing/doesn't exist, or if romPath is
// blank while extRAM settings need it to build the conf's own romPath
// field (Run alone tolerates a blank romPath in the unchanged case,
// relying on the emulator's own default-conf-file discovery -- but that
// discovery is bypassed entirely once --conf is given explicitly, so
// this specific path has no fallback).
async function resolveEmulatorLaunch(anchorUri) {
  const emulatorPath = resolveCommandSetting('lh5801.emulatorPath', anchorUri);
  if (!emulatorPath) {
    await complainMissingSetting('lh5801.emulatorPath', 'point it at your built pc1500emu.exe first.');
    return null;
  }
  if (!fs.existsSync(emulatorPath)) {
    vscode.window.showErrorMessage(`lh5801.emulatorPath doesn't exist: ${emulatorPath}`);
    return null;
  }

  const romPath = resolveCommandSetting('lh5801.romPath', anchorUri);
  const config = vscode.workspace.getConfiguration('lh5801');
  const extRam4800Bytes = config.get('extRam4800Bytes', 0) || 0;
  const extRam0000Bytes = config.get('extRam0000Bytes', 0) || 0;

  if (!extRam4800Bytes && !extRam0000Bytes) {
    return { emulatorPath, args: romPath ? [romPath] : [] };
  }

  if (!romPath) {
    await complainMissingSetting('lh5801.romPath', 'required to generate a --conf for lh5801.extRam4800Bytes/extRam0000Bytes.');
    return null;
  }

  const confPath = path.join(os.tmpdir(), `pc1500emu-lh5801-${process.pid}-${Date.now()}.json`);
  fs.writeFileSync(
    confPath,
    JSON.stringify({ romPath, extRam4800Bytes, extRam0000Bytes, autoLoadOnStart: false, autoSaveOnExit: false }, null, 2)
  );
  return { emulatorPath, args: ['--conf', confPath, '--no-state'] };
}

// Resolves lh5801.preloadRomModules -- an array of
// { slot?, base, requirePv?, usePuBank?, path } entries to load into a
// *freshly spawned* emulator via loadrommodule[2-4], before the main
// program itself loads. `slot` is 0-3 (default 0), matching
// pickLoadMode()'s own slot numbering.
function resolvePreloadRomModules(anchorUri) {
  const raw = vscode.workspace.getConfiguration('lh5801').get('preloadRomModules', []);
  if (!Array.isArray(raw)) return [];
  return raw.map((m) => ({
    slot: Number.isInteger(m.slot) ? m.slot : 0,
    base: String(m.base || '').trim(),
    requirePv: !!m.requirePv,
    usePuBank: !!m.usePuBank,
    path: expandWorkspaceFolder(String(m.path || '').trim(), anchorUri),
  }));
}

// Polls `status` until two consecutive reads report the same P register
// (a ROM-address-agnostic stability check -- boot time isn't perfectly
// deterministic, so a fixed delay would either be too short on a slow
// run or wastefully long on a fast one). Returns false on timeout or if
// the pipe stops responding.
async function waitForStableBoot(timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  let lastP = null;
  while (Date.now() < deadline) {
    const s = await sendCommand('status', Math.max(500, deadline - Date.now()));
    if (s === null) return false;
    const m = /P=([0-9A-Fa-f]+)/.exec(s);
    const p = m ? m[1] : null;
    if (p !== null && p === lastP) return true;
    lastP = p;
    await new Promise((resolve) => setTimeout(resolve, 250));
  }
  return false;
}

// Ensures a pc1500emu instance is reachable over the command pipe,
// launching one via resolveEmulatorLaunch if a quick probe finds
// nothing already listening. On a fresh spawn only (never when
// attaching to one already running -- that implies the user already
// has it set up the way they want), also preloads
// lh5801.preloadRomModules before returning, skipping (with a warning)
// any entry whose slot collides with `reservedSlot` (the main program's
// own chosen slot, if it's being loaded as a ROM module itself).
// Returns { running: bool, freshlySpawned: bool }.
async function ensureEmulatorRunning(anchorUri, reservedSlot) {
  if ((await sendCommand('status', 700)) !== null) return { running: true, freshlySpawned: false };

  const launch = await resolveEmulatorLaunch(anchorUri);
  if (!launch) return { running: false, freshlySpawned: false };

  const child = spawn(launch.emulatorPath, launch.args, { detached: true, stdio: 'ignore' });
  child.unref();

  if ((await sendCommand('status', 10000)) === null) return { running: false, freshlySpawned: false };

  // The command pipe comes up (SDL/window init) well before a cold-booting
  // ROM's own init sequence actually completes -- confirmed live: loading/
  // calling into the machine before it settles corrupts whatever boot code
  // was still mid-execution (P visibly wanders, RAM variables stay at
  // their 0xFF power-up default). A state-restored boot has no such
  // window (P is already stable the instant the pipe answers), so this
  // only costs a couple of cheap polls in the common case.
  if (!(await waitForStableBoot(5000))) {
    vscode.window.showErrorMessage('pc1500emu: command pipe came up but its boot never settled.');
    return { running: false, freshlySpawned: false };
  }

  for (const mod of resolvePreloadRomModules(anchorUri)) {
    if (reservedSlot !== undefined && mod.slot === reservedSlot) {
      vscode.window.showWarningMessage(
        `pc1500emu: skipping preload ROM module for slot ${mod.slot + 1} -- the main program itself is loading into that slot.`
      );
      continue;
    }
    const cmd = `${mod.slot === 0 ? 'loadrommodule' : 'loadrommodule' + (mod.slot + 1)} ${mod.base} ${mod.requirePv ? 1 : 0} ${mod.usePuBank ? 1 : 0} ${mod.path}`;
    const response = await sendCommand(cmd, 5000);
    if (response === null || !response.trim().startsWith('OK')) {
      vscode.window.showErrorMessage(
        `pc1500emu: failed to preload ROM module (slot ${mod.slot + 1}): ${response === null ? 'no response' : response.trim()}`
      );
    }
  }

  return { running: true, freshlySpawned: true };
}

async function runCommand(clickedUri) {
  try {
    const program = await resolveProgram(clickedUri);
    if (!program) return;
    const loadMode = await pickLoadMode();
    if (!loadMode) return;

    const anchorUri = program.sourceUri || vscode.Uri.file(program.binPath);
    const emu = await ensureEmulatorRunning(anchorUri, loadMode.mode === 'rommodule' ? loadMode.slot : undefined);
    if (!emu.running) return;

    const loadCmd =
      loadMode.mode === 'binary'
        ? `loadbinary ${program.loadAddressHex} ${program.binPath}`
        : `${loadMode.slot === 0 ? 'loadrommodule' : 'loadrommodule' + (loadMode.slot + 1)} ${program.loadAddressHex} ${loadMode.requirePv ? 1 : 0} ${loadMode.usePuBank ? 1 : 0} ${program.binPath}`;

    const response = await sendCommand(loadCmd, 5000);
    if (response === null) {
      vscode.window.showErrorMessage('pc1500emu: no response to load command.');
      return;
    }
    if (!response.trim().startsWith('OK')) {
      vscode.window.showErrorMessage(`pc1500emu: ${response.trim()}`);
      return;
    }

    await sendCommandNoWait(`call ${program.entryHex}`);
    vscode.window.showInformationMessage(`Running ${path.basename(program.binPath)} at 0x${program.entryHex} in pc1500emu.`);
  } catch {
    // already reported to the user above
  }
}

async function debugCommand(clickedUri) {
  try {
    const program = await resolveProgram(clickedUri);
    if (!program) return;
    const loadMode = await pickLoadMode();
    if (!loadMode) return;

    const launchChoice = await vscode.window.showQuickPick(
      [
        { label: 'Attach to running pc1500emu', attach: true },
        { label: 'Launch pc1500emu and debug', attach: false },
      ],
      { placeHolder: 'How should the debugger reach pc1500emu?' }
    );
    if (!launchChoice) return;

    const anchorUri = program.sourceUri || vscode.Uri.file(program.binPath);
    const config = {
      type: 'lh5801',
      request: 'launch',
      name: `Debug ${path.basename(program.binPath)}`,
      program: program.binPath,
      loadAddress: program.loadAddressHex,
      loadMode: loadMode.mode,
      entry: program.entryHex,
      stopOnEntry: true,
    };
    if (program.lstPath) config.listing = program.lstPath;
    if (program.sourceUri) config.sourcePath = program.sourceUri.fsPath;
    if (loadMode.mode === 'rommodule') {
      config.requirePv = loadMode.requirePv;
      config.usePuBank = loadMode.usePuBank;
      // The lh5801 debug adapter's loadrommodule support targets slot 1
      // only (see debug_adapter_main.cpp) -- other slots need the
      // non-debug "Run in pc1500emu" command instead.
      if (loadMode.slot !== 0) {
        vscode.window.showWarningMessage('Debugging only supports Extension ROM Slot 1 -- loading into slot 1 instead.');
      }
    }
    if (!launchChoice.attach) {
      const launch = await resolveEmulatorLaunch(anchorUri);
      if (!launch) return;
      config.emulatorPath = launch.emulatorPath;
      config.emulatorArgs = launch.args;

      // Debug always loads a rommodule program into slot 1 (see the
      // warning above), so that's the only slot a preload entry can
      // collide with here.
      const reservedSlot = loadMode.mode === 'rommodule' ? 0 : undefined;
      const preload = resolvePreloadRomModules(anchorUri).filter((mod) => {
        if (reservedSlot !== undefined && mod.slot === reservedSlot) {
          vscode.window.showWarningMessage(
            `pc1500emu: skipping preload ROM module for slot ${mod.slot + 1} -- the main program itself is loading into that slot.`
          );
          return false;
        }
        return true;
      });
      if (preload.length) config.preloadRomModules = preload;
    }

    const folder = vscode.workspace.getWorkspaceFolder(anchorUri) || vscode.workspace.workspaceFolders?.[0];
    const started = await vscode.debug.startDebugging(folder, config);
    if (!started) vscode.window.showErrorMessage('Failed to start the lh5801 debug session.');
  } catch {
    // already reported to the user above
  }
}

// ---------------------------------------------------------------------
// Command pipe I/O -- mirrors src/debugadapter/emulator_client.cpp's
// EmulatorClient exactly: connect/write/close to send (retrying until a
// deadline if nothing's listening yet -- e.g. right after launching a
// fresh instance), then for sendCommand, poll the response file's
// modification time until it changes. Two platform-specific
// connect-and-write implementations (Windows named pipe vs. POSIX FIFO),
// sharing the same response-file-polling logic above that.
// ---------------------------------------------------------------------

// Windows: connects to the named pipe, retrying on failure (the pipe
// doesn't exist until an instance is running and has opened its
// listener) until `deadline`.
function winConnectAndWrite(command, deadline) {
  return new Promise((resolve) => {
    function tryConnect() {
      if (Date.now() >= deadline) return resolve(false);
      const sock = net.connect(WIN_PIPE_PATH);
      sock.once('connect', () => {
        sock.end(command.endsWith('\n') ? command : command + '\n');
        resolve(true);
      });
      sock.once('error', () => setTimeout(tryConnect, 100));
    }
    tryConnect();
  });
}

// POSIX: matches emulator_client.cpp's own connectAndWrite -- open
// non-blocking first (fails immediately with ENXIO rather than hanging
// forever when no reader/instance exists yet) and retry until
// `deadline`, then write and close. Unlike the C++ side, this doesn't
// switch the fd back to blocking mode before writing (Node's fs module
// doesn't expose fcntl) -- fine in practice since every command here is a
// single short line, far under the pipe buffer size a non-blocking write
// would ever actually block on.
function posixConnectAndWrite(command, deadline) {
  return new Promise((resolve) => {
    function tryOpen() {
      if (Date.now() >= deadline) return resolve(false);
      fs.open(POSIX_FIFO_PATH, fs.constants.O_WRONLY | fs.constants.O_NONBLOCK, (err, fd) => {
        if (err) {
          if (err.code === 'ENXIO') return setTimeout(tryOpen, 100);
          return resolve(false);
        }
        const line = Buffer.from(command.endsWith('\n') ? command : command + '\n');
        fs.write(fd, line, (writeErr, written) => {
          fs.close(fd, () => {});
          resolve(!writeErr && written === line.length);
        });
      });
    }
    tryOpen();
  });
}

function connectAndWrite(command, deadline) {
  return IS_WINDOWS ? winConnectAndWrite(command, deadline) : posixConnectAndWrite(command, deadline);
}

function sendCommandNoWait(command, timeoutMs = 3000) {
  return connectAndWrite(command, Date.now() + timeoutMs);
}

function sendCommand(command, timeoutMs) {
  return new Promise((resolve) => {
    const respPath = responsePath();
    let before = null;
    try {
      before = fs.statSync(respPath).mtimeMs;
    } catch {
      before = null;
    }
    const deadline = Date.now() + timeoutMs;
    let settled = false;
    const finish = (value) => {
      if (!settled) {
        settled = true;
        resolve(value);
      }
    };

    connectAndWrite(command, deadline).then((ok) => {
      if (!ok) return finish(null);
      pollResponse();
    });

    function pollResponse() {
      if (Date.now() >= deadline) return finish(null);
      fs.stat(respPath, (err, stat) => {
        if (settled) return;
        if (!err && (before === null || stat.mtimeMs !== before)) {
          fs.readFile(respPath, 'utf8', (readErr, data) => finish(readErr ? null : data));
        } else {
          setTimeout(pollResponse, 50);
        }
      });
    }
  });
}

// ---------------------------------------------------------------------
// Tool invocation -- the "wsl ..." vs. plain-exe convention shared by
// lh5801.sdasCommand/linkCommand/makebinCommand/buildCCommand (see their
// package.json descriptions and .vscode/settings.json's own comment).
// ---------------------------------------------------------------------

// A configured command is a single string split on spaces (no quoting
// support, matching .vscode/tasks.json's existing convention) -- either
// a plain native command, or "wsl <linux-path>" for a WSL build. In the
// WSL case, `cwd` is translated and passed via `--cd` (confirmed: wsl.exe
// also auto-translates the calling process's own Windows CWD the same
// way, but an explicit --cd is used here to not depend on that), and any
// absolute Windows-style path (C:\...) among `args` is translated to
// /mnt/c/... -- WSL does NOT reliably do this for arguments passed to a
// Linux binary itself (confirmed live: backslashes were silently
// dropped rather than translated). Relative filenames (the common case,
// since callers set `cwd` to the right directory and pass bare
// filenames) pass through untouched either way.
function runToolCommand(configuredCommand, args, cwd) {
  const parts = configuredCommand.trim().split(/\s+/);
  const isWsl = parts[0] === 'wsl' || parts[0] === 'wsl.exe';
  if (!isWsl) {
    return execFileP(parts[0], [...parts.slice(1), ...args], cwd);
  }
  const translatedArgs = args.map((a) => (/^[A-Za-z]:[\\/]/.test(a) ? toWslPath(a) : a));
  return execFileP('wsl.exe', ['--cd', toWslPath(cwd), ...parts.slice(1), ...translatedArgs], cwd);
}

// A plain command (no wsl/args-splitting convention) -- used by
// lh5801.disasmCommand, which is this repo's own build output and never
// a WSL path.
function execPlainCommand(command, args, cwd) {
  return execFileP(command, args, cwd);
}

function toWslPath(winPath) {
  const resolved = path.resolve(winPath);
  const m = /^([A-Za-z]):[\\/](.*)$/.exec(resolved);
  if (!m) return resolved.replace(/\\/g, '/');
  return `/mnt/${m[1].toLowerCase()}/${m[2].replace(/\\/g, '/')}`;
}

function execFileP(command, args, cwd) {
  return new Promise((resolve) => {
    execFile(command, args, { cwd }, (error, stdout, stderr) => {
      resolve({ error, stdout: stdout || '', stderr: stderr || '' });
    });
  });
}

// ---------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------

// clickedUri is set when invoked from the Explorer/editor context menu;
// falls back to the active editor's file if its extension matches
// `extRe`, then to an Open dialog (Command Palette invocation with no
// obvious target).
async function resolveInputUri(clickedUri, extRe, filters) {
  if (clickedUri) return clickedUri;
  const active = vscode.window.activeTextEditor;
  if (active && extRe.test(active.document.uri.fsPath)) {
    return active.document.uri;
  }
  const picked = await vscode.window.showOpenDialog({ canSelectMany: false, filters, openLabel: 'Select' });
  return picked && picked[0];
}

// The workspace folder that owns `uri`, falling back to the first
// workspace folder -- shared by resolveCommandSetting and
// resolvePreloadRomModules's own ${workspaceFolder} expansion below.
function workspaceFolderFor(uri) {
  return (uri && vscode.workspace.getWorkspaceFolder(uri)) || vscode.workspace.workspaceFolders?.[0];
}

function expandWorkspaceFolder(raw, uri) {
  if (!raw) return raw;
  const folder = workspaceFolderFor(uri);
  return folder ? raw.replace(/\$\{workspaceFolder\}/g, folder.uri.fsPath) : raw;
}

// lh5801.* command/path settings aren't task/launch config, so VS Code
// doesn't expand ${workspaceFolder} in them automatically the way it
// does for those -- done manually here instead, against whichever
// workspace folder actually contains the input file (falling back to
// the first one).
function resolveCommandSetting(key, uri) {
  const raw = vscode.workspace.getConfiguration('lh5801').get(key.replace(/^lh5801\./, ''), '');
  return expandWorkspaceFolder(raw, uri);
}

function workspaceCwdFor(uri) {
  return (vscode.workspace.getWorkspaceFolder(uri) || vscode.workspace.workspaceFolders?.[0])?.uri.fsPath;
}

async function complainMissingSetting(key, hint) {
  const choice = await vscode.window.showErrorMessage(`${key} isn't set -- ${hint}`, 'Open Settings');
  if (choice === 'Open Settings') {
    vscode.commands.executeCommand('workbench.action.openSettings', key);
  }
}

function withProgress(title, fn) {
  return vscode.window.withProgress({ location: vscode.ProgressLocation.Notification, title }, fn);
}

function deactivate() {}

module.exports = { activate, deactivate };
