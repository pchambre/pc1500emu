// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
//
// pc1500debugadapter: a Debug Adapter Protocol server that lets VS Code's
// native debugging UI (breakpoints, step, call stack, variables) drive a
// running pc1500emu instance over its existing scriptable command pipe.
// See docs/plan and README.md's debugger section for the overall design;
// in short: DAP requests come in framed over stdin, get translated into
// the emulator's setbreakpoints/continue/pause/debugstep/debugstatus/
// peek/poke/call/loadbinary commands (emulator_client.h), and framed DAP
// events/responses go out over stdout.
//
// Known v1 limitations (see README): only one stack frame is ever
// reported (the LH5801 has no easy hardware call-stack walk), and a
// running target's stop is detected by polling `debugstatus` rather than
// a push notification, since the command pipe is a plain request/
// response channel with no way for the emulator to initiate a message.
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "dap_protocol.h"
#include "emulator_client.h"
#include "listing_file.h"

using namespace pc1500::dap;

namespace {

// A launched-emulator process handle -- HANDLE on Windows, pid_t on
// POSIX. kInvalidProcess is the "not launched (attached to an
// already-running instance instead)" sentinel both onLaunch/onDisconnect
// compare against explicitly, rather than relying on truthiness (which
// works for a pointer-like HANDLE but not for pid_t, where 0 is not an
// invalid value).
#if defined(_WIN32)
using ProcessHandle = HANDLE;
constexpr ProcessHandle kInvalidProcess = nullptr;
#else
using ProcessHandle = pid_t;
constexpr ProcessHandle kInvalidProcess = -1;
#endif

// ---- base64, for readMemory/writeMemory's `data` fields ----

const char kB64Chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const std::vector<uint8_t>& data) {
  std::string out;
  size_t i = 0;
  while (i + 3 <= data.size()) {
    uint32_t n = (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8) | data[i + 2];
    out += kB64Chars[(n >> 18) & 0x3F];
    out += kB64Chars[(n >> 12) & 0x3F];
    out += kB64Chars[(n >> 6) & 0x3F];
    out += kB64Chars[n & 0x3F];
    i += 3;
  }
  size_t rem = data.size() - i;
  if (rem == 1) {
    uint32_t n = static_cast<uint32_t>(data[i]) << 16;
    out += kB64Chars[(n >> 18) & 0x3F];
    out += kB64Chars[(n >> 12) & 0x3F];
    out += "==";
  } else if (rem == 2) {
    uint32_t n = (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8);
    out += kB64Chars[(n >> 18) & 0x3F];
    out += kB64Chars[(n >> 12) & 0x3F];
    out += kB64Chars[(n >> 6) & 0x3F];
    out += "=";
  }
  return out;
}

int b64Val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

std::vector<uint8_t> base64Decode(const std::string& s) {
  std::vector<uint8_t> out;
  int vals[4];
  int n = 0;
  for (char c : s) {
    int v = b64Val(c);
    if (v < 0) continue;
    vals[n++] = v;
    if (n == 4) {
      out.push_back(static_cast<uint8_t>((vals[0] << 2) | (vals[1] >> 4)));
      out.push_back(static_cast<uint8_t>((vals[1] << 4) | (vals[2] >> 2)));
      out.push_back(static_cast<uint8_t>((vals[2] << 6) | vals[3]));
      n = 0;
    }
  }
  if (n >= 2) {
    out.push_back(static_cast<uint8_t>((vals[0] << 2) | (vals[1] >> 4)));
    if (n == 3) out.push_back(static_cast<uint8_t>((vals[1] << 4) | (vals[2] >> 2)));
  }
  return out;
}

// ---- misc helpers ----

uint16_t parseHex16(std::string s) {
  if (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0) s = s.substr(2);
  return s.empty() ? 0 : static_cast<uint16_t>(std::stoul(s, nullptr, 16));
}

std::string toHex4(uint16_t v) {
  std::ostringstream out;
  out << std::hex << std::uppercase;
  out.width(4);
  out.fill('0');
  out << v;
  return out.str();
}

// One CPU register/flag snapshot, as reported by the emulator's
// `debugstatus`/`debugstep`/`status` commands (see main.cpp's
// formatRegisters lambda -- these three commands all share that exact
// text layout for the register block, differing only in whether a
// leading "STOPPED reason=.../RUNNING" line precedes it).
struct RegisterSnapshot {
  std::string p, a, x, y, u, s;
  std::string halted, bf, disp, pu, pv, ie, timerCounter;
  std::string ind1, ind2;
  bool stopped = false;
  std::string reason;
};

void parseKV(const std::string& tok, const std::string& key, std::string* out) {
  if (tok.rfind(key + "=", 0) == 0) *out = tok.substr(key.size() + 1);
}

// Parses the 3-line register block common to debugstatus/debugstep/status
// (whatever remains of `in` after any leading STOPPED/RUNNING line has
// already been consumed by the caller).
RegisterSnapshot parseRegisterBlock(std::istream& in) {
  RegisterSnapshot r;
  std::string line;

  std::getline(in, line);
  {
    std::istringstream ls(line);
    std::string tok;
    while (ls >> tok) {
      parseKV(tok, "P", &r.p);
      parseKV(tok, "A", &r.a);
      parseKV(tok, "X", &r.x);
      parseKV(tok, "Y", &r.y);
      parseKV(tok, "U", &r.u);
      parseKV(tok, "S", &r.s);
    }
  }
  std::getline(in, line);
  {
    std::istringstream ls(line);
    std::string tok;
    while (ls >> tok) {
      parseKV(tok, "halted", &r.halted);
      parseKV(tok, "bf", &r.bf);
      parseKV(tok, "disp", &r.disp);
      parseKV(tok, "pu", &r.pu);
      parseKV(tok, "pv", &r.pv);
      parseKV(tok, "ie", &r.ie);
      parseKV(tok, "timerCounter", &r.timerCounter);
    }
  }
  std::getline(in, line);
  {
    std::istringstream ls(line);
    std::string tok;
    while (ls >> tok) {
      if (!tok.empty() && tok[0] == '[') break;
      parseKV(tok, "ind1(764E)", &r.ind1);
      parseKV(tok, "ind2(764F)", &r.ind2);
    }
  }
  return r;
}

// debugstatus's response: "STOPPED reason=<r>" or "RUNNING reason=",
// then the 3-line register block.
RegisterSnapshot parseDebugStatusResponse(const std::string& text) {
  std::istringstream in(text);
  std::string line;
  std::getline(in, line);
  RegisterSnapshot r = parseRegisterBlock(in);
  r.stopped = line.rfind("STOPPED", 0) == 0;
  auto pos = line.find("reason=");
  if (pos != std::string::npos) {
    r.reason = line.substr(pos + 7);
    // The response file is written by a Windows text-mode ofstream (see
    // main.cpp's writeResponse), so each "\n" the emulator wrote became
    // "\r\n" on disk -- strip the trailing \r getline() leaves behind, or
    // it ends up embedded in this reason string (and from there, in DAP
    // event bodies clients display verbatim).
    while (!r.reason.empty() && (r.reason.back() == '\r' || r.reason.back() == '\n')) r.reason.pop_back();
  }
  return r;
}

// debugstep/status's response: just the 3-line register block, no
// STOPPED/RUNNING prefix line.
RegisterSnapshot parseRegistersOnly(const std::string& text) {
  std::istringstream in(text);
  return parseRegisterBlock(in);
}

struct StopInfo {
  std::string dapReason;    // one of DAP's enumerated `stopped` reasons
  std::string description;  // human-readable detail (e.g. the exact address)
};

StopInfo mapReason(const std::string& raw) {
  if (raw.rfind("breakpoint", 0) == 0) return {"breakpoint", raw};
  if (raw == "paused") return {"pause", raw};
  if (raw == "entry") return {"entry", raw};
  if (raw == "step") return {"step", raw};
  return {"step", raw};
}

constexpr int kThreadId = 1;
constexpr int kRegistersVarRef = 1;

// ---- DAP transport plumbing ----

std::mutex g_outMutex;
int g_seq = 1;

void send(Json msg) {
  std::lock_guard<std::mutex> lock(g_outMutex);
  msg["seq"] = g_seq++;
  writeMessage(std::cout, msg);
}

void sendResponse(const Json& request, bool success, const Json& body = nullptr,
                   const std::string& message = "") {
  Json resp;
  resp["type"] = "response";
  resp["request_seq"] = request.value("seq", 0);
  resp["command"] = request.value("command", "");
  resp["success"] = success;
  if (!body.is_null()) resp["body"] = body;
  if (!message.empty()) resp["message"] = message;
  send(resp);
}

void sendEvent(const std::string& event, const Json& body = nullptr) {
  Json e;
  e["type"] = "event";
  e["event"] = event;
  if (!body.is_null()) e["body"] = body;
  send(e);
}

// ---- the adapter itself ----

class Adapter {
 public:
  int run();

 private:
  void handleRequest(const Json& req);

  void onInitialize(const Json& req);
  void onLaunch(const Json& req);
  void onSetBreakpoints(const Json& req);
  void onConfigurationDone(const Json& req);
  void onContinue(const Json& req);
  void onStep(const Json& req);
  void onPause(const Json& req);
  void onStackTrace(const Json& req);
  void onScopes(const Json& req);
  void onVariables(const Json& req);
  void onThreads(const Json& req);
  void onReadMemory(const Json& req);
  void onWriteMemory(const Json& req);
  void onDisconnect(const Json& req);

  std::optional<std::string> sendCmd(const std::string& cmd);
  bool sendCmdNoWait(const std::string& cmd);
  std::optional<RegisterSnapshot> fetchStatus();

  void startPolling();
  void stopPolling();
  void pollLoop();
  void reportStopped(const StopInfo& info);

  std::mutex pipeMutex_;

  ListingFile listing_;
  bool hasListing_ = false;
  std::string sourcePath_;
  bool stopOnEntry_ = true;

  ProcessHandle emulatorProcess_ = kInvalidProcess;

  std::atomic<bool> polling_{false};
  std::thread pollThread_;

  bool shouldExit_ = false;
};

std::optional<std::string> Adapter::sendCmd(const std::string& cmd) {
  std::lock_guard<std::mutex> lock(pipeMutex_);
  return EmulatorClient::sendCommand(cmd);
}

bool Adapter::sendCmdNoWait(const std::string& cmd) {
  std::lock_guard<std::mutex> lock(pipeMutex_);
  return EmulatorClient::sendCommandNoWait(cmd);
}

std::optional<RegisterSnapshot> Adapter::fetchStatus() {
  auto resp = sendCmd("debugstatus");
  if (!resp) return std::nullopt;
  return parseDebugStatusResponse(*resp);
}

void Adapter::startPolling() {
  stopPolling();
  polling_ = true;
  pollThread_ = std::thread(&Adapter::pollLoop, this);
}

void Adapter::stopPolling() {
  polling_ = false;
  if (pollThread_.joinable()) pollThread_.join();
}

void Adapter::pollLoop() {
  while (polling_) {
    std::this_thread::sleep_for(std::chrono::milliseconds(75));
    if (!polling_) break;
    auto snap = fetchStatus();
    if (snap && snap->stopped) {
      polling_ = false;
      reportStopped(mapReason(snap->reason));
      break;
    }
  }
}

void Adapter::reportStopped(const StopInfo& info) {
  Json body;
  body["reason"] = info.dapReason;
  body["description"] = info.description;
  body["threadId"] = kThreadId;
  body["allThreadsStopped"] = true;
  sendEvent("stopped", body);
}

void Adapter::onInitialize(const Json& req) {
  Json caps;
  caps["supportsConfigurationDoneRequest"] = true;
  sendResponse(req, true, caps);
  sendEvent("initialized");
}

void Adapter::onLaunch(const Json& req) {
  Json args = req.value("arguments", Json::object());

  if (args.contains("emulatorPath")) {
    std::string emulatorPath = args.value("emulatorPath", "");

    // emulatorArgs (a JSON array of strings) is what the lh5801-asm
    // extension's own "LH5801: Debug in pc1500emu" command populates --
    // e.g. ["--conf", "<generated conf path>", "--no-state"] when
    // extension-RAM settings are in play, or just [romPath] otherwise.
    // A hand-written launch.json config (see .vscode/launch.json's own
    // templates) won't have it, so this falls back to the plain
    // [romPath] positional arg used before emulatorArgs existed.
    std::vector<std::string> emulatorArgs;
    if (args.contains("emulatorArgs") && args["emulatorArgs"].is_array()) {
      for (const auto& a : args["emulatorArgs"]) emulatorArgs.push_back(a.get<std::string>());
    } else {
      emulatorArgs.push_back(args.value("romPath", ""));
    }

#if defined(_WIN32)
    std::string cmdLine = "\"" + emulatorPath + "\"";
    for (const auto& a : emulatorArgs) cmdLine += " \"" + a + "\"";
    std::vector<char> cmdLineBuf(cmdLine.begin(), cmdLine.end());
    cmdLineBuf.push_back('\0');

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessA(nullptr, cmdLineBuf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
    if (!ok) {
      sendResponse(req, false, nullptr, "failed to launch emulator: " + emulatorPath);
      return;
    }
    CloseHandle(pi.hThread);
    emulatorProcess_ = pi.hProcess;
#else
    std::vector<char> exeBuf(emulatorPath.begin(), emulatorPath.end());
    exeBuf.push_back('\0');
    std::vector<std::vector<char>> argBufs;
    for (const auto& a : emulatorArgs) {
      std::vector<char> buf(a.begin(), a.end());
      buf.push_back('\0');
      argBufs.push_back(std::move(buf));
    }
    std::vector<char*> argv;
    argv.push_back(exeBuf.data());
    for (auto& b : argBufs) argv.push_back(b.data());
    argv.push_back(nullptr);

    pid_t pid = kInvalidProcess;
    int rc = posix_spawn(&pid, exeBuf.data(), nullptr, nullptr, argv.data(), environ);
    if (rc != 0) {
      sendResponse(req, false, nullptr, "failed to launch emulator: " + emulatorPath);
      return;
    }
    emulatorProcess_ = pid;
#endif

    // Wait for the emulator's command pipe to come up (it's created once
    // SDL/window init finishes -- see cmdPipeInit's call site in
    // main.cpp) before sending it anything.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool ready = false;
    while (std::chrono::steady_clock::now() < deadline) {
      if (sendCmd("debugstatus").has_value()) {
        ready = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!ready) {
      sendResponse(req, false, nullptr, "emulator started but its command pipe never came up");
      return;
    }

    // The command pipe comes up (SDL/window init) well before a
    // cold-booting ROM's own init sequence actually completes --
    // confirmed live: loading/calling into the machine before it
    // settles corrupts whatever boot code was still mid-execution (P
    // visibly wanders, RAM variables stay at their 0xFF power-up
    // default). Wait for two consecutive debugstatus polls to report
    // the same P before proceeding -- a ROM-address-agnostic stability
    // check, not a fixed delay, since a state-restored boot has no such
    // window (P is already stable the instant the pipe answers) and
    // costs just one extra poll here.
    auto extractP = [](const std::string& resp) -> std::optional<std::string> {
      auto pos = resp.find("P=");
      if (pos == std::string::npos) return std::nullopt;
      pos += 2;
      auto end = resp.find(' ', pos);
      if (end == std::string::npos) end = resp.size();
      return resp.substr(pos, end - pos);
    };
    auto stableDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::optional<std::string> lastP;
    bool stable = false;
    while (std::chrono::steady_clock::now() < stableDeadline) {
      auto resp = sendCmd("debugstatus");
      if (!resp) break;
      auto p = extractP(*resp);
      if (p && lastP && *p == *lastP) {
        stable = true;
        break;
      }
      lastP = p;
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (!stable) {
      sendResponse(req, false, nullptr, "emulator's command pipe came up but its boot never settled");
      return;
    }

    // Preload any auxiliary ROM modules (e.g. CE-150/158) before the
    // main program itself loads -- only meaningful right after a fresh
    // spawn like this one, never when attaching to an already-running
    // instance (args.contains("emulatorPath") is exactly that
    // distinction). See lh5801.preloadRomModules in the extension's own
    // package.json for the field shapes this expects.
    if (args.contains("preloadRomModules") && args["preloadRomModules"].is_array()) {
      for (const auto& mod : args["preloadRomModules"]) {
        int slot = mod.value("slot", 0);
        std::string base = mod.value("base", "");
        bool requirePv = mod.value("requirePv", false);
        bool usePuBank = mod.value("usePuBank", false);
        std::string modPath = mod.value("path", "");
        std::string cmdName = slot <= 0 ? "loadrommodule" : "loadrommodule" + std::to_string(slot + 1);
        std::ostringstream cmd;
        cmd << cmdName << " " << base << " " << (requirePv ? 1 : 0) << " " << (usePuBank ? 1 : 0) << " " << modPath;
        auto resp = sendCmd(cmd.str());
        if (!resp || resp->rfind("OK", 0) != 0) {
          sendResponse(req, false, nullptr,
                       "failed to preload ROM module (slot " + std::to_string(slot + 1) +
                           "): " + (resp ? *resp : std::string("no response")));
          return;
        }
      }
    }
  }

  if (args.contains("listing")) {
    std::string listingPath = args.value("listing", "");
    std::string error;
    hasListing_ = listing_.load(listingPath, &error);
    if (!hasListing_) {
      sendResponse(req, false, nullptr, "failed to parse listing file: " + error);
      return;
    }
    sourcePath_ = args.value("sourcePath", listingPath);
  }

  stopOnEntry_ = args.value("stopOnEntry", true);

  std::string program = args.value("program", "");
  std::string loadMode = args.value("loadMode", "binary");
  uint16_t loadAddress = parseHex16(args.value("loadAddress", "0000"));
  std::string loadResp;

  if (loadMode == "rommodule") {
    bool requirePv = args.value("requirePv", false);
    bool usePuBank = args.value("usePuBank", false);
    std::ostringstream cmd;
    cmd << "loadrommodule " << toHex4(loadAddress) << " " << (requirePv ? 1 : 0) << " " << (usePuBank ? 1 : 0)
        << " " << program;
    auto resp = sendCmd(cmd.str());
    if (!resp) {
      sendResponse(req, false, nullptr, "no response from emulator for loadrommodule");
      return;
    }
    loadResp = *resp;
  } else {
    std::ostringstream cmd;
    cmd << "loadbinary " << toHex4(loadAddress) << " " << program;
    auto resp = sendCmd(cmd.str());
    if (!resp) {
      sendResponse(req, false, nullptr, "no response from emulator for loadbinary");
      return;
    }
    loadResp = *resp;
  }
  if (loadResp.rfind("OK", 0) != 0) {
    sendResponse(req, false, nullptr, "emulator rejected program load: " + loadResp);
    return;
  }

  uint16_t entry = args.contains("entry") ? parseHex16(args.value("entry", "0000")) : loadAddress;
  sendCmdNoWait("call " + toHex4(entry));
  // Always pause first -- this both parks the CPU at the entry point
  // under debug control (rather than letting the normal free-running
  // frame loop take off immediately) and gives configurationDone a known
  // stopped state to report as "entry" if stopOnEntry was requested.
  sendCmd("pause");

  sendResponse(req, true);
}

void Adapter::onSetBreakpoints(const Json& req) {
  Json args = req.value("arguments", Json::object());
  std::vector<int> lines;
  if (args.contains("breakpoints")) {
    for (const auto& bp : args["breakpoints"]) lines.push_back(bp.value("line", 0));
  }

  std::vector<uint16_t> addresses;
  Json verified = Json::array();
  for (int line : lines) {
    auto addr = hasListing_ ? listing_.addressForLine(line) : std::nullopt;
    Json bpResult;
    bpResult["line"] = line;
    bpResult["verified"] = addr.has_value();
    if (addr) {
      addresses.push_back(*addr);
      bpResult["instructionReference"] = "0x" + toHex4(*addr);
    }
    verified.push_back(bpResult);
  }

  std::ostringstream cmd;
  cmd << "setbreakpoints";
  for (uint16_t a : addresses) cmd << " " << toHex4(a);
  sendCmd(cmd.str());

  Json body;
  body["breakpoints"] = verified;
  sendResponse(req, true, body);
}

void Adapter::onConfigurationDone(const Json& req) {
  sendResponse(req, true);
  if (stopOnEntry_) {
    reportStopped({"entry", "entry"});
  } else {
    sendCmd("continue");
    startPolling();
  }
}

void Adapter::onContinue(const Json& req) {
  sendCmd("continue");
  startPolling();
  Json body;
  body["allThreadsContinued"] = true;
  sendResponse(req, true, body);
}

void Adapter::onStep(const Json& req) {
  // LH5801 stepping doesn't distinguish step-over/step-into/step-out --
  // there's no easy hardware call-stack walk (see the file header
  // comment), so "next"/"stepIn"/"stepOut" all just single-step one
  // instruction.
  stopPolling();
  sendCmd("debugstep");
  sendResponse(req, true);
  reportStopped({"step", "step"});
}

void Adapter::onPause(const Json& req) {
  sendCmd("pause");
  stopPolling();
  sendResponse(req, true);
  auto snap = fetchStatus();
  reportStopped(mapReason(snap ? snap->reason : "paused"));
}

void Adapter::onStackTrace(const Json& req) {
  auto snap = fetchStatus();
  uint16_t pc = snap ? parseHex16(snap->p) : 0;

  Json frame;
  frame["id"] = 1;
  frame["name"] = "PC";
  frame["instructionPointerReference"] = "0x" + toHex4(pc);
  if (hasListing_) {
    auto line = listing_.lineForAddress(pc);
    if (line) {
      Json source;
      source["path"] = sourcePath_;
      frame["source"] = source;
      frame["line"] = *line;
      frame["column"] = 1;
    } else {
      frame["line"] = 0;
      frame["column"] = 1;
    }
  } else {
    frame["line"] = 0;
    frame["column"] = 1;
  }

  Json body;
  body["stackFrames"] = Json::array({frame});
  body["totalFrames"] = 1;
  sendResponse(req, true, body);
}

void Adapter::onScopes(const Json& req) {
  Json scope;
  scope["name"] = "Registers";
  scope["variablesReference"] = kRegistersVarRef;
  scope["expensive"] = false;
  Json body;
  body["scopes"] = Json::array({scope});
  sendResponse(req, true, body);
}

void Adapter::onVariables(const Json& req) {
  Json args = req.value("arguments", Json::object());
  int ref = args.value("variablesReference", 0);
  Json vars = Json::array();
  if (ref == kRegistersVarRef) {
    auto snap = fetchStatus();
    if (snap) {
      auto add = [&](const std::string& name, const std::string& value) {
        Json v;
        v["name"] = name;
        v["value"] = value;
        v["variablesReference"] = 0;
        vars.push_back(v);
      };
      add("P", snap->p);
      add("A", snap->a);
      add("X", snap->x);
      add("Y", snap->y);
      add("U", snap->u);
      add("S", snap->s);
      add("halted", snap->halted);
      add("bf", snap->bf);
      add("disp", snap->disp);
      add("pu", snap->pu);
      add("pv", snap->pv);
      add("ie", snap->ie);
      add("timerCounter", snap->timerCounter);
      add("ind1(764E)", snap->ind1);
      add("ind2(764F)", snap->ind2);
    }
  }
  Json body;
  body["variables"] = vars;
  sendResponse(req, true, body);
}

void Adapter::onThreads(const Json& req) {
  Json t;
  t["id"] = kThreadId;
  t["name"] = "cpu";
  Json body;
  body["threads"] = Json::array({t});
  sendResponse(req, true, body);
}

void Adapter::onReadMemory(const Json& req) {
  Json args = req.value("arguments", Json::object());
  uint16_t base = parseHex16(args.value("memoryReference", "0"));
  int offset = args.value("offset", 0);
  int count = args.value("count", 0);

  std::vector<uint8_t> bytes;
  bytes.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; i++) {
    uint16_t addr = static_cast<uint16_t>(base + offset + i);
    auto resp = sendCmd("peek " + toHex4(addr));
    bytes.push_back(resp ? static_cast<uint8_t>(std::stoi(*resp)) : 0);
  }

  Json body;
  body["address"] = "0x" + toHex4(static_cast<uint16_t>(base + offset));
  body["data"] = base64Encode(bytes);
  sendResponse(req, true, body);
}

void Adapter::onWriteMemory(const Json& req) {
  Json args = req.value("arguments", Json::object());
  uint16_t base = parseHex16(args.value("memoryReference", "0"));
  int offset = args.value("offset", 0);
  std::vector<uint8_t> bytes = base64Decode(args.value("data", ""));

  for (size_t i = 0; i < bytes.size(); i++) {
    uint16_t addr = static_cast<uint16_t>(base + offset + static_cast<int>(i));
    std::ostringstream cmd;
    cmd << "poke " << toHex4(addr) << " " << static_cast<int>(bytes[i]);
    sendCmdNoWait(cmd.str());
  }

  Json body;
  body["bytesWritten"] = static_cast<int>(bytes.size());
  sendResponse(req, true, body);
}

void Adapter::onDisconnect(const Json& req) {
  stopPolling();
  if (emulatorProcess_ != kInvalidProcess) {
    bool terminateDebuggee = req.value("arguments", Json::object()).value("terminateDebuggee", true);
#if defined(_WIN32)
    if (terminateDebuggee) TerminateProcess(emulatorProcess_, 0);
    CloseHandle(emulatorProcess_);
#else
    if (terminateDebuggee) kill(emulatorProcess_, SIGTERM);
    int status = 0;
    waitpid(emulatorProcess_, &status, 0);  // reap regardless, or it stays a zombie
#endif
    emulatorProcess_ = kInvalidProcess;
  }
  sendResponse(req, true);
  shouldExit_ = true;
}

void Adapter::handleRequest(const Json& req) {
  std::string command = req.value("command", "");
  if (command == "initialize") {
    onInitialize(req);
  } else if (command == "launch") {
    onLaunch(req);
  } else if (command == "setBreakpoints") {
    onSetBreakpoints(req);
  } else if (command == "configurationDone") {
    onConfigurationDone(req);
  } else if (command == "continue") {
    onContinue(req);
  } else if (command == "next" || command == "stepIn" || command == "stepOut") {
    onStep(req);
  } else if (command == "pause") {
    onPause(req);
  } else if (command == "stackTrace") {
    onStackTrace(req);
  } else if (command == "scopes") {
    onScopes(req);
  } else if (command == "variables") {
    onVariables(req);
  } else if (command == "threads") {
    onThreads(req);
  } else if (command == "readMemory") {
    onReadMemory(req);
  } else if (command == "writeMemory") {
    onWriteMemory(req);
  } else if (command == "disconnect" || command == "terminate") {
    onDisconnect(req);
  } else {
    sendResponse(req, false, nullptr, "unsupported request: " + command);
  }
}

int Adapter::run() {
  std::optional<Json> msg;
  while (!shouldExit_ && (msg = readMessage(std::cin)).has_value()) {
    if (msg->value("type", "") == "request") handleRequest(*msg);
  }
  stopPolling();
  return 0;
}

}  // namespace

int main() {
#if defined(_WIN32)
  // stdin/stdout default to text mode on Windows, which would silently
  // translate "\n" <-> "\r\n" inside DAP's Content-Length-framed bodies --
  // POSIX has no such distinction, so nothing to do there.
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif

  Adapter adapter;
  return adapter.run();
}
