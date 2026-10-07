// ssh_mock.cpp -- see ssh_mock.h. Mirrors RP2350/ssh_session.c; the
// differences are the socket in place of lwIP, and the stores in memory.
#include "ssh_mock.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>
using sock_t = SOCKET;
static const sock_t kNoSocket = INVALID_SOCKET;
static void closeSocket(sock_t s) { closesocket(s); }
#else
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
static const sock_t kNoSocket = -1;
static void closeSocket(sock_t s) { close(s); }
#endif

#ifdef PC1500_HAVE_EXPANSION_KEYWORDS
extern "C" {
#include "pc_exp.h"
#include "ssh_client.h"
#include "ssh_keys.h"
#include "ssh_term.h"
#include "third_party/monocypher/monocypher-ed25519.h"
#include "third_party/monocypher/monocypher.h"
}
#endif

namespace pc1500 {

#ifdef PC1500_HAVE_EXPANSION_KEYWORDS

namespace {

constexpr int kStepMs = 500;
constexpr int kBlinkMs = 500;
constexpr char kCursorChar = '_';


int b64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

uint32_t be32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 | p[3];
}

}  // namespace

struct SshMock::Impl {
  ssh_t ssh{};
  ssh_term_t term{};
  ssh_keys_t keys{};
  sock_t sock = kNoSocket;
  bool session = false;
  std::atomic<bool> terminal{false};
  uint8_t error = EXP_SSH_ERR_NONE;
  std::vector<uint8_t> pending;  // received, not yet taken by ssh_feed()
  bool gone = false;             // the server closed the connection
  std::string host;
  uint16_t port = 22;
  uint8_t secret[64]{};
  uint8_t publicKey[32]{};
  std::map<std::string, std::vector<uint8_t>> knownHosts;  // "host:port" (lower case) -> key
  uint8_t breakSeen = 0;
  uint32_t shownVersion = 0;
  bool blink = true;
  std::chrono::steady_clock::time_point nextBlink;
  std::mt19937_64 rng{std::random_device{}()};

  Impl() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    uint8_t seed[32];
    random(seed, sizeof seed);
    crypto_ed25519_key_pair(secret, publicKey, seed);
  }
  ~Impl() {
    if (sock != kNoSocket) closeSocket(sock);
  }

  void random(uint8_t* out, size_t len) {
    while (len) {
      uint64_t r = rng();
      size_t n = len < 8 ? len : 8;
      std::memcpy(out, &r, n);
      out += n;
      len -= n;
    }
  }

  static bool ioSend(void* ctx, const uint8_t* data, size_t len) {
    Impl* self = static_cast<Impl*>(ctx);
    while (len) {
      int n = send(self->sock, reinterpret_cast<const char*>(data), static_cast<int>(len), 0);
      if (n <= 0) return false;
      data += n;
      len -= static_cast<size_t>(n);
    }
    return true;
  }
  static void ioRandom(void* ctx, uint8_t* out, size_t len) { static_cast<Impl*>(ctx)->random(out, len); }
  static void ioData(void* ctx, const uint8_t* data, size_t len) {
    ssh_term_output(&static_cast<Impl*>(ctx)->term, data, len);
  }

  // What's arrived within `waitMs`, to the protocol (as ssh_session.c's pump()).
  void pump(int waitMs) {
    if (sock != kNoSocket && !gone) {
      fd_set rd;
      FD_ZERO(&rd);
      FD_SET(sock, &rd);
      timeval tv{waitMs / 1000, (waitMs % 1000) * 1000};
      if (select(static_cast<int>(sock + 1), &rd, nullptr, nullptr, &tv) > 0) {
        uint8_t buf[4096];
        int n = recv(sock, reinterpret_cast<char*>(buf), sizeof buf, 0);
        if (n <= 0) gone = true;
        else pending.insert(pending.end(), buf, buf + n);
      }
    }
    if (!pending.empty() && ssh_state(&ssh) != SSH_ST_CLOSED) {
      size_t taken = ssh_feed(&ssh, pending.data(), pending.size());
      pending.erase(pending.begin(), pending.begin() + static_cast<long>(taken));
    }
    if (gone && pending.empty() && ssh_state(&ssh) != SSH_ST_CLOSED) ssh_close(&ssh, true);
  }

  uint8_t errorCode() const {
    switch (ssh_error(&ssh)) {
      case SSH_ERR_NONE: return EXP_SSH_ERR_NONE;
      case SSH_ERR_IO: return EXP_SSH_ERR_LOST;
      case SSH_ERR_HOSTKEY: return EXP_SSH_ERR_HOSTKEY;
      case SSH_ERR_REJECTED: return EXP_SSH_ERR_REJECTED;
      case SSH_ERR_AUTH: return EXP_SSH_ERR_AUTH;
      case SSH_ERR_CHANNEL: return EXP_SSH_ERR_CHANNEL;
      case SSH_ERR_DISCONNECTED: return ssh.disconnect_reason == 14 ? EXP_SSH_ERR_AUTH : EXP_SSH_ERR_LOST;
      default: return EXP_SSH_ERR_PROTOCOL;
    }
  }

  void teardown() {
    if (!session) return;
    if (ssh_state(&ssh) != SSH_ST_CLOSED) ssh_close(&ssh, false);
    if (error == EXP_SSH_ERR_NONE) error = errorCode();
    if (sock != kNoSocket) closeSocket(sock);
    sock = kNoSocket;
    session = false;
    terminal = false;
  }

  std::string hostKeyName() const {
    std::string name = host;
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return name + ":" + std::to_string(port);
  }

  uint8_t open(std::vector<uint8_t>& w, bool wifiUp) {
    const uint8_t* p = w.data();
    teardown();
    std::string user(reinterpret_cast<const char*>(p + 1), std::min<size_t>(p[0], EXP_SSH_USER_MAX));
    p += 1 + EXP_SSH_USER_MAX;
    host.assign(reinterpret_cast<const char*>(p + 1), std::min<size_t>(p[0], EXP_SSH_HOST_MAX));
    p += 1 + EXP_SSH_HOST_MAX;
    port = static_cast<uint16_t>(p[0] << 8 | p[1]);
    p += 2;
    std::string password;
    if (p[0] != EXP_SSH_PW_NONE) password.assign(reinterpret_cast<const char*>(p + 1), std::min<size_t>(p[0], EXP_SSH_PW_MAX));
    auto failure = [&](uint8_t code) {
      w[0] = code;
      return EXP_STATUS_ERROR;
    };
    if (!wifiUp) return failure(EXP_SSH_ERR_NO_WIFI);
    addrinfo hints{}, *ai = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &ai) != 0 || !ai) return failure(EXP_SSH_ERR_NOT_FOUND);
    sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    bool connected = sock != kNoSocket && connect(sock, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0;
    freeaddrinfo(ai);
    if (!connected) {
      if (sock != kNoSocket) closeSocket(sock);
      sock = kNoSocket;
      return failure(EXP_SSH_ERR_REFUSED);
    }
    int one = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
    pending.clear();
    gone = false;
    ssh_term_init(&term);
    ssh_keys_init(&keys);
    session = true;
    error = EXP_SSH_ERR_NONE;
    ssh_io_t io{this, ioSend, ioRandom, ioData};
    ssh_start(&ssh, &io, user.c_str(), secret, password.empty() ? nullptr : password.c_str());
    return EXP_STATUS_SUCCESS;
  }

  // As ssh_session.c's check_hostkey(): true if the user has to be asked.
  bool checkHostKey() {
    const uint8_t* key = ssh_hostkey_blob(&ssh) + 19;
    auto it = knownHosts.find(hostKeyName());
    if (it == knownHosts.end()) return true;
    if (std::memcmp(it->second.data(), key, 32) == 0) {
      ssh_hostkey_answer(&ssh, true);
    } else {
      error = EXP_SSH_ERR_HOSTKEY;
      ssh_hostkey_answer(&ssh, false);
    }
    return false;
  }

  uint8_t step(std::vector<uint8_t>& w) {
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kStepMs);
    if (!session) {
      w[0] = EXP_SSH_ST_CLOSED;
      w[1] = error;
      return EXP_STATUS_SUCCESS;
    }
    for (;;) {
      pump(5);
      switch (ssh_state(&ssh)) {
        case SSH_ST_HOSTKEY:
          if (!checkHostKey()) continue;
          w[0] = EXP_SSH_ST_HOSTKEY_NEW;
          w[1] = SSH_FINGERPRINT_LEN;
          ssh_fingerprint(ssh_hostkey_blob(&ssh), reinterpret_cast<char*>(&w[2]));
          return EXP_STATUS_SUCCESS;
        case SSH_ST_PASSWORD: w[0] = EXP_SSH_ST_PASSWORD; return EXP_STATUS_SUCCESS;
        case SSH_ST_OPEN: w[0] = EXP_SSH_ST_OPEN; return EXP_STATUS_SUCCESS;
        case SSH_ST_CLOSED:
          teardown();
          w[0] = EXP_SSH_ST_CLOSED;
          w[1] = error;
          return EXP_STATUS_SUCCESS;
        default: break;
      }
      if (std::chrono::steady_clock::now() >= until) {
        w[0] = EXP_SSH_ST_CONNECTING;
        return EXP_STATUS_SUCCESS;
      }
    }
  }

  uint8_t writeKey(std::vector<uint8_t>& w, const std::filesystem::path& sdRoot, const std::string& hostName) {
    char line[128];
    size_t n = ssh_public_key_line(publicKey, line);
    std::string text = std::string(line, n) + " pc1500@" + hostName + "\n";
    std::ofstream out(sdRoot / "SSHKEY.PUB", std::ios::binary);
    if (sdRoot.empty() || !(out << text)) {
      w[0] = EXP_SSH_ERR_CARD;
      return EXP_STATUS_ERROR;
    }
    uint8_t blob[SSH_HOSTKEY_BLOB_LEN];
    std::memcpy(blob, "\0\0\0\x0bssh-ed25519\0\0\0\x20", 19);
    std::memcpy(blob + 19, publicKey, 32);
    char fp[SSH_FINGERPRINT_LEN + 1];
    ssh_fingerprint(blob, fp);
    w[0] = SSH_FINGERPRINT_LEN;
    std::memcpy(&w[1], fp, SSH_FINGERPRINT_LEN);
    return EXP_STATUS_SUCCESS;
  }

  void publish(std::vector<uint8_t>& w) {
    auto now = std::chrono::steady_clock::now();
    if (now >= nextBlink) {
      blink = !blink;
      nextBlink = now + std::chrono::milliseconds(kBlinkMs);
      shownVersion = term.version - 1;
    }
    if (term.version == shownVersion) return;
    shownVersion = term.version;
    char line[TERM_WIDTH];
    uint8_t cursor = ssh_term_render(&term, line);
    if (cursor != TERM_NO_CURSOR && blink) line[cursor] = kCursorChar;
    std::memcpy(&w[EXP_SSH_TERM_LINE], line, TERM_WIDTH);
    w[EXP_SSH_TERM_LINE_COUNT]++;
  }
};

SshMock::SshMock() : impl_(std::make_unique<Impl>()) {}
SshMock::~SshMock() = default;

uint8_t SshMock::command(uint8_t cmd, std::vector<uint8_t>& w, bool wifiUp, const std::filesystem::path& sdRoot,
                         const std::string& hostName) {
  Impl& s = *impl_;
  switch (cmd) {
    case EXP_COMMAND_SSH_OPEN: return s.open(w, wifiUp);
    case EXP_COMMAND_SSH_STEP: return s.step(w);
    case EXP_COMMAND_SSH_ANSWER:
      if (!s.session || ssh_state(&s.ssh) != SSH_ST_HOSTKEY) return EXP_STATUS_ERROR;
      if (w[0]) {
        const uint8_t* key = ssh_hostkey_blob(&s.ssh) + 19;
        s.knownHosts[s.hostKeyName()] = std::vector<uint8_t>(key, key + 32);
      }
      ssh_hostkey_answer(&s.ssh, w[0] != 0);
      return EXP_STATUS_SUCCESS;
    case EXP_COMMAND_SSH_PASSWORD: {
      if (!s.session || ssh_state(&s.ssh) != SSH_ST_PASSWORD) return EXP_STATUS_ERROR;
      std::string pw(reinterpret_cast<const char*>(&w[1]), std::min<size_t>(w[0], EXP_SSH_PW_MAX));
      ssh_password(&s.ssh, pw.c_str());
      return EXP_STATUS_SUCCESS;
    }
    case EXP_COMMAND_SSH_TERM:
      if (!s.session || ssh_state(&s.ssh) != SSH_ST_OPEN) return EXP_STATUS_ERROR;
      s.breakSeen = w[EXP_SSH_TERM_BREAK_COUNT];
      w[EXP_SSH_TERM_KEY] = 0;
      w[EXP_SSH_TERM_CLOSED] = 0;
      w[EXP_SSH_TERM_INDICATORS] = EXP_SSH_IND_SMALL;  // lower case to start with
      s.shownVersion = s.term.version - 1;
      s.nextBlink = std::chrono::steady_clock::now() + std::chrono::milliseconds(kBlinkMs);
      s.blink = true;
      s.terminal = true;
      return EXP_STATUS_SUCCESS;
    case EXP_COMMAND_SSH_CLOSE: s.teardown(); return EXP_STATUS_SUCCESS;
    case EXP_COMMAND_SSH_KEY: return s.writeKey(w, sdRoot, hostName);
    case EXP_COMMAND_SSH_FORGET: {
      std::string host(reinterpret_cast<const char*>(&w[1]), std::min<size_t>(w[0], EXP_SSH_HOST_MAX));
      for (char& c : host) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      int n = 0;
      for (auto it = s.knownHosts.begin(); it != s.knownHosts.end();) {
        if (host.empty() || it->first.compare(0, host.size() + 1, host + ":") == 0) {
          it = s.knownHosts.erase(it);
          n++;
        } else {
          ++it;
        }
      }
      w[0] = static_cast<uint8_t>(n);
      return EXP_STATUS_SUCCESS;
    }
    default: return EXP_STATUS_NOT_IMPLEMENTED;
  }
}

bool SshMock::terminal() const { return impl_->terminal; }

uint8_t SshMock::pingCommand(uint8_t cmd, std::vector<uint8_t>& w, bool wifiUp) {
  static in_addr target{};
  if (cmd == EXP_COMMAND_PING_START) {
    std::string host(reinterpret_cast<const char*>(&w[1]), std::min<size_t>(w[0], EXP_SSH_HOST_MAX));
    if (!wifiUp) {
      w[0] = EXP_SSH_ERR_NO_WIFI;
      return EXP_STATUS_ERROR;
    }
    addrinfo hints{}, *ai = nullptr;
    hints.ai_family = AF_INET;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &ai) != 0 || !ai) {
      w[0] = EXP_SSH_ERR_NOT_FOUND;
      return EXP_STATUS_ERROR;
    }
    target = reinterpret_cast<sockaddr_in*>(ai->ai_addr)->sin_addr;
    freeaddrinfo(ai);
    char ip[16];
    inet_ntop(AF_INET, &target, ip, sizeof ip);
    w[0] = static_cast<uint8_t>(std::strlen(ip));
    std::memcpy(&w[1], ip, w[0]);
    return EXP_STATUS_SUCCESS;
  }
  if (cmd != EXP_COMMAND_PING_ROUND) return EXP_STATUS_NOT_IMPLEMENTED;
  w[0] = 0;
#ifdef _WIN32
  HANDLE icmp = IcmpCreateFile();
  if (icmp != INVALID_HANDLE_VALUE) {
    char data[32] = "PC-1500 WFPING";
    alignas(8) uint8_t reply[sizeof(ICMP_ECHO_REPLY) + sizeof data + 8];
    if (IcmpSendEcho(icmp, target.s_addr, data, sizeof data, nullptr, reply, sizeof reply, 1000) > 0) {
      const ICMP_ECHO_REPLY* r = reinterpret_cast<const ICMP_ECHO_REPLY*>(reply);
      if (r->Status == IP_SUCCESS) {
        w[0] = 1;
        w[1] = static_cast<uint8_t>(r->RoundTripTime >> 8);
        w[2] = static_cast<uint8_t>(r->RoundTripTime);
        w[3] = r->Options.Ttl;
      }
    }
    IcmpCloseHandle(icmp);
  }
#endif
  return EXP_STATUS_SUCCESS;
}

void SshMock::poll(std::vector<uint8_t>& w, uint32_t nowMs) {
  Impl& s = *impl_;
  if (!s.terminal) return;
  s.pump(0);
  uint8_t out[SSH_KEYS_OUT_MAX];
  ssh_view_t view;
  size_t n = ssh_keys_sample(&s.keys, w[EXP_SSH_TERM_KEY], nowMs, out, &view);
  if (n) {
    ssh_term_live(&s.term);
    ssh_write(&s.ssh, out, n);
  }
  switch (view) {
    case SSH_VIEW_UP: ssh_term_scroll(&s.term, 1); break;
    case SSH_VIEW_DOWN: ssh_term_scroll(&s.term, -1); break;
    case SSH_VIEW_LEFT: ssh_term_pan(&s.term, -1); break;
    case SSH_VIEW_RIGHT: ssh_term_pan(&s.term, 1); break;
    case SSH_VIEW_QUIT: ssh_close(&s.ssh, false); break;
    case SSH_VIEW_LIVE: ssh_term_live(&s.term); break;
    default: break;
  }
  uint8_t brk = w[EXP_SSH_TERM_BREAK_COUNT];
  if (brk != s.breakSeen) {
    static const uint8_t kCtrlC = 0x03;
    s.breakSeen = brk;
    ssh_term_live(&s.term);
    ssh_write(&s.ssh, &kCtrlC, 1);
  }
  s.publish(w);
  w[EXP_SSH_TERM_INDICATORS] =
      static_cast<uint8_t>((ssh_keys_def(&s.keys) || ssh_keys_scrolling(&s.keys) ? EXP_SSH_IND_DEF : 0) |
                           (ssh_keys_caps(&s.keys) ? 0 : EXP_SSH_IND_SMALL) |
                           (ssh_keys_shift(&s.keys) ? EXP_SSH_IND_SHIFT : 0));
  if (ssh_state(&s.ssh) == SSH_ST_CLOSED) {
    s.teardown();
    w[EXP_SSH_TERM_CLOSED] = 1;
  }
}

bool SshMock::loadDeviceKeyFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  std::string body = text.str();
  size_t at = body.find("-----BEGIN OPENSSH PRIVATE KEY-----");
  if (at == std::string::npos) return false;
  std::vector<uint8_t> bin;
  uint32_t acc = 0;
  int bits = 0;
  for (size_t i = at + 35; i < body.size() && body[i] != '-'; i++) {
    int v = b64Value(body[i]);
    if (v < 0) continue;
    acc = acc << 6 | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      bin.push_back(static_cast<uint8_t>(acc >> bits));
    }
  }
  // PROTOCOL.key: magic, cipher "none", kdf "none", kdf options, 1 key, the
  // public key, then the private section: check ints, type, public, secret.
  if (bin.size() < 15 || std::memcmp(bin.data(), "openssh-key-v1", 15) != 0) return false;
  size_t p = 15;
  auto skip = [&]() {
    if (p + 4 > bin.size()) return false;
    p += 4 + be32(&bin[p]);
    return p <= bin.size();
  };
  for (int i = 0; i < 2; i++) {  // unencrypted only
    if (p + 8 > bin.size() || be32(&bin[p]) != 4 || std::memcmp(&bin[p + 4], "none", 4) != 0) return false;
    p += 8;
  }
  if (!skip()) return false;  // kdf options
  p += 4;                     // number of keys
  if (!skip()) return false;  // public key
  p += 4 + 8;                 // the private section's length, check ints
  if (!skip() || !skip()) return false;  // type, public key
  if (p + 4 + 64 > bin.size() || be32(&bin[p]) != 64) return false;
  std::memcpy(impl_->secret, &bin[p + 4], 64);
  std::memcpy(impl_->publicKey, &bin[p + 4 + 32], 32);
  return true;
}

bool SshMock::knowsHost(const std::string& host) const {
  std::string name = host;
  for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (const auto& known : impl_->knownHosts)
    if (known.first.compare(0, name.size() + 1, name + ":") == 0) return true;
  return false;
}

std::vector<std::string> SshMock::lines() const {
  const ssh_term_t& t = impl_->term;
  std::vector<std::string> out;
  for (int i = t.count; i >= 1; i--) {
    const term_line_t& l = t.history[(t.newest + TERM_HISTORY - (i - 1)) % TERM_HISTORY];
    out.emplace_back(l.text, l.len);
  }
  out.emplace_back(t.live.text, t.live.len);
  return out;
}

#else  // no firmware sources: no SSH

struct SshMock::Impl {};
SshMock::SshMock() : impl_(std::make_unique<Impl>()) {}
SshMock::~SshMock() = default;
uint8_t SshMock::command(uint8_t, std::vector<uint8_t>&, bool, const std::filesystem::path&, const std::string&) {
  return 64;  // EXP_STATUS_NOT_IMPLEMENTED
}
bool SshMock::terminal() const { return false; }
uint8_t SshMock::pingCommand(uint8_t, std::vector<uint8_t>&, bool) { return 64; }
void SshMock::poll(std::vector<uint8_t>&, uint32_t) {}
bool SshMock::loadDeviceKeyFile(const std::string&) { return false; }
bool SshMock::knowsHost(const std::string&) const { return false; }
std::vector<std::string> SshMock::lines() const { return {}; }

#endif

}  // namespace pc1500
