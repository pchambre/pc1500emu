# Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
# Version 2.0 -- see LICENSE.
#
# Writes one command to a running pc1500emu instance's scriptable command
# pipe on Windows. See README.md's "Scriptable command interface" section
# for the list of commands and kResponsePath's location (%TEMP%\pc1500emu.response).
#
# Windows has no filesystem-visible named-pipe node, so unlike the POSIX
# FIFO this can't be reached with plain shell redirection ("echo cmd >
# path") -- this script is the equivalent for Windows.
#
# Usage: powershell -File tools\send-command.ps1 "status"
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Command
)

$pipe = New-Object System.IO.Pipes.NamedPipeClientStream(".", "pc1500emu.cmd", [System.IO.Pipes.PipeDirection]::Out)
try {
    $pipe.Connect(2000)
    $writer = New-Object System.IO.StreamWriter($pipe)
    $writer.NewLine = "`n"
    $writer.AutoFlush = $true
    $writer.WriteLine($Command)
    $writer.Dispose()
} finally {
    $pipe.Dispose()
}
