// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
//
// The one command this extension contributes: run pc1500disasm on a ROM/
// BIN file and open the resulting listing. Plain CommonJS, no build step
// -- this extension is installed by copying the folder directly into
// %USERPROFILE%\.vscode\extensions\ (see README.md), not via `vsce
// package`, so anything here has to run as-is with no compile step.
'use strict';

const vscode = require('vscode');
const { execFile } = require('child_process');
const path = require('path');

// Mirrors pc1500disasm's own --mode values and default --base (see
// src/disasm/disasm_main.cpp) -- keep these two in sync if that ever
// changes.
const MODES = [
  { label: 'Base ROM', description: 'e.g. ROM1.BIN', mode: 'base', defaultBase: '0xC000' },
  { label: 'Module ROM', description: 'expansion/plug-in ROM, e.g. CE-150.ROM', mode: 'module', defaultBase: '0x8000' },
  { label: 'Standalone program', description: 'a BASIC-POKEd/CALLed ML routine (.bin)', mode: 'program', defaultBase: '' },
];

function activate(context) {
  context.subscriptions.push(
    vscode.commands.registerCommand('lh5801-asm.disassemble', (clickedUri) => disassemble(clickedUri))
  );
}

async function disassemble(clickedUri) {
  const inputUri = await resolveInputUri(clickedUri);
  if (!inputUri) return;  // cancelled, or nothing to disassemble

  const disasmCommand = resolveDisasmCommand(inputUri);
  if (!disasmCommand) {
    const choice = await vscode.window.showErrorMessage(
      "lh5801.disasmCommand isn't set -- point it at your built pc1500disasm(.exe) first.",
      'Open Settings'
    );
    if (choice === 'Open Settings') {
      vscode.commands.executeCommand('workbench.action.openSettings', 'lh5801.disasmCommand');
    }
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
  const cwd = (vscode.workspace.getWorkspaceFolder(inputUri) || vscode.workspace.workspaceFolders?.[0])?.uri.fsPath;

  await vscode.window.withProgress(
    { location: vscode.ProgressLocation.Notification, title: `Disassembling ${path.basename(inputUri.fsPath)}...` },
    () =>
      new Promise((resolve) => {
        execFile(disasmCommand, args, { cwd }, (error, _stdout, stderr) => {
          if (error) {
            vscode.window.showErrorMessage(`pc1500disasm failed: ${(stderr || error.message).trim()}`);
            resolve();
            return;
          }
          if (stderr && stderr.trim()) {
            // Non-fatal warnings (e.g. low-confidence keyword-table
            // candidates -- see --seed in README.md) -- the output was
            // still written, but this is worth surfacing, not discarding.
            const firstLine = stderr.trim().split('\n')[0];
            vscode.window.showWarningMessage(`pc1500disasm: ${firstLine}`);
          }
          vscode.workspace.openTextDocument(outUri).then((doc) => vscode.window.showTextDocument(doc));
          resolve();
        });
      })
  );
}

// clickedUri is set when invoked from the Explorer context menu; falls
// back to the active editor's file if it looks like a ROM/BIN, then to an
// Open dialog (Command Palette invocation with no obvious target).
async function resolveInputUri(clickedUri) {
  if (clickedUri) return clickedUri;
  const active = vscode.window.activeTextEditor;
  if (active && /\.(rom|bin)$/i.test(active.document.uri.fsPath)) {
    return active.document.uri;
  }
  const picked = await vscode.window.showOpenDialog({
    canSelectMany: false,
    filters: { 'ROM/Binary': ['rom', 'bin'], 'All files': ['*'] },
    openLabel: 'Disassemble',
  });
  return picked && picked[0];
}

// lh5801.disasmCommand isn't a task/launch config, so VS Code doesn't
// expand ${workspaceFolder} in it automatically the way it does for those
// -- done manually here instead, against whichever workspace folder
// actually contains the input file (falling back to the first one).
function resolveDisasmCommand(inputUri) {
  const raw = vscode.workspace.getConfiguration('lh5801').get('disasmCommand', '');
  if (!raw) return '';
  const folder = vscode.workspace.getWorkspaceFolder(inputUri) || vscode.workspace.workspaceFolders?.[0];
  return folder ? raw.replace(/\$\{workspaceFolder\}/g, folder.uri.fsPath) : raw;
}

function deactivate() {}

module.exports = { activate, deactivate };
