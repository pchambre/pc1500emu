// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#include "emulator_client.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>

namespace pc1500::dap {

namespace {

constexpr const char* kCommandPipeName = "\\\\.\\pipe\\pc1500emu.cmd";

std::string responsePath() {
  char dir[MAX_PATH]{};
  DWORD n = GetTempPathA(static_cast<DWORD>(sizeof(dir)), dir);
  std::string path = (n > 0 && n < sizeof(dir)) ? std::string(dir, n) : std::string("C:\\Windows\\Temp\\");
  if (!path.empty() && path.back() != '\\') path += '\\';
  return path + "pc1500emu.response";
}

// Returns the response file's last-write time, or a zeroed FILETIME if it
// doesn't exist yet (a fresh emulator instance that's never responded).
FILETIME lastWriteTime(const std::string& path) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) return FILETIME{};
  return data.ftLastWriteTime;
}

bool sameTime(const FILETIME& a, const FILETIME& b) {
  return a.dwLowDateTime == b.dwLowDateTime && a.dwHighDateTime == b.dwHighDateTime;
}

// Connects to the command pipe, writes `command` (newline-terminated),
// and closes -- the connect/write/close pattern common to both
// sendCommand and sendCommandNoWait. Returns false on a connect or write
// failure within `deadline`.
bool connectAndWrite(const std::string& command, std::chrono::steady_clock::time_point deadline) {
  HANDLE pipe = INVALID_HANDLE_VALUE;
  while (true) {
    pipe = CreateFileA(kCommandPipeName, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe != INVALID_HANDLE_VALUE) break;
    if (GetLastError() != ERROR_PIPE_BUSY || std::chrono::steady_clock::now() >= deadline) return false;
    WaitNamedPipeA(kCommandPipeName, 100);
  }

  std::string line = command;
  if (line.empty() || line.back() != '\n') line += '\n';
  DWORD written = 0;
  BOOL ok = WriteFile(pipe, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
  CloseHandle(pipe);
  return ok && written == line.size();
}

}  // namespace

std::optional<std::string> EmulatorClient::sendCommand(const std::string& command, int timeoutMs) {
  std::string path = responsePath();
  FILETIME before = lastWriteTime(path);

  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  if (!connectAndWrite(command, deadline)) return std::nullopt;

  while (std::chrono::steady_clock::now() < deadline) {
    FILETIME now = lastWriteTime(path);
    if (now.dwLowDateTime != 0 || now.dwHighDateTime != 0) {
      if (!sameTime(now, before)) {
        std::ifstream f(path, std::ios::binary);
        std::ostringstream buf;
        buf << f.rdbuf();
        return buf.str();
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return std::nullopt;
}

bool EmulatorClient::sendCommandNoWait(const std::string& command, int timeoutMs) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  return connectAndWrite(command, deadline);
}

}  // namespace pc1500::dap
