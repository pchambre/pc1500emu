// history_mock.h -- BASIC's command history (2026-10-07, MCONF HISTORY),
// for the mock: RP2350/history_session.c's EXP_COMMAND_HIST_* on the
// firmware's own portable cmd_history.c, the history in memory instead of
// flash. As ssh_mock.h: command() on the mock's worker, poll() on the
// emulation thread while TERM_RUN has the window (terminal()).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pc1500 {

class HistoryMock {
 public:
  HistoryMock();
  ~HistoryMock();
  HistoryMock(const HistoryMock&) = delete;
  HistoryMock& operator=(const HistoryMock&) = delete;

  uint8_t command(uint8_t cmd, std::vector<uint8_t>& window, bool enabled);
  bool terminal() const;
  void poll(std::vector<uint8_t>& window, uint32_t nowMs);
  // The commands kept, newest first.
  std::vector<std::string> entries() const;
  void clear();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace pc1500
