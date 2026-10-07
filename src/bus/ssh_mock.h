// ssh_mock.h -- the expansion board's SSH session (2026-10-07), for the
// mock: RP2350/ssh_session.c's EXP_COMMAND_SSH_* over a host TCP socket
// instead of lwIP, running the firmware's own portable protocol, terminal
// and key handling (ssh_client.c, ssh_term.c, ssh_keys.c) -- so a test (or
// the emulator) really logs in to a real sshd.
//
// Threading, as the firmware's two cores: command() runs on the mock's
// command worker; poll() on the emulation thread, and only while the TERM
// action has the window (terminal()), when no command can be running.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace pc1500 {

class SshMock {
 public:
  SshMock();
  ~SshMock();
  SshMock(const SshMock&) = delete;
  SshMock& operator=(const SshMock&) = delete;

  // An EXP_COMMAND_SSH_* command. `wifiUp`: the mock is on a network
  // (WFCON); `sdRoot`: where SSHKEY writes SSHKEY.PUB; `hostName`: MCONF
  // HOSTNAME, for the key's comment.
  uint8_t command(uint8_t cmd, std::vector<uint8_t>& window, bool wifiUp, const std::filesystem::path& sdRoot,
                  const std::string& hostName);
  bool terminal() const;
  // EXP_COMMAND_PING_START/ROUND (WFPING, RP2350/net_ping.c): a real echo
  // from the host (Windows' IcmpSendEcho; elsewhere no reply).
  uint8_t pingCommand(uint8_t cmd, std::vector<uint8_t>& window, bool wifiUp);
  // The terminal's turn: what arrived, the key the ROM reported, the line
  // to show. `nowMs` is the emulated clock (the keys' auto-repeat).
  void poll(std::vector<uint8_t>& window, uint32_t nowMs);

  // The dongle's key: a fresh random one unless a test gives the one the
  // host knows -- an unencrypted OpenSSH ed25519 private key file.
  bool loadDeviceKeyFile(const std::string& path);
  bool knowsHost(const std::string& host) const;
  // Everything the shell printed so far, a line at a time (the scrollback,
  // then the live line).
  std::vector<std::string> lines() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace pc1500
