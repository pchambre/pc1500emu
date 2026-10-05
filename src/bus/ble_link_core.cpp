// See ble_link_core.h. The connector side follows the firmware's
// RP2350/ble_link.c, the advertiser side the laptop app's
// ble_app/lib/link.dart; both follow RP2350/BLE_PROTOCOL.md.
#include "ble_link_core.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iterator>

#include <cstdio>
#include <random>

#ifdef PC1500_HAVE_EXPANSION_KEYWORDS
#include "link_secure.h"  // the Link's security (sec.7), shared with the firmware
#include "plotter.h"      // PLOT's frame splitting, shared with the firmware
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dpapi.h>
#else
#include <sys/stat.h>
#endif

namespace pc1500::ble {

namespace {

// BLE_PROTOCOL.md sec.5
constexpr uint8_t kHello = 0x01, kBye = 0x02, kText = 0x10;
constexpr uint8_t kAuth = 0x03, kPairStart = 0x04, kPairNonce = 0x05, kPairConfirm = 0x06;  // sec.7
constexpr uint8_t kFilePut = 0x20, kFileData = 0x21, kFileEnd = 0x22, kFileGet = 0x23, kFileAbort = 0x24;
constexpr uint8_t kFileOffer = 0x25, kFileAnswer = 0x26;  // peer-to-peer (2026-09-28)
constexpr uint8_t kMsg = 0x30;                             // peer messaging (2026-09-29)
constexpr size_t kMsgMax = 220, kInboxMax = 8;             // EXP_BLE_MSG_MAX, EXP_BLE_MSG_INBOX
constexpr uint8_t kAck = 0x7E, kErr = 0x7F;
constexpr uint8_t kErrBadFrame = 1, kErrUnsupported = 2, kErrNotFound = 3, kErrExists = 4, kErrIo = 5,
                  kErrBusy = 6, kErrAborted = 7, kErrNotPaired = 8, kErrAuthFailed = 9;
constexpr uint8_t kVersion = 2, kKindPc1500 = 1, kTargetServer = 0, kKindUnknown = 0xFF;  // 2: sec.7
constexpr size_t kSealOverhead = 20;  // LS_OVERHEAD: a sealed frame's counter and tag
constexpr size_t kIdLen = 8, kNonceLen = 16, kKeyLen = 32, kProofLen = 16;
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
constexpr uint8_t kCmdPairBegin = 0x58, kCmdPairConfirm = 0x59, kCmdPairAnswer = 0x5A, kCmdUnpair = 0x5B;  // sec.7
constexpr uint8_t kStatusPairAsk = 0x20;
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

// Random bytes from the OS (std::random_device is the OS's generator on
// every platform this builds for).
void randomBytes(uint8_t* p, size_t n) {
  std::random_device rd;
  for (size_t i = 0; i < n; i++) p[i] = static_cast<uint8_t>(rd());
}

std::string hexOf(const uint8_t* p, size_t n) {
  static const char kHex[] = "0123456789ABCDEF";
  std::string s;
  for (size_t i = 0; i < n; i++) {
    s += kHex[p[i] >> 4];
    s += kHex[p[i] & 15];
  }
  return s;
}

// The pairing file's bytes, protected for this user: DPAPI on Windows; on
// other systems the file itself is made owner-only.
std::vector<uint8_t> protect(const std::vector<uint8_t>& plain, bool unprotect) {
#ifdef _WIN32
  DATA_BLOB in{static_cast<DWORD>(plain.size()), const_cast<BYTE*>(plain.data())}, out{};
  BOOL ok = unprotect ? CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)
                      : CryptProtectData(&in, L"pc1500emu Link pairings", nullptr, nullptr, nullptr, 0, &out);
  if (!ok) return {};
  std::vector<uint8_t> r(out.pbData, out.pbData + out.cbData);
  SecureZeroMemory(out.pbData, out.cbData);
  LocalFree(out.pbData);
  return r;
#else
  (void)unprotect;
  return plain;
#endif
}

}  // namespace

// sec.7's state: this side's identity and pairings, the session, and
// pairings under way in either role. Its own lock, taken briefly, never
// while waiting for the peer.
struct LinkCore::Security {
  mutable std::mutex m;
  std::filesystem::path file;
  uint8_t id[kIdLen] = {};
  struct Pair {
    uint8_t id[kIdLen];
    std::string name;
    uint8_t ltk[kKeyLen];
  };
  std::vector<Pair> pairs;
#ifdef PC1500_HAVE_EXPANSION_KEYWORDS
  ls_session_t session{};
#endif
  bool authed = false;
  uint8_t nonceC[kNonceLen] = {}, nonceS[kNonceLen] = {}, peerId[kIdLen] = {};
  bool peerKnown = false;
  uint8_t linkLtk[kKeyLen] = {};
  // the advertiser's side of a pairing (answered in serve())
  int aStep = 0, aAnswer = -1;
  uint8_t aSk[32] = {}, aPkC[32] = {}, aPkS[32] = {}, aNc[kNonceLen] = {}, aNs[kNonceLen] = {}, aLtk[kKeyLen] = {};
  uint32_t aCode = 0;
  std::string aName;
  // the connector's (BLPAIR)
  bool cActive = false;
  uint8_t cPkC[32] = {}, cPkS[32] = {}, cNc[kNonceLen] = {}, cNs[kNonceLen] = {}, cLtk[kKeyLen] = {};

  Security() { randomBytes(id, kIdLen); }

  const Pair* find(const uint8_t* peer) const {
    for (const Pair& p : pairs)
      if (std::equal(p.id, p.id + kIdLen, peer)) return &p;
    return nullptr;
  }
  void add(const uint8_t* peer, const std::string& name, const uint8_t* ltk) {
    Pair* p = const_cast<Pair*>(find(peer));
    if (!p) {
      pairs.push_back(Pair{});
      p = &pairs.back();
    }
    std::copy(peer, peer + kIdLen, p->id);
    p->name = name.substr(0, 16);
    std::copy(ltk, ltk + kKeyLen, p->ltk);
    save();
  }
  // ["PLK1"][id 8][count][per pairing: id 8, name str8, ltk 32]
  void save() const {
    if (file.empty()) return;
    std::vector<uint8_t> b{'P', 'L', 'K', '1'};
    b.insert(b.end(), id, id + kIdLen);
    b.push_back(static_cast<uint8_t>(pairs.size()));
    for (const Pair& p : pairs) {
      b.insert(b.end(), p.id, p.id + kIdLen);
      b.push_back(static_cast<uint8_t>(p.name.size()));
      b.insert(b.end(), p.name.begin(), p.name.end());
      b.insert(b.end(), p.ltk, p.ltk + kKeyLen);
    }
    std::vector<uint8_t> out = protect(b, false);
    std::fill(b.begin(), b.end(), 0);
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    f.close();
#ifndef _WIN32
    chmod(file.string().c_str(), 0600);
#endif
  }
  void load() {
    std::ifstream f(file, std::ios::binary);
    if (!f) return save();  // a new identity, kept from now on
    std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<uint8_t> b = protect(raw, true);
    size_t at = 4 + kIdLen + 1;
    if (b.size() < at || !std::equal(b.begin(), b.begin() + 4, "PLK1")) return;
    std::copy(b.begin() + 4, b.begin() + 4 + kIdLen, id);
    pairs.clear();
    for (int n = b[4 + kIdLen]; n > 0 && at + kIdLen + 1 <= b.size(); n--) {
      Pair p;
      std::copy(b.begin() + static_cast<long>(at), b.begin() + static_cast<long>(at + kIdLen), p.id);
      size_t len = b[at + kIdLen];
      if (at + kIdLen + 1 + len + kKeyLen > b.size()) break;
      p.name.assign(b.begin() + static_cast<long>(at + kIdLen + 1), b.begin() + static_cast<long>(at + kIdLen + 1 + len));
      std::copy(b.begin() + static_cast<long>(at + kIdLen + 1 + len),
                b.begin() + static_cast<long>(at + kIdLen + 1 + len + kKeyLen), p.ltk);
      pairs.push_back(p);
      at += kIdLen + 1 + len + kKeyLen;
    }
    std::fill(b.begin(), b.end(), 0);
  }
  void resetLink() {
#ifdef PC1500_HAVE_EXPANSION_KEYWORDS
    ls_session_end(&session);
#endif
    authed = peerKnown = false;
    std::fill(linkLtk, linkLtk + kKeyLen, 0);
    resetAdvertiserPair();
    resetConnectorPair();
  }
  void resetAdvertiserPair() {
    aStep = 0;
    aAnswer = -1;
    std::fill(aSk, aSk + 32, 0);
    std::fill(aLtk, aLtk + kKeyLen, 0);
  }
  void resetConnectorPair() {
    cActive = false;
    std::fill(cLtk, cLtk + kKeyLen, 0);
  }
};

LinkCore::LinkCore(std::unique_ptr<Transport> transport, std::filesystem::path filesDir, std::string name)
    : transport_(std::move(transport)),
      filesDir_(std::move(filesDir)),
      name_(std::move(name)),
      sec_(std::make_unique<Security>()) {
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

void LinkCore::onFrame(const std::vector<uint8_t>& raw) {
  if (raw.size() < 4 || raw.size() - 4 != static_cast<size_t>(raw[2] | raw[3] << 8)) {
    log("Bad frame (" + std::to_string(raw.size()) + " bytes)");
    return;
  }
  std::vector<uint8_t> f = raw;
#ifdef PC1500_HAVE_EXPANSION_KEYWORDS
  {
    std::lock_guard<std::mutex> lock(sec_->m);
    if (sec_->session.on) {  // sec.7: every frame sealed; one that fails is dropped
      std::vector<uint8_t> plain(raw.size());
      uint16_t n = ls_open(&sec_->session, raw.data(), static_cast<uint16_t>(raw.size()), plain.data());
      if (n == 0) return log("Dropped a frame that failed authentication");
      plain.resize(n);
      f = std::move(plain);
    }
  }
#endif
  {
    std::lock_guard<std::mutex> lock(mutex_);
    Frame frame{f[0], f[1], std::vector<uint8_t>(f.begin() + 4, f.end())};
    if (f[0] == kAck || f[0] == kErr) {
      answerType_ = f[0];
      answerSeq_ = f[1];
      answerCode_ = f.size() > 4 ? f[4] : 0;
      answerData_.assign(f.begin() + 4, f.end());  // a pairing ACK's payload
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
    linkGen_++;
    asServer_ = connected && asAdvertiser;
    txSeq_ = 0;
    answerReady_ = false;
    incoming_.clear();
    toServe_.clear();
    helloDone_ = false;
    offerIn_ = offerOut_ = answered_ = false;  // sec.5: a dropped link drops them
    {
      std::lock_guard<std::mutex> slock(sec_->m);
      sec_->resetLink();
    }
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
#ifdef PC1500_HAVE_EXPANSION_KEYWORDS
  {
    std::lock_guard<std::mutex> lock(sec_->m);
    if (sec_->session.on) {  // sec.7
      std::vector<uint8_t> sealed(f.size() + kSealOverhead);
      if (ls_seal(&sec_->session, f.data(), static_cast<uint16_t>(f.size()), sealed.data()) == 0) return false;
      f = std::move(sealed);
    }
  }
#endif
  if (f.size() > transport_->frameMax()) return false;
  return transport_->send(f);
}

// A frame in the clear even with the session started: the advertiser's
// ACK of AUTH (sec.7), sent after its session starts so that nothing the
// connector seals next can arrive before it.
bool LinkCore::sendPlain(uint8_t type, uint8_t seq, const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> f{type, seq, static_cast<uint8_t>(payload.size()), static_cast<uint8_t>(payload.size() >> 8)};
  f.insert(f.end(), payload.begin(), payload.end());
  return transport_->send(f);
}

size_t LinkCore::plainMax() { return transport_->frameMax() - kSealOverhead; }

bool LinkCore::authed() const {
  std::lock_guard<std::mutex> lock(sec_->m);
  return sec_->authed;
}

// A frame out and its answer back: 0 for ACK, the ERR code, kNoLink, or
// kTimeout (the link is then dropped -- sec.4).
int LinkCore::request(uint8_t type, const std::vector<uint8_t>& payload) {
  uint8_t seq;
  unsigned gen;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!linked_) return kNoLink;
    gen = linkGen_;
    seq = txSeq_++;
    answerReady_ = false;
  }
  if (!sendFrame(type, seq, payload)) return kNoLink;
  std::unique_lock<std::mutex> lock(mutex_);
  bool done = cv_.wait_for(lock, std::chrono::milliseconds(answerTimeoutMs_),
                           [&] { return (answerReady_ && answerSeq_ == seq) || linkGen_ != gen; });
  if (linkGen_ != gen) return kNoLink;  // the link this went out on is gone
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
  unsigned gen = linkGen_;
  bool got = cv_.wait_for(lock, std::chrono::milliseconds(answerTimeoutMs_),
                          [&] { return !incoming_.empty() || linkGen_ != gen; });
  if (linkGen_ != gen) return false;
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

// Our HELLO's start (sec.5): version, kind, name; the rest is sec.7's.
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
      if (isLinked() && !authed()) return fail(kErrNotPaired);
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
      if (isLinked() && !authed()) return fail(kErrNotPaired);
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
    case kCmdPairAnswer: {  // the emulated PC-1500's BLADV: its user's answer
      std::lock_guard<std::mutex> lock(sec_->m);
      if (sec_->aStep == 2) sec_->aAnswer = w[0] ? 1 : 0;
      return kStatusSuccess;
    }
    case kCmdUnpair: {  // [len][name], 0 = all; out: how many
      std::string name(w.begin() + 1, w.begin() + 1 + std::min<int>(w[0], 16));
      std::lock_guard<std::mutex> lock(sec_->m);
      size_t before = sec_->pairs.size();
      sec_->pairs.erase(std::remove_if(sec_->pairs.begin(), sec_->pairs.end(),
                                       [&](const Security::Pair& p) { return name.empty() || sameName(p.name, name); }),
                        sec_->pairs.end());
      w[0] = static_cast<uint8_t>(before - sec_->pairs.size());
      sec_->save();
      return kStatusSuccess;
    }
    default:
      break;
  }
  // sec.7: only an authenticated link carries anything but pairing
  if ((cmd == kCmdText || cmd == kCmdPlot || cmd == kCmdFilePut ||
       cmd == kCmdFileGet) && isLinked() && !authed())
    return fail(kErrNotPaired);
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
    case kCmdPairBegin:
      return pairBegin(w);
    case kCmdPairConfirm:
      return pairConfirm(w);
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
      std::vector<uint8_t> frame(plainMax() - 4);
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

// Connects, exchanges HELLOs and authenticates (sec.5, 7) -- or, for a
// pairing about to start, stops after the HELLOs; the peer's name to the
// window. ERROR: [kErrNotPaired], [kErrAuthFailed], [0].
uint8_t LinkCore::connectTo(const Peer& peer, std::vector<uint8_t>& w, bool authenticate) {
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
      linkGen_++;
      txSeq_ = 0;
    }
  }
  return helloExchange(w, authenticate);
}

uint8_t LinkCore::helloExchange(std::vector<uint8_t>& w, bool authenticate) {
#ifndef PC1500_HAVE_EXPANSION_KEYWORDS
  (void)authenticate;
  dropLink("Built without the expansion firmware's link security");
  w[0] = 0;
  return kStatusError;
#else
  auto failWith = [&](const std::string& why, uint8_t code) {
    dropLink(why);
    w[0] = code;
    return kStatusError;
  };
  std::vector<uint8_t> h = hello();
  {
    std::lock_guard<std::mutex> lock(sec_->m);
    sec_->resetLink();
    randomBytes(sec_->nonceC, kNonceLen);
    h.insert(h.end(), sec_->id, sec_->id + kIdLen);
    h.insert(h.end(), sec_->nonceC, sec_->nonceC + kNonceLen);
  }
  Frame f;
  int r = request(kHello, h);
  if (r != 0 || !receive(&f))
    return failWith(r == kErrUnsupported ? "The peer speaks an older version" : "HELLO failed",
                    r == kErrUnsupported ? kErrUnsupported : 0);
  const std::vector<uint8_t>& p = f.payload;
  size_t nn = p.size() >= 3 ? p[2] : 0, rest = 3 + nn;
  bool known = p.size() >= rest + kIdLen + kNonceLen + 1 && p[rest + kIdLen + kNonceLen] == 1;
  if (f.type != kHello || p.size() < rest + kIdLen + kNonceLen + 1 || p[0] != kVersion ||
      (known && p.size() < rest + kIdLen + kNonceLen + 1 + kProofLen)) {
    answer(f.seq, kErrUnsupported);
    return failWith("The peer isn't compatible", kErrUnsupported);
  }
  answer(f.seq);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    peerName_.assign(p.begin() + 3, p.begin() + 3 + static_cast<long>(nn));
  }
  uint8_t proof[kProofLen], ltk[kKeyLen];
  {
    std::lock_guard<std::mutex> lock(sec_->m);
    std::copy(p.begin() + static_cast<long>(rest), p.begin() + static_cast<long>(rest + kIdLen), sec_->peerId);
    std::copy(p.begin() + static_cast<long>(rest + kIdLen), p.begin() + static_cast<long>(rest + kIdLen + kNonceLen),
              sec_->nonceS);
    if (authenticate) {
      const Security::Pair* pair = sec_->find(sec_->peerId);
      if (!pair) known = false;
      else std::copy(pair->ltk, pair->ltk + kKeyLen, ltk);
    }
  }
  w[0] = static_cast<uint8_t>(std::min<size_t>(peerName_.size(), 16));
  std::copy(peerName_.begin(), peerName_.begin() + w[0], w.begin() + 1);
  if (!authenticate) {
    log("Connected to " + peerName_ + ", unpaired");
    return kStatusSuccess;
  }
  if (!known) return failWith(peerName_ + " isn't paired with this emulator", kErrNotPaired);
  {
    std::lock_guard<std::mutex> lock(sec_->m);
    ls_auth_proof(ltk, 'S', sec_->nonceC, sec_->nonceS, sec_->id, sec_->peerId, proof);
  }
  if (!ls_equal16(proof, &p[rest + kIdLen + kNonceLen + 1])) return failWith(peerName_ + " failed to prove itself", kErrAuthFailed);
  {
    std::lock_guard<std::mutex> lock(sec_->m);
    ls_auth_proof(ltk, 'C', sec_->nonceC, sec_->nonceS, sec_->id, sec_->peerId, proof);
  }
  r = request(kAuth, std::vector<uint8_t>(proof, proof + kProofLen));
  if (r != 0)
    return failWith(r == kErrAuthFailed ? peerName_ + " no longer has our pairing" : "Authentication failed",
                    r == kErrAuthFailed || r == kErrNotPaired ? static_cast<uint8_t>(r) : 0);
  {
    std::lock_guard<std::mutex> lock(sec_->m);
    ls_session_start(&sec_->session, ltk, sec_->nonceC, sec_->nonceS, true);
    sec_->authed = true;
  }
  std::fill(ltk, ltk + kKeyLen, 0);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    helloDone_ = true;
  }
  log("Connected to " + peerName_ + " (authenticated)");
  w[0] = static_cast<uint8_t>(std::min<size_t>(peerName_.size(), 16));
  std::copy(peerName_.begin(), peerName_.begin() + w[0], w.begin() + 1);
  return kStatusSuccess;
#endif
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
  size_t chunk = plainMax() - 5;
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
  size_t chunk = plainMax() - 4;
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
  {
    std::lock_guard<std::mutex> slock(sec_->m);
    if (sec_->aStep == 2 && sec_->aAnswer < 0) {  // a connector's pairing: its code, for BLADV's user
      flags |= kStatusPairAsk;
      uint32_t code = sec_->aCode;
      for (int i = 5; i >= 0; i--, code /= 10) w[kFileArgs + i] = static_cast<uint8_t>('0' + code % 10);
    }
  }
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
  switch (f.type) {  // sec.7: anything else needs an authenticated link
    case kHello:
      return serveHello(f);
    case kAuth:
      return serveAuth(f);
    case kPairStart:
    case kPairNonce:
    case kPairConfirm:
      return servePair(f);
    case kBye:
    case kAck:
    case kErr:
      break;
    default:
      if (!authed()) return answer(f.seq, kErrNotPaired);
      break;
  }
  switch (f.type) {
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
  size_t chunk = plainMax() - 4;
  for (size_t at = 0; at < data.size(); at += chunk) {
    std::vector<uint8_t> part(data.begin() + static_cast<long>(at), data.begin() + static_cast<long>(std::min(data.size(), at + chunk)));
    if (int r = request(kFileData, part); r != 0) return log(name + ": stopped at byte " + std::to_string(at));
  }
  if (request(kFileEnd, {}) == 0) log("Sent " + name);
}

// ---- advertiser: sec.7's HELLO, AUTH and pairing ----

// The connector's HELLO: ACK it, and send ours -- with our proof if we have
// a pairing for its id. Also the HELLO a connector sends again after pairing.
void LinkCore::serveHello(const Frame& f) {
#ifndef PC1500_HAVE_EXPANSION_KEYWORDS
  answer(f.seq, kErrUnsupported);
#else
  const std::vector<uint8_t>& p = f.payload;
  size_t nn = p.size() >= 3 ? p[2] : 0;
  if (p.size() < 3 || p[0] != kVersion || p.size() < 3 + nn + kIdLen + kNonceLen) {
    log("A peer with an older Link version was refused");
    return answer(f.seq, kErrUnsupported);
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    peerName_.assign(p.begin() + 3, p.begin() + 3 + static_cast<long>(nn));
    helloDone_ = false;
  }
  std::vector<uint8_t> h = hello();
  {
    std::lock_guard<std::mutex> lock(sec_->m);
    sec_->resetLink();
    std::copy(p.begin() + static_cast<long>(3 + nn), p.begin() + static_cast<long>(3 + nn + kIdLen), sec_->peerId);
    std::copy(p.begin() + static_cast<long>(3 + nn + kIdLen), p.begin() + static_cast<long>(3 + nn + kIdLen + kNonceLen),
              sec_->nonceC);
    randomBytes(sec_->nonceS, kNonceLen);
    const Security::Pair* pair = sec_->find(sec_->peerId);
    sec_->peerKnown = pair != nullptr;
    if (pair) std::copy(pair->ltk, pair->ltk + kKeyLen, sec_->linkLtk);
    h.insert(h.end(), sec_->id, sec_->id + kIdLen);
    h.insert(h.end(), sec_->nonceS, sec_->nonceS + kNonceLen);
    h.push_back(pair ? 1 : 0);
    if (pair) {
      uint8_t proof[kProofLen];
      ls_auth_proof(sec_->linkLtk, 'S', sec_->nonceC, sec_->nonceS, sec_->peerId, sec_->id, proof);
      h.insert(h.end(), proof, proof + kProofLen);
    }
  }
  log("HELLO from " + peerName_);
  answer(f.seq);
  if (request(kHello, h) != 0) return log("Our HELLO wasn't answered");
#endif
}

// AUTH: the connector's proof; the session starts before the ACK goes out
// (in the clear, the last frame that is).
void LinkCore::serveAuth(const Frame& f) {
#ifndef PC1500_HAVE_EXPANSION_KEYWORDS
  answer(f.seq, kErrUnsupported);
#else
  uint8_t want[kProofLen];
  {
    std::lock_guard<std::mutex> lock(sec_->m);
    if (!sec_->peerKnown || sec_->authed || f.payload.size() != kProofLen) {
      uint8_t code = sec_->peerKnown ? kErrBadFrame : kErrNotPaired;
      sendPlain(kErr, f.seq, {code});
      return;
    }
    ls_auth_proof(sec_->linkLtk, 'C', sec_->nonceC, sec_->nonceS, sec_->peerId, sec_->id, want);
    if (!ls_equal16(want, f.payload.data())) {
      sendPlain(kErr, f.seq, {kErrAuthFailed});
      log(peerName_ + " failed to prove itself");
      return;
    }
    ls_session_start(&sec_->session, sec_->linkLtk, sec_->nonceC, sec_->nonceS, false);
    std::fill(sec_->linkLtk, sec_->linkLtk + kKeyLen, 0);
    sec_->authed = true;
    sendPlain(kAck, f.seq, {});
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    helloDone_ = true;
  }
  log(peerName_ + " authenticated");
#endif
}

// The pairing frames: answered at once, the ACKs carrying our side; the
// user's answer comes from the UI (answerPairing) or the emulated PC-1500's
// BLADV (PAIR_ANSWER).
void LinkCore::servePair(const Frame& f) {
#ifndef PC1500_HAVE_EXPANSION_KEYWORDS
  answer(f.seq, kErrUnsupported);
#else
  const std::vector<uint8_t>& p = f.payload;
  std::vector<uint8_t> out;
  uint8_t err = 0;
  std::string note, peer;
  {
    std::lock_guard<std::mutex> lock(mutex_);  // before sec_->m, never inside it (status() takes them in this order)
    peer = peerName_;
  }
  {
    std::lock_guard<std::mutex> lock(sec_->m);
    Security& s = *sec_;
    if (s.authed) {
      err = kErrBadFrame;
    } else if (f.type == kPairStart && p.size() == 32) {
      uint8_t r[32];
      s.resetAdvertiserPair();
      randomBytes(r, sizeof r);
      ls_keypair(r, s.aSk, s.aPkS);
      std::fill(r, r + 32, 0);
      std::copy(p.begin(), p.end(), s.aPkC);
      randomBytes(s.aNs, kNonceLen);
      out.assign(s.aPkS, s.aPkS + 32);
      out.resize(32 + kProofLen);
      ls_pair_commit(s.aPkS, s.aPkC, s.aNs, out.data() + 32);
      s.aStep = 1;
    } else if (f.type == kPairNonce && p.size() == kNonceLen && s.aStep == 1) {
      std::copy(p.begin(), p.end(), s.aNc);
      if (!ls_pair_ltk(s.aSk, s.aPkC, s.aPkC, s.aPkS, s.aNc, s.aNs, s.aLtk)) {
        s.resetAdvertiserPair();
        err = kErrAuthFailed;
      } else {
        std::fill(s.aSk, s.aSk + 32, 0);
        s.aCode = ls_pair_code(s.aPkC, s.aPkS, s.aNc, s.aNs);
        s.aAnswer = -1;
        s.aStep = 2;
        s.aName = peer;
        out.assign(s.aNs, s.aNs + kNonceLen);
        note = s.aName + " wants to pair: check its code";
      }
    } else if (f.type == kPairConfirm && p.size() == 1 + kProofLen && s.aStep == 2) {
      if (s.aAnswer < 0) {
        err = kErrBusy;  // our user hasn't answered: the connector asks again
      } else if (s.aAnswer == 0 || p[0] != 1) {
        s.resetAdvertiserPair();
        out = {0};
        note = "Pairing refused";
      } else {
        uint8_t want[kProofLen];
        ls_pair_confirm(s.aLtk, 'C', s.peerId, s.id, want);
        if (!ls_equal16(want, p.data() + 1)) {
          s.resetAdvertiserPair();
          err = kErrAuthFailed;
        } else {
          s.add(s.peerId, s.aName, s.aLtk);
          out.assign(1 + kProofLen, 1);
          ls_pair_confirm(s.aLtk, 'S', s.peerId, s.id, out.data() + 1);
          note = "Paired with " + s.aName;
          s.resetAdvertiserPair();
        }
      }
    } else {
      s.resetAdvertiserPair();
      err = kErrBadFrame;
    }
  }
  if (!note.empty()) log(note);
  if (err) return answer(f.seq, err);
  sendFrame(kAck, f.seq, out);
#endif
}

// ---- connector: BLPAIR (sec.7) ----

uint8_t LinkCore::pairBegin(std::vector<uint8_t>& w) {
#ifndef PC1500_HAVE_EXPANSION_KEYWORDS
  w[0] = 0;
  return kStatusError;
#else
  auto fail = [&](const std::string& why, uint8_t code) {
    dropLink(why);
    std::lock_guard<std::mutex> lock(sec_->m);
    sec_->resetConnectorPair();
    w[0] = code;
    return kStatusError;
  };
  Peer peer;
  if (w[0] == 0) {
    if (w[1] >= listed_.size()) return fail("No such peer", 0);
    peer = listed_[w[1]];
  } else {
    std::string wanted(w.begin() + 2, w.begin() + 2 + std::min<int>(w[1], 40));
    bool found = false;
    for (const Peer& p : transport_->scan(3000))
      if (sameName(p.name, wanted)) peer = p, found = true;
    if (!found) return fail("No peer named " + wanted, 0);
  }
  if (connectTo(peer, w, false) != kStatusSuccess) return kStatusError;
  std::string name = peerName_.substr(0, 16);
  uint8_t sk[32], pk[32], commit[kProofLen], nc[kNonceLen];
  randomBytes(sk, 32);
  ls_keypair(sk, sk, pk);
  randomBytes(nc, kNonceLen);
  int r = request(kPairStart, std::vector<uint8_t>(pk, pk + 32));
  if (r != 0 || answerData_.size() != 32 + kProofLen) return fail("Pairing refused", r > 0 ? static_cast<uint8_t>(r) : 0);
  std::vector<uint8_t> first = answerData_;
  r = request(kPairNonce, std::vector<uint8_t>(nc, nc + kNonceLen));
  if (r != 0 || answerData_.size() != kNonceLen) return fail("Pairing failed", r > 0 ? static_cast<uint8_t>(r) : 0);
  uint8_t check[kProofLen];
  bool ok;
  std::copy(first.begin() + 32, first.end(), commit);
  {
    std::lock_guard<std::mutex> lock(sec_->m);
    Security& s = *sec_;
    std::copy(pk, pk + 32, s.cPkC);
    std::copy(first.begin(), first.begin() + 32, s.cPkS);
    std::copy(nc, nc + kNonceLen, s.cNc);
    std::copy(answerData_.begin(), answerData_.end(), s.cNs);
    ls_pair_commit(s.cPkS, s.cPkC, s.cNs, check);
    ok = ls_equal16(check, commit) && ls_pair_ltk(sk, s.cPkS, s.cPkC, s.cPkS, s.cNc, s.cNs, s.cLtk);
    std::fill(sk, sk + 32, 0);
    s.cActive = ok;
    if (ok) {
      uint32_t code = ls_pair_code(s.cPkC, s.cPkS, s.cNc, s.cNs);
      for (int i = 5; i >= 0; i--, code /= 10) w[i] = static_cast<uint8_t>('0' + code % 10);
    }
  }
  if (!ok) return fail("The pairing was tampered with", kErrAuthFailed);
  w[6] = static_cast<uint8_t>(name.size());
  std::copy(name.begin(), name.end(), w.begin() + 7);
  return kStatusSuccess;
#endif
}

uint8_t LinkCore::pairConfirm(std::vector<uint8_t>& w) {
#ifndef PC1500_HAVE_EXPANSION_KEYWORDS
  w[0] = 0;
  return kStatusError;
#else
  bool ok = w[0] != 0;
  std::vector<uint8_t> payload(1 + kProofLen);
  {
    std::lock_guard<std::mutex> lock(sec_->m);
    if (!sec_->cActive) {
      w[0] = 0;
      return kStatusError;
    }
    payload[0] = ok ? 1 : 0;
    ls_pair_confirm(sec_->cLtk, 'C', sec_->id, sec_->peerId, payload.data() + 1);
  }
  int r = request(kPairConfirm, payload);
  if (r == kErrBusy) {  // the peer's user hasn't answered yet
    w[0] = 0;
    return kStatusSuccess;
  }
  auto end = [&] {
    std::lock_guard<std::mutex> lock(sec_->m);
    sec_->resetConnectorPair();
  };
  if (r != 0 || answerData_.empty()) {
    end();
    dropLink("Pairing failed");
    w[0] = r > 0 ? static_cast<uint8_t>(r) : 0;
    return kStatusError;
  }
  if (!ok || answerData_[0] != 1) {
    end();
    log("Pairing refused");
    disconnectLink();
    w[0] = 2;
    return kStatusSuccess;
  }
  bool good;
  {
    std::lock_guard<std::mutex> lock(sec_->m);
    uint8_t want[kProofLen];
    ls_pair_confirm(sec_->cLtk, 'S', sec_->id, sec_->peerId, want);
    good = answerData_.size() == 1 + kProofLen && ls_equal16(want, answerData_.data() + 1);
    if (good) sec_->add(sec_->peerId, peerName_, sec_->cLtk);
    sec_->resetConnectorPair();
  }
  if (!good) {
    dropLink("The pairing was tampered with");
    w[0] = kErrAuthFailed;
    return kStatusError;
  }
  log("Paired with " + peerName_);
  std::vector<uint8_t> rest(w.size());
  if (helloExchange(rest, true) != kStatusSuccess) {  // the new pairing, used at once
    w[0] = rest[0];
    return kStatusError;
  }
  w[0] = 1;
  std::copy(rest.begin(), rest.begin() + 1 + rest[0], w.begin() + 1);
  return kStatusSuccess;
#endif
}

// ---- pairing: the UI's side ----

void LinkCore::setPairingFile(const std::filesystem::path& path) {
  std::lock_guard<std::mutex> lock(sec_->m);
  sec_->file = path;
  sec_->load();
}

bool LinkCore::pendingPairing(std::string* code, std::string* name) const {
  std::lock_guard<std::mutex> lock(sec_->m);
  if (sec_->aStep != 2 || sec_->aAnswer >= 0) return false;
  char text[8];
  std::snprintf(text, sizeof text, "%06u", static_cast<unsigned>(sec_->aCode));
  *code = text;
  *name = sec_->aName;
  return true;
}

void LinkCore::answerPairing(bool accept) {
  std::lock_guard<std::mutex> lock(sec_->m);
  if (sec_->aStep == 2) sec_->aAnswer = accept ? 1 : 0;
}

std::vector<std::pair<std::string, std::string>> LinkCore::pairings() const {
  std::lock_guard<std::mutex> lock(sec_->m);
  std::vector<std::pair<std::string, std::string>> out;
  for (const Security::Pair& p : sec_->pairs) out.emplace_back(p.name, hexOf(p.id, kIdLen));
  return out;
}

void LinkCore::forgetPairing(const std::string& idHex) {
  std::lock_guard<std::mutex> lock(sec_->m);
  sec_->pairs.erase(std::remove_if(sec_->pairs.begin(), sec_->pairs.end(),
                                   [&](const Security::Pair& p) { return idHex.empty() || hexOf(p.id, kIdLen) == idHex; }),
                    sec_->pairs.end());
  sec_->save();
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
