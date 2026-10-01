// See ble_link_core.h. The connector side follows the firmware's
// RP2350/ble_link.c, the advertiser side the laptop app's
// ble_app/lib/link.dart; both follow RP2350/BLE_PROTOCOL.md.
#include "ble_link_core.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iterator>

#ifdef PC1500_HAVE_EXPANSION_KEYWORDS
#include "plotter.h"  // PLOT's frame splitting, shared with the firmware
#endif

namespace pc1500::ble {

namespace {

// BLE_PROTOCOL.md sec.5
constexpr uint8_t kHello = 0x01, kBye = 0x02, kText = 0x10;
constexpr uint8_t kFilePut = 0x20, kFileData = 0x21, kFileEnd = 0x22, kFileGet = 0x23, kFileAbort = 0x24;
constexpr uint8_t kFileOffer = 0x25, kFileAnswer = 0x26;  // peer-to-peer (2026-09-28)
constexpr uint8_t kMsg = 0x30;                             // peer messaging (2026-09-29)
constexpr size_t kMsgMax = 240, kInboxMax = 8;             // EXP_BLE_MSG_MAX, EXP_BLE_MSG_INBOX
constexpr uint8_t kAck = 0x7E, kErr = 0x7F;
constexpr uint8_t kErrBadFrame = 1, kErrUnsupported = 2, kErrNotFound = 3, kErrExists = 4, kErrIo = 5,
                  kErrBusy = 6, kErrAborted = 7;
constexpr uint8_t kVersion = 1, kKindPc1500 = 1, kTargetServer = 0, kKindUnknown = 0xFF;
constexpr uint32_t kSizeUnknown = 0xFFFFFFFFu;

// RP2350/pc_exp.h
constexpr uint8_t kStatusSuccess = 2, kStatusError = 128, kStatusNotImplemented = 64;
constexpr uint8_t kCmdScan = 0x40, kCmdConnect = 0x41, kCmdConnectName = 0x42, kCmdDisconnect = 0x43,
                  kCmdText = 0x44, kCmdFilePut = 0x45, kCmdFileGet = 0x46;
constexpr uint8_t kCmdAdvertise = 0x47, kCmdStatus = 0x48, kCmdOffer = 0x49, kCmdWithdraw = 0x4A,
                  kCmdOfferGet = 0x4B, kCmdAnswer = 0x4C, kCmdSend = 0x4D, kCmdDataWrite = 0x4E,
                  kCmdDataRead = 0x4F, kCmdDataClose = 0x50;
constexpr uint8_t kCmdMsgSend = 0x51, kCmdMsgWait = 0x52, kCmdMsgRecv = 0x53, kCmdMsgCount = 0x54;
constexpr uint8_t kCmdPlot = 0x57;  // the CE-150 stand-in's drawing (2026-09-30)
constexpr uint8_t kPlot = 0x40;
// EXP_BLE_STATUS_*
constexpr uint8_t kStatusLinked = 0x01, kStatusAdvertising = 0x02, kStatusOfferIn = 0x04, kStatusAnswered = 0x08,
                  kStatusAccepted = 0x10;
constexpr size_t kFileArgs = 42;      // EXP_BLE_FILE_ARGS: after the name slot
constexpr size_t kLengthPort = 0x7FD; // EXP_LENGTH_PORT_PAGE/ADDRESS
constexpr size_t kPathMax = 40;       // EXP_PATH_ARG_LEN
constexpr size_t kMaxTransfer = 1024; // EXP_MAX_TRANSFER_LEN
// LIST_SD_DIR's listing (for BROWSE)
constexpr size_t kNameLen = 16, kSizeTextLen = 10, kRecordSize = 30, kSummaryLen = 26, kMaxListed = 8;

constexpr int kNoLink = -1, kTimeout = -2;

std::string nameFromSlot(const std::vector<uint8_t>& w) {
  size_t len = (static_cast<size_t>(w[0]) << 8) | w[1];
  return std::string(w.begin() + 2, w.begin() + 2 + static_cast<long>(std::min(len, kPathMax)));
}

std::vector<uint8_t> str8(const std::string& s) {
  std::vector<uint8_t> out{static_cast<uint8_t>(std::min<size_t>(s.size(), 255))};
  out.insert(out.end(), s.begin(), s.begin() + out[0]);
  return out;
}

void putU32(std::vector<uint8_t>& v, uint32_t x) {
  for (int i = 0; i < 4; i++) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}

uint32_t getU32(const uint8_t* p) {
  return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24;
}

bool sameName(const std::string& a, const std::string& b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::toupper(static_cast<unsigned char>(x)) == std::toupper(static_cast<unsigned char>(y));
         });
}

// A plain file name for the store: no folders, nothing a filesystem rejects.
bool plainName(const std::string& n) {
  if (n.empty() || n == "." || n == "..") return false;
  for (unsigned char c : n)
    if (c < 0x20 || std::string("\\/:*?\"<>|").find(static_cast<char>(c)) != std::string::npos) return false;
  return true;
}

// A MSG's payload: whole value chunks ('N' + 8 bytes, 'S' + length +
// characters), at least one.
bool validChunks(const std::vector<uint8_t>& p) {
  size_t i = 0;
  if (p.empty()) return false;
  while (i < p.size()) {
    if (p[i] == 'N') i += 9;
    else if (p[i] == 'S' && i + 1 < p.size()) i += 2 + p[i + 1];
    else return false;
  }
  return i == p.size();
}

void writeText(std::vector<uint8_t>& w, size_t at, const std::string& text, size_t len) {
  for (size_t i = 0; i < len; i++) w[at + i] = i < text.size() ? static_cast<uint8_t>(text[i]) : ' ';
}

}  // namespace

LinkCore::LinkCore(std::unique_ptr<Transport> transport, std::filesystem::path filesDir, std::string name)
    : transport_(std::move(transport)), filesDir_(std::move(filesDir)), name_(std::move(name)) {
  transport_->onFrame = [this](const std::vector<uint8_t>& f) { onFrame(f); };
  transport_->onLink = [this](bool connected, bool asAdvertiser) { onLink(connected, asAdvertiser); };
  transport_->onLog = [this](const std::string& line) { log(line); };
  serverThread_ = std::thread([this] { serverLoop(); });
}

LinkCore::~LinkCore() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  cv_.notify_all();
  serverThread_.join();
  transport_->advertise(false, name_);
  transport_->disconnect();
}

// ---- frames in ----

void LinkCore::onFrame(const std::vector<uint8_t>& f) {
  if (f.size() < 4 || f.size() - 4 != static_cast<size_t>(f[2] | f[3] << 8)) {
    log("Bad frame (" + std::to_string(f.size()) + " bytes)");
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    Frame frame{f[0], f[1], std::vector<uint8_t>(f.begin() + 4, f.end())};
    if (f[0] == kAck || f[0] == kErr) {
      answerType_ = f[0];
      answerSeq_ = f[1];
      answerCode_ = f.size() > 4 ? f[4] : 0;
      answerReady_ = true;
    } else if (f[0] == kFileOffer || f[0] == kFileAnswer || f[0] == kMsg ||
               (f[0] == kFileAbort && xfer_ == Xfer::kNone)) {
      toServe_.push_back(std::move(frame));  // may come while no command runs, in either role
    } else if (xfer_ != Xfer::kNone || !asServer_) {
      incoming_.push_back(std::move(frame));  // a transfer's, or the connector's command's
    } else {
      toServe_.push_back(std::move(frame));  // a peer using this emulator as its server
    }
  }
  cv_.notify_all();
}

void LinkCore::onLink(bool connected, bool asAdvertiser) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    linked_ = connected;
    asServer_ = connected && asAdvertiser;
    txSeq_ = 0;
    answerReady_ = false;
    incoming_.clear();
    toServe_.clear();
    helloDone_ = false;
    offerIn_ = offerOut_ = answered_ = false;  // sec.5: a dropped link drops them
    if (connected) inbox_.clear();             // a new link empties the inbox; a drop doesn't
    if (!connected) {
      peerName_.clear();
      putActive_ = false;
      if (xfer_ != Xfer::kNone) xferFailed_ = true;
    }
  }
  cv_.notify_all();
  log(connected ? (asAdvertiser ? "A peer connected to us" : "Connected") : "Disconnected");
}

bool LinkCore::isLinked() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return linked_;
}

void LinkCore::log(const std::string& line) {
  std::lock_guard<std::mutex> lock(mutex_);
  log_.push_back(line);
  if (log_.size() > 200) log_.pop_front();
}

// ---- frames out ----

bool LinkCore::sendFrame(uint8_t type, uint8_t seq, const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> f{type, seq, static_cast<uint8_t>(payload.size()), static_cast<uint8_t>(payload.size() >> 8)};
  f.insert(f.end(), payload.begin(), payload.end());
  if (f.size() > transport_->frameMax()) return false;
  return transport_->send(f);
}

// A frame out and its answer back: 0 for ACK, the ERR code, kNoLink, or
// kTimeout (the link is then dropped -- sec.4).
int LinkCore::request(uint8_t type, const std::vector<uint8_t>& payload) {
  uint8_t seq;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!linked_) return kNoLink;
    seq = txSeq_++;
    answerReady_ = false;
  }
  if (!sendFrame(type, seq, payload)) return kNoLink;
  std::unique_lock<std::mutex> lock(mutex_);
  bool done = cv_.wait_for(lock, std::chrono::milliseconds(answerTimeoutMs_),
                           [&] { return (answerReady_ && answerSeq_ == seq) || !linked_; });
  if (done && answerReady_ && answerSeq_ == seq) {
    answerReady_ = false;
    return answerType_ == kAck ? 0 : (answerCode_ ? answerCode_ : kErrBadFrame);
  }
  if (!linked_) return kNoLink;
  lock.unlock();
  dropLink("No answer from " + (peerName_.empty() ? std::string("the peer") : peerName_));
  return kTimeout;
}

// The peer's next frame (connector side), or false after the timeout.
bool LinkCore::receive(Frame* out) {
  std::unique_lock<std::mutex> lock(mutex_);
  bool got = cv_.wait_for(lock, std::chrono::milliseconds(answerTimeoutMs_),
                          [&] { return !incoming_.empty() || !linked_; });
  if (!got || incoming_.empty()) {
    bool linked = linked_;
    lock.unlock();
    if (linked) dropLink("The peer went quiet");
    return false;
  }
  *out = std::move(incoming_.front());
  incoming_.pop_front();
  return true;
}

void LinkCore::setName(const std::string& name) {
  std::lock_guard<std::mutex> lock(mutex_);
  name_ = name;
}

// Our HELLO's payload (sec.5): version, kind, name.
std::vector<uint8_t> LinkCore::hello() {
  std::string name;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    name = name_;
  }
  std::vector<uint8_t> h{kVersion, kKindPc1500};
  std::vector<uint8_t> n = str8(name);
  h.insert(h.end(), n.begin(), n.end());
  return h;
}

void LinkCore::answer(uint8_t seq, uint8_t error) {
  sendFrame(error ? kErr : kAck, seq, error ? std::vector<uint8_t>{error} : std::vector<uint8_t>{});
}

void LinkCore::dropLink(const std::string& why) {
  log(why);
  transport_->disconnect();
  onLink(false, false);
}

// ---- the keywords' commands (connector) ----

uint8_t LinkCore::command(uint8_t cmd, std::vector<uint8_t>& w) {
  auto fail = [&](int r) {
    w[0] = r > 0 ? static_cast<uint8_t>(r) : 0;
    return kStatusError;
  };
  bool server;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    server = asServer_;
  }
  switch (cmd) {  // peer-to-peer, and the rest that work in either role
    case kCmdDisconnect:
      if (advertising_) setAdvertising(false);
      disconnectLink();
      return kStatusSuccess;
    case kCmdAdvertise:
      if (!w[0]) {
        if (advertising_) setAdvertising(false);
        return kStatusSuccess;
      }
      if (isLinked()) return kStatusSuccess;
      return setAdvertising(true) ? kStatusSuccess : fail(0);
    case kCmdStatus:
      return status(w);
    case kCmdOffer:
      return offer(w);
    case kCmdWithdraw: {
      bool out;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        out = offerOut_;
        offerOut_ = answered_ = false;
      }
      if (out && isLinked()) request(kFileAbort, {});
      return kStatusSuccess;
    }
    case kCmdOfferGet:
      return offerGet(w);
    case kCmdAnswer:
      return answerOffer(w);
    case kCmdSend:
      return sendAccepted(w);
    case kCmdDataWrite:
      return write(w);
    case kCmdDataRead:
      return read(w);
    case kCmdDataClose:
      if (w[0]) xferFailed_ = true;  // abandoned
      return close();
    case kCmdMsgSend: {  // BLSEND: SUCCESS once the peer stored it
      size_t len = (static_cast<size_t>(w[0]) << 8) | w[1];
      if (len == 0 || len > kMsgMax) return fail(kErrBadFrame);
      int r = request(kMsg, std::vector<uint8_t>(w.begin() + 2, w.begin() + 2 + static_cast<long>(len)));
      return r == 0 ? kStatusSuccess : fail(r);
    }
    case kCmdMsgWait: {
      uint16_t s = static_cast<uint16_t>(w[0] << 8 | w[1]);
      std::lock_guard<std::mutex> lock(mutex_);
      recvHasDeadline_ = s != 0xFFFF;
      recvDeadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(s);
      return kStatusSuccess;
    }
    case kCmdMsgRecv: {
      std::lock_guard<std::mutex> lock(mutex_);
      if (inbox_.empty()) {
        if (recvHasDeadline_ && std::chrono::steady_clock::now() >= recvDeadline_) w[0] = 1;
        else w[0] = (linked_ && helloDone_) ? 0 : 2;
        return kStatusError;
      }
      const std::vector<uint8_t>& m = inbox_.front();
      w[0] = static_cast<uint8_t>(m.size() >> 8);
      w[1] = static_cast<uint8_t>(m.size());
      std::copy(m.begin(), m.end(), w.begin() + 2);
      inbox_.pop_front();
      return kStatusSuccess;
    }
    case kCmdMsgCount: {
      std::lock_guard<std::mutex> lock(mutex_);
      w[0] = static_cast<uint8_t>(inbox_.size());
      w[1] = (linked_ && helloDone_) ? 1 : 0;
      return kStatusSuccess;
    }
    default:
      break;
  }
  if (server) {  // a peer is using this emulator as its server
    log("Busy: a peer is connected to this emulator");
    return fail(0);
  }
  if (std::string why = transport_->problem(); !why.empty()) {
    log(why);
    return fail(0);
  }
  switch (cmd) {
    case kCmdScan:
      return scan(w);
    case kCmdConnect:
      if (w[0] >= listed_.size()) return fail(0);
      return connectTo(listed_[w[0]], w);
    case kCmdConnectName: {
      std::string wanted = nameFromSlot(w);
      for (const Peer& p : transport_->scan(3000))
        if (sameName(p.name, wanted)) return connectTo(p, w);
      log("No peer named " + wanted);
      return fail(0);
    }
    case kCmdText:
      return text(w);
#ifdef PC1500_HAVE_EXPANSION_KEYWORDS
    case kCmdPlot: {  // a PLOT payload (RP2350/plotter.h) as frames, each ACKed
      uint16_t len = static_cast<uint16_t>(std::min<size_t>((static_cast<size_t>(w[0]) << 8) | w[1], 1000));
      std::vector<uint8_t> frame(transport_->frameMax() - 4);
      plot_split_t split;
      plot_split_start(&split, &w[2], len);
      while (uint16_t n = plot_split_next(&split, &w[2], len, frame.data(), static_cast<uint16_t>(frame.size()))) {
        int r = request(kPlot, std::vector<uint8_t>(frame.begin(), frame.begin() + n));
        if (r != 0) {
          log(r == kErrUnsupported ? "The peer has no plotter" : "Plot failed");
          return fail(r);
        }
      }
      return kStatusSuccess;
    }
#endif
    case kCmdFilePut:
      return filePut(w);
    case kCmdFileGet:
      return fileGet(w);
    default:
      return kStatusNotImplemented;
  }
}

// The peers found, as a LIST_SD_DIR listing: the name (or address), and the
// signal strength where a file's size would go.
uint8_t LinkCore::scan(std::vector<uint8_t>& w) {
  int seconds = w[0] ? w[0] : 3;
  if (isLinked()) disconnectLink();
  listed_ = transport_->scan(seconds * 1000);
  if (listed_.size() > kMaxListed) listed_.resize(kMaxListed);
  for (size_t i = 0; i < listed_.size(); i++) {
    size_t at = 2 + i * kRecordSize;
    const Peer& p = listed_[i];
    writeText(w, at, p.name.empty() ? p.address : p.name, kNameLen);
    writeText(w, at + kNameLen, std::to_string(p.rssi) + "DBM", kSizeTextLen);
    std::fill(w.begin() + static_cast<long>(at + kNameLen + kSizeTextLen), w.begin() + static_cast<long>(at + kRecordSize), 0);
  }
  w[0] = 0;
  w[1] = static_cast<uint8_t>(listed_.size());
  writeText(w, 2 + listed_.size() * kRecordSize, std::to_string(listed_.size()) + " FOUND", kSummaryLen);
  return kStatusSuccess;
}

// Connects and exchanges HELLOs (sec.5); the peer's name to the window.
uint8_t LinkCore::connectTo(const Peer& peer, std::vector<uint8_t>& w) {
  if (isLinked()) disconnectLink();
  if (!transport_->connect(peer)) {
    log("Couldn't connect to " + (peer.name.empty() ? peer.address : peer.name));
    w[0] = 0;
    return kStatusError;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!linked_) {  // the transport reports the link through onLink; make sure
      linked_ = true;
      txSeq_ = 0;
    }
  }
  Frame f;
  if (request(kHello, hello()) != 0 || !receive(&f)) {
    dropLink("HELLO failed");
    w[0] = 0;
    return kStatusError;
  }
  if (f.type != kHello || f.payload.size() < 3 || f.payload[0] != kVersion || 3u + f.payload[2] > f.payload.size()) {
    answer(f.seq, kErrUnsupported);
    dropLink("The peer isn't compatible");
    w[0] = 0;
    return kStatusError;
  }
  answer(f.seq);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    peerName_.assign(f.payload.begin() + 3, f.payload.begin() + 3 + f.payload[2]);
    helloDone_ = true;
  }
  log("Connected to " + peerName_);
  w[0] = static_cast<uint8_t>(peerName_.size());
  std::copy(peerName_.begin(), peerName_.end(), w.begin() + 1);
  return kStatusSuccess;
}

void LinkCore::disconnectLink() {
  bool linked, server;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    linked = linked_;
    server = asServer_;
  }
  if (linked && !server) request(kBye, {});
  if (linked) {
    transport_->disconnect();
    onLink(false, false);
  }
  xfer_ = Xfer::kNone;
}

uint8_t LinkCore::text(std::vector<uint8_t>& w) {
  size_t total = (static_cast<size_t>(w[0]) << 8) | w[1];
  size_t chunk = transport_->frameMax() - 5;
  for (size_t at = 0; at < total; at += chunk) {
    std::vector<uint8_t> p{0};  // channel 0: the console
    p.insert(p.end(), w.begin() + 2 + static_cast<long>(at), w.begin() + 2 + static_cast<long>(std::min(total, at + chunk)));
    if (request(kText, p) != 0) {
      log("Text failed");
      w[0] = 0;
      return kStatusError;
    }
  }
  return kStatusSuccess;
}

uint8_t LinkCore::filePut(std::vector<uint8_t>& w) {
  const uint8_t* a = &w[kFileArgs];
  std::vector<uint8_t> p{kTargetServer, a[0], a[1]};
  putU32(p, static_cast<uint32_t>(a[2]) << 24 | a[3] << 16 | a[4] << 8 | a[5]);
  std::vector<uint8_t> n = str8(nameFromSlot(w));
  p.insert(p.end(), n.begin(), n.end());
  int r = request(kFilePut, p);
  if (r != 0) {
    w[0] = r > 0 ? static_cast<uint8_t>(r) : 0;
    return kStatusError;
  }
  xfer_ = Xfer::kPut;
  routed_ = true;
  xferFailed_ = false;
  return kStatusSuccess;
}

uint8_t LinkCore::fileGet(std::vector<uint8_t>& w) {
  std::vector<uint8_t> p{kTargetServer};
  std::vector<uint8_t> n = str8(nameFromSlot(w));
  p.insert(p.end(), n.begin(), n.end());
  int r = request(kFileGet, p);
  Frame f;
  if (r != 0 || !receive(&f)) {
    w[0] = r > 0 ? static_cast<uint8_t>(r) : 0;
    return kStatusError;
  }
  if (f.type != kFilePut || f.payload.size() < 7) {
    answer(f.seq, kErrBadFrame);
    w[0] = kErrBadFrame;
    return kStatusError;
  }
  answer(f.seq);
  w[0] = f.payload[1];  // kind
  xfer_ = Xfer::kGet;
  routed_ = true;
  xferFailed_ = getEnded_ = false;
  getBuf_.clear();
  getPos_ = 0;
  return kStatusSuccess;
}

// WRITE_TO_SD_FILE while saving: FILE_DATA frames.
uint8_t LinkCore::write(std::vector<uint8_t>& w) {
  size_t len = static_cast<size_t>(w[kLengthPort] << 8 | w[kLengthPort + 1]);
  if (xfer_ != Xfer::kPut || xferFailed_ || len == 0) return kStatusError;
  size_t chunk = transport_->frameMax() - 4;
  for (size_t at = 0; at < len; at += chunk) {
    if (request(kFileData, std::vector<uint8_t>(w.begin() + static_cast<long>(at),
                                                w.begin() + static_cast<long>(std::min(len, at + chunk)))) != 0) {
      xferFailed_ = true;
      log("Save failed");
      return kStatusError;
    }
  }
  return kStatusSuccess;
}

// READ_FROM_SD_FILE while loading: up to the requested bytes; 0 at the end.
uint8_t LinkCore::read(std::vector<uint8_t>& w) {
  size_t want = static_cast<size_t>(w[kLengthPort] << 8 | w[kLengthPort + 1]), n = 0;
  if (xfer_ != Xfer::kGet || xferFailed_ || want > kMaxTransfer) return kStatusError;
  while (n < want) {
    if (getPos_ < getBuf_.size()) {
      size_t k = std::min(getBuf_.size() - getPos_, want - n);
      std::copy_n(getBuf_.begin() + static_cast<long>(getPos_), k, w.begin() + static_cast<long>(n));
      getPos_ += k;
      n += k;
      continue;
    }
    if (getEnded_) break;
    Frame f;
    if (!receive(&f)) {
      xferFailed_ = true;
      log("Load failed");
      return kStatusError;
    }
    if (f.type == kFileData) {
      getBuf_ = std::move(f.payload);
      getPos_ = 0;
      answer(f.seq);
    } else if (f.type == kFileEnd) {
      getEnded_ = true;
      answer(f.seq);
    } else {
      answer(f.seq, f.type == kFileAbort ? 0 : kErrBadFrame);
      xferFailed_ = true;
      log("Load aborted");
      return kStatusError;
    }
  }
  w[kLengthPort] = static_cast<uint8_t>(n >> 8);
  w[kLengthPort + 1] = static_cast<uint8_t>(n);
  return kStatusSuccess;
}

// CLOSE_SD_FILE: SUCCESS only if the whole file moved.
uint8_t LinkCore::close() {
  bool ok;
  if (xfer_ == Xfer::kPut) {
    ok = !xferFailed_ && request(kFileEnd, {}) == 0;
    if (!ok && isLinked()) request(kFileAbort, {});
  } else {
    ok = !xferFailed_ && getEnded_ && getPos_ == getBuf_.size();
  }
  xfer_ = Xfer::kNone;
  return ok ? kStatusSuccess : kStatusError;
}

// ---- peer-to-peer: the emulated PC-1500's BLADV/BLPUT/BLGET ----

uint8_t LinkCore::status(std::vector<uint8_t>& w) {
  std::lock_guard<std::mutex> lock(mutex_);
  uint8_t flags = 0;
  if (linked_ && helloDone_) flags |= kStatusLinked;
  if (advertising_ && !linked_) flags |= kStatusAdvertising;
  if (offerIn_) flags |= kStatusOfferIn;
  if (offerOut_ && answered_) flags |= kStatusAnswered | (accepted_ ? kStatusAccepted : 0);
  std::string name = peerName_.substr(0, 16);
  w[0] = flags;
  w[1] = static_cast<uint8_t>(name.size());
  std::copy(name.begin(), name.end(), w.begin() + 2);
  return kStatusSuccess;
}

uint8_t LinkCore::offer(std::vector<uint8_t>& w) {
  const uint8_t* a = &w[kFileArgs];
  std::vector<uint8_t> p{a[0]};  // kind
  putU32(p, static_cast<uint32_t>(a[2]) << 24 | a[3] << 16 | a[4] << 8 | a[5]);
  std::vector<uint8_t> n = str8(nameFromSlot(w));
  p.insert(p.end(), n.begin(), n.end());
  {
    std::lock_guard<std::mutex> lock(mutex_);
    offerOut_ = true;  // before sending: the answer may come back at once
    answered_ = accepted_ = false;
  }
  int r = request(kFileOffer, p);
  if (r == 0) return kStatusSuccess;
  std::lock_guard<std::mutex> lock(mutex_);
  offerOut_ = false;
  w[0] = r > 0 ? static_cast<uint8_t>(r) : 0;
  return kStatusError;
}

uint8_t LinkCore::offerGet(std::vector<uint8_t>& w) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!offerIn_) return kStatusError;
  w[0] = 0;
  w[1] = static_cast<uint8_t>(offerInName_.size());
  std::copy(offerInName_.begin(), offerInName_.end(), w.begin() + 2);
  uint8_t* a = &w[kFileArgs];
  a[0] = offerInKind_;
  a[1] = 0;
  for (int i = 0; i < 4; i++) a[2 + i] = static_cast<uint8_t>(offerInSize_ >> (24 - 8 * i));
  return kStatusSuccess;
}

// Answers the held offer; accepting opens the receiving transfer first (the
// sender's FILE_DATA may follow the answer at once).
uint8_t LinkCore::answerOffer(std::vector<uint8_t>& w) {
  uint8_t accept = w[0] ? 1 : 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!offerIn_) return kStatusError;
    offerIn_ = false;
    if (accept) {
      routed_ = w[1] != 0;
      xferFailed_ = getEnded_ = false;
      getBuf_.clear();
      getPos_ = 0;
      incoming_.clear();
      xfer_ = Xfer::kGet;
    }
  }
  int r = request(kFileAnswer, {accept});
  if (r == 0) return kStatusSuccess;
  xfer_ = Xfer::kNone;
  w[0] = r > 0 ? static_cast<uint8_t>(r) : 0;
  return kStatusError;
}

uint8_t LinkCore::sendAccepted(std::vector<uint8_t>& w) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!offerOut_ || !answered_ || !accepted_ || !linked_) return kStatusError;
  offerOut_ = answered_ = false;
  routed_ = w[0] != 0;
  xferFailed_ = false;
  xfer_ = Xfer::kPut;
  return kStatusSuccess;
}

// ---- advertiser: this emulator as a peer's feature server ----

bool LinkCore::setAdvertising(bool on) {
  std::string name;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    name = name_;
  }
  bool ok = transport_->advertise(on, name);
  advertising_ = on && ok;
  log(on ? (ok ? "Advertising as " + name : "Couldn't advertise: " + transport_->problem()) : "Advertising stopped");
  return ok;
}

void LinkCore::serverLoop() {
  std::unique_lock<std::mutex> lock(mutex_);
  for (;;) {
    cv_.wait(lock, [&] { return stopping_ || !toServe_.empty(); });
    if (stopping_) return;
    Frame f = std::move(toServe_.front());
    toServe_.pop_front();
    lock.unlock();
    serve(f);
    lock.lock();
  }
}

void LinkCore::serve(const Frame& f) {
  const std::vector<uint8_t>& p = f.payload;
  switch (f.type) {
    case kHello: {
      if (p.size() < 3 || p[0] != kVersion || 3u + p[2] > p.size()) return answer(f.seq, kErrUnsupported);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        peerName_.assign(p.begin() + 3, p.begin() + 3 + p[2]);
      }
      log("HELLO from " + peerName_);
      answer(f.seq);
      if (request(kHello, hello()) != 0) return log("Our HELLO wasn't answered");
      std::lock_guard<std::mutex> lock(mutex_);
      helloDone_ = true;
      return;
    }
    case kFileOffer: {  // held for the emulated PC-1500's BLGET (sec.5)
      if (p.size() < 6 || 6u + p[5] > p.size()) return answer(f.seq, kErrBadFrame);
      std::string what;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!offerIn_ && xfer_ == Xfer::kNone) {
          offerInKind_ = p[0];
          offerInSize_ = getU32(&p[1]);
          offerInName_.assign(p.begin() + 6, p.begin() + 6 + std::min<size_t>(p[5], kPathMax));
          offerIn_ = true;
          what = offerInName_.empty() ? std::string("a program") : offerInName_;
        }
      }
      if (what.empty()) return answer(f.seq, kErrBusy);
      log("Offered " + what + " by " + peerName_);
      return answer(f.seq);
    }
    case kMsg: {  // into the inbox for the emulated PC-1500's BLRECV
      if (p.size() > kMsgMax || !validChunks(p)) return answer(f.seq, kErrBadFrame);
      bool stored = false;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (inbox_.size() < kInboxMax) {
          inbox_.push_back(p);
          stored = true;
        }
      }
      return answer(f.seq, stored ? 0 : kErrBusy);
    }
    case kFileAnswer: {  // to our BLPUT's offer
      bool ok;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        ok = offerOut_ && !answered_ && !p.empty();
        if (ok) {
          accepted_ = p[0] == 1;
          answered_ = true;
        }
      }
      return answer(f.seq, ok ? 0 : kErrBadFrame);
    }
    case kBye:
      log("BYE");
      return answer(f.seq);
    case kText: {
      if (p.empty()) return answer(f.seq, kErrBadFrame);
      if (p[0] == 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 1; i < p.size(); i++) {
          char c = p[i] == 0x0D ? '\n' : static_cast<char>(p[i]);
          if (p[i] == 0x0C) {  // FF: clear the console (BLCLS)
            console_.clear();
            consoleNew_ += '\f';
            continue;
          }
          console_ += c;
          consoleNew_ += c;
        }
      }
      return answer(f.seq);
    }
    case kPlot:  // the PC-1500's CE-150 stand-in drawing here (2026-09-30)
      return answer(f.seq, paper_.addPayload(p.data(), p.size()) ? 0 : kErrBadFrame);
    case kFilePut: {
      if (p.size() < 8 || 8u + p[7] > p.size()) return answer(f.seq, kErrBadFrame);
      if (p[0] != kTargetServer) return answer(f.seq, kErrUnsupported);
      if (putActive_) return answer(f.seq, kErrBusy);
      std::string name(p.begin() + 8, p.begin() + 8 + p[7]);
      if (!plainName(name)) return answer(f.seq, kErrIo);
      bool overwrite = p[2] & 1;
      if (!overwrite && std::filesystem::exists(filesDir_ / name)) return answer(f.seq, kErrExists);
      putName_ = name;
      putSize_ = getU32(&p[3]);
      putData_.clear();
      putActive_ = true;
      log("Saving " + name);
      return answer(f.seq);
    }
    case kFileData:
      if (!putActive_) return answer(f.seq, kErrBadFrame);
      putData_.insert(putData_.end(), p.begin(), p.end());
      return answer(f.seq);
    case kFileEnd: {
      if (!putActive_) return answer(f.seq, kErrBadFrame);
      putActive_ = false;
      if (putSize_ != kSizeUnknown && putData_.size() != putSize_) {
        log(putName_ + ": got " + std::to_string(putData_.size()) + " bytes, expected " + std::to_string(putSize_));
        return answer(f.seq, kErrIo);
      }
      std::error_code ec;
      std::filesystem::create_directories(filesDir_, ec);
      std::ofstream out(filesDir_ / putName_, std::ios::binary);
      out.write(reinterpret_cast<const char*>(putData_.data()), static_cast<std::streamsize>(putData_.size()));
      out.close();  // on disk before the ACK says it's saved (a BLLOAD may follow at once)
      if (!out) return answer(f.seq, kErrIo);
      log("Saved " + putName_ + " (" + std::to_string(putData_.size()) + " bytes)");
      return answer(f.seq);
    }
    case kFileAbort:
      if (putActive_) log("Save of " + putName_ + " abandoned");
      putActive_ = false;
      {
        bool held;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          held = offerIn_;
          offerIn_ = false;  // the sender withdrew its offer
        }
        if (held) log("The offer was withdrawn");
      }
      return answer(f.seq);
    case kFileGet:
      return serveFileGet(f);
    default:
      return answer(f.seq, kErrUnsupported);
  }
}

void LinkCore::serveFileGet(const Frame& f) {
  const std::vector<uint8_t>& p = f.payload;
  if (p.size() < 2 || 2u + p[1] > p.size()) return answer(f.seq, kErrBadFrame);
  if (p[0] != kTargetServer) return answer(f.seq, kErrUnsupported);
  std::string name(p.begin() + 2, p.begin() + 2 + p[1]);
  std::ifstream in(filesDir_ / name, std::ios::binary);
  if (!plainName(name) || !in) return answer(f.seq, kErrNotFound);
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  answer(f.seq);
  log("Sending " + name + " (" + std::to_string(data.size()) + " bytes)");
  std::vector<uint8_t> header{kTargetServer, kKindUnknown, 0};
  putU32(header, static_cast<uint32_t>(data.size()));
  std::vector<uint8_t> n = str8(name);
  header.insert(header.end(), n.begin(), n.end());
  if (request(kFilePut, header) != 0) return log(name + ": refused");
  size_t chunk = transport_->frameMax() - 4;
  for (size_t at = 0; at < data.size(); at += chunk) {
    std::vector<uint8_t> part(data.begin() + static_cast<long>(at), data.begin() + static_cast<long>(std::min(data.size(), at + chunk)));
    if (int r = request(kFileData, part); r != 0) return log(name + ": stopped at byte " + std::to_string(at));
  }
  if (request(kFileEnd, {}) == 0) log("Sent " + name);
}

// ---- views ----

std::string LinkCore::state() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (linked_) {
    std::string who = peerName_.empty() ? std::string("a peer") : peerName_;
    return "connected to " + who + (asServer_ ? " (it connected to us)" : "");
  }
  return advertising_ ? "advertising" : "idle";
}

std::string LinkCore::takeConsoleText() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string t = std::move(consoleNew_);
  consoleNew_.clear();
  return t;
}

std::string LinkCore::consoleText() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return console_;
}

std::vector<std::string> LinkCore::logLines() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return std::vector<std::string>(log_.begin(), log_.end());
}

}  // namespace pc1500::ble
