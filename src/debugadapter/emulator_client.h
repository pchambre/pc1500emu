// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
//
// Win32 named-pipe client for a running pc1500emu's scriptable command
// interface (see main.cpp's "Scriptable command interface" section and
// README.md). Reimplements what tools/send-command.ps1 does, in C++, so
// the debug adapter's request/response loop doesn't pay PowerShell's
// process-launch latency on every single-step.
#pragma once

#include <optional>
#include <string>

namespace pc1500::dap {

class EmulatorClient {
 public:
  // Sends `command` (a single command line, no trailing newline needed)
  // over the \\.\pipe\pc1500emu.cmd named pipe to a running emulator
  // instance, then waits for its response, written to the fixed
  // %TEMP%\pc1500emu.response file, to be rewritten -- detected via the
  // file's last-write time rather than its content, since two
  // consecutive responses (e.g. two "debugstatus" calls with nothing
  // changed) can be byte-identical and would otherwise look like the
  // response never arrived. Returns std::nullopt if the pipe couldn't be
  // reached or the response didn't arrive within `timeoutMs`.
  static std::optional<std::string> sendCommand(const std::string& command, int timeoutMs = 2000);

  // For commands that never call writeResponse (currently "call" and
  // "poke" -- see main.cpp's processCommand) -- connects, writes, and
  // returns immediately rather than waiting out a full timeout for a
  // response that will never come. Returns false only on a pipe-connect
  // or write failure.
  static bool sendCommandNoWait(const std::string& command, int timeoutMs = 2000);
};

}  // namespace pc1500::dap
