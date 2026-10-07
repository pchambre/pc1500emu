// history_mock.cpp -- see history_mock.h. Mirrors RP2350/history_session.c.
#include "history_mock.h"

#include <atomic>
#include <cstring>

#ifdef PC1500_HAVE_EXPANSION_KEYWORDS
extern "C" {
#include "cmd_history.h"
#include "pc_exp.h"
}
#endif

namespace pc1500 {

#ifdef PC1500_HAVE_EXPANSION_KEYWORDS

namespace {
constexpr uint8_t kIndSmall = 0x08;      // 764EH's SMALL: BASIC's own, left as it was
constexpr size_t kTermSavedInd = 0x1D;   // the ROM's copy of 764EH (rom_defs.inc)

uint8_t lineLen(const uint8_t* w) {
  uint8_t n = 0;
  while (n < EXP_HIST_LINE_LEN && w[n] != 0x0D) n++;
  return n;
}
}  // namespace

struct HistoryMock::Impl {
  hist_t hist{};
  hist_ui_t ui{};
  std::atomic<bool> terminal{false};
  std::string original;
  uint8_t breakSeen = 0;
  uint32_t shownVersion = 0;

  Impl() { hist_init(&hist); }

  void publish(std::vector<uint8_t>& w) {
    w[EXP_SSH_TERM_INDICATORS] = static_cast<uint8_t>(hist_ui_indicators(&ui) | (w[kTermSavedInd] & kIndSmall));
    if (ui.version == shownVersion) return;
    shownVersion = ui.version;
    char line[HIST_WIDTH];
    hist_ui_render(&ui, line);
    std::memcpy(&w[EXP_SSH_TERM_LINE], line, HIST_WIDTH);
    w[EXP_SSH_TERM_LINE_COUNT]++;
  }
};

HistoryMock::HistoryMock() : impl_(std::make_unique<Impl>()) {}
HistoryMock::~HistoryMock() = default;

uint8_t HistoryMock::command(uint8_t cmd, std::vector<uint8_t>& w, bool enabled) {
  Impl& s = *impl_;
  switch (cmd) {
    case EXP_COMMAND_HIST_ADD:
      if (enabled) hist_add(&s.hist, reinterpret_cast<const char*>(w.data()), lineLen(w.data()));
      return EXP_STATUS_SUCCESS;
    case EXP_COMMAND_HIST_BEGIN: {
      hist_start_t how = w[0] == EXP_HIST_START_SEARCH  ? HIST_START_SEARCH
                         : w[0] == EXP_HIST_START_NEWER ? HIST_START_NEWER
                                                        : HIST_START_OLDER;
      if (!enabled) return EXP_STATUS_ERROR;
      s.original.assign(reinterpret_cast<const char*>(&w[1]), lineLen(&w[1]));
      if (!hist_ui_start(&s.ui, &s.hist, how)) return EXP_STATUS_ERROR;
      s.breakSeen = w[EXP_SSH_TERM_BREAK_COUNT];
      w[EXP_SSH_TERM_KEY] = s.ui.held;  // the arrow that opened it, still down
      w[EXP_SSH_TERM_CLOSED] = 0;
      s.shownVersion = s.ui.version - 1;
      s.publish(w);
      s.terminal = true;
      return EXP_STATUS_SUCCESS;
    }
    default: return EXP_STATUS_NOT_IMPLEMENTED;
  }
}

bool HistoryMock::terminal() const { return impl_->terminal; }

void HistoryMock::poll(std::vector<uint8_t>& w, uint32_t nowMs) {
  Impl& s = *impl_;
  if (!s.terminal) return;
  hist_ui_key(&s.ui, w[EXP_SSH_TERM_KEY], nowMs);
  if (w[EXP_SSH_TERM_BREAK_COUNT] != s.breakSeen) {
    s.breakSeen = w[EXP_SSH_TERM_BREAK_COUNT];
    hist_ui_break(&s.ui);
  }
  s.publish(w);
  if (hist_ui_result(&s.ui) == HIST_UI_OPEN) return;
  std::fill(w.begin() + EXP_HIST_LINE, w.begin() + EXP_HIST_LINE + EXP_HIST_LINE_LEN, 0x0D);
  uint8_t result;
  if (const hist_entry_t* e = hist_ui_selected(&s.ui)) {
    std::string line(e->text, e->len);
    w[EXP_HIST_LINE_LENGTH] = e->len;
    std::memcpy(&w[EXP_HIST_LINE], line.data(), line.size());
    result = hist_ui_result(&s.ui) == HIST_UI_RUN ? EXP_HIST_RUN : EXP_HIST_EDIT;
    if (result == EXP_HIST_RUN) hist_add(&s.hist, line.data(), line.size());
  } else {
    std::memcpy(&w[EXP_HIST_LINE], s.original.data(), s.original.size());
    w[EXP_HIST_LINE_LENGTH] = static_cast<uint8_t>(s.original.size());
    result = hist_ui_result(&s.ui) == HIST_UI_BREAK ? EXP_HIST_BREAK : EXP_HIST_CANCEL;
  }
  s.terminal = false;
  w[EXP_SSH_TERM_CLOSED] = result;
}

std::vector<std::string> HistoryMock::entries() const {
  std::vector<std::string> out;
  for (uint16_t i = 1; const hist_entry_t* e = hist_get(&impl_->hist, i); i++) out.emplace_back(e->text, e->len);
  return out;
}

void HistoryMock::clear() { hist_init(&impl_->hist); }

#else

struct HistoryMock::Impl {};
HistoryMock::HistoryMock() : impl_(std::make_unique<Impl>()) {}
HistoryMock::~HistoryMock() = default;
uint8_t HistoryMock::command(uint8_t, std::vector<uint8_t>&, bool) { return 64; }  // NOT_IMPLEMENTED
bool HistoryMock::terminal() const { return false; }
void HistoryMock::poll(std::vector<uint8_t>&, uint32_t) {}
std::vector<std::string> HistoryMock::entries() const { return {}; }
void HistoryMock::clear() {}

#endif

}  // namespace pc1500
