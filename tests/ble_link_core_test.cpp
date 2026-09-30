// LinkCore (src/bus/ble_link_core.h) over a loopback Transport: one core
// connects, the other advertises and serves, as a real PC-1500 and this
// emulator would over Bluetooth. Driven through the same windows the
// firmware's keywords.c builds (RP2350/pc_exp.h's EXP_COMMAND_BLE_*).
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ble_link_core.h"

namespace fs = std::filesystem;
using pc1500::ble::LinkCore;
using pc1500::ble::Peer;
using pc1500::ble::Transport;

static int g_failures = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::printf("FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__);          \
      g_failures++;                                                          \
    }                                                                        \
  } while (0)

// Two Transports wired to each other. Frames are delivered synchronously;
// `mute` makes one side's frames vanish (a peer that stops answering).
struct Wire;

class LoopTransport : public Transport {
 public:
  LoopTransport(Wire* wire, int side) : wire_(wire), side_(side) {}
  std::string problem() override { return ""; }
  std::string describe() override { return "loopback"; }
  std::vector<Peer> scan(int) override;
  bool connect(const Peer& peer) override;
  bool advertise(bool on, const std::string& name) override;
  bool send(const std::vector<uint8_t>& frame) override;
  void disconnect() override;
  size_t frameMax() override { return 64; }  // small, so transfers take several frames

 private:
  Wire* wire_;
  int side_;
};

struct Wire {
  LoopTransport* side[2] = {nullptr, nullptr};
  bool advertising[2] = {false, false};
  std::string name[2];
  bool connected = false;
  bool mute[2] = {false, false};
  std::mutex mutex;
};

std::vector<Peer> LoopTransport::scan(int) {
  int other = 1 - side_;
  if (!wire_->advertising[other]) return {};
  return {Peer{"LOOP-" + std::to_string(other), wire_->name[other], -42}};
}

bool LoopTransport::connect(const Peer&) {
  int other = 1 - side_;
  if (!wire_->advertising[other]) return false;
  wire_->connected = true;
  onLink(true, false);
  wire_->side[other]->onLink(true, true);
  return true;
}

bool LoopTransport::advertise(bool on, const std::string& name) {
  wire_->advertising[side_] = on;
  wire_->name[side_] = name;
  return true;
}

bool LoopTransport::send(const std::vector<uint8_t>& frame) {
  if (!wire_->connected) return false;
  if (!wire_->mute[side_]) wire_->side[1 - side_]->onFrame(frame);
  return true;
}

void LoopTransport::disconnect() {
  if (!wire_->connected) return;
  wire_->connected = false;
  wire_->side[1 - side_]->onLink(false, false);
}

// A connecting core (the "PC-1500") and a serving one (the "emulator"),
// each with its own files folder.
struct Pair {
  Wire wire;
  fs::path dir[2];
  std::unique_ptr<LinkCore> pc, server;

  explicit Pair(const char* testName) {
    for (int i = 0; i < 2; i++) {
      dir[i] = fs::temp_directory_path() / (std::string(testName) + "_" + std::to_string(i));
      fs::remove_all(dir[i]);
      fs::create_directories(dir[i]);
    }
    auto a = std::make_unique<LoopTransport>(&wire, 0);
    auto b = std::make_unique<LoopTransport>(&wire, 1);
    wire.side[0] = a.get();
    wire.side[1] = b.get();
    pc = std::make_unique<LinkCore>(std::move(a), dir[0], "PC-1500");
    server = std::make_unique<LinkCore>(std::move(b), dir[1], "PC-1500 EMU");
    pc->setAnswerTimeoutMs(300);
    server->setAnswerTimeoutMs(300);
  }
};

std::vector<uint8_t> window() { return std::vector<uint8_t>(2048, 0); }

void nameSlot(std::vector<uint8_t>& w, const std::string& name) {
  w[0] = 0;
  w[1] = static_cast<uint8_t>(name.size());
  std::copy(name.begin(), name.end(), w.begin() + 2);
}

std::string windowName(const std::vector<uint8_t>& w) { return std::string(w.begin() + 1, w.begin() + 1 + w[0]); }

constexpr uint8_t kOk = 2, kError = 128;
constexpr size_t kLengthPort = 0x7FD;

// BLSCAN, pick, connect: the server's name comes back; BLDISC ends it.
void testScanConnectDisconnect() {
  Pair p("ble_core_connect");
  auto w = window();
  w[0] = 1;
  CHECK(p.pc->command(0x40, w) == kOk);  // SCAN: nobody advertising
  CHECK(w[1] == 0);
  CHECK(p.server->setAdvertising(true));
  w = window();
  w[0] = 1;
  CHECK(p.pc->command(0x40, w) == kOk);
  CHECK(w[1] == 1);
  CHECK(std::string(w.begin() + 2, w.begin() + 2 + 11) == "PC-1500 EMU");
  w = window();
  w[0] = 0;
  CHECK(p.pc->command(0x41, w) == kOk);  // CONNECT to entry 0
  CHECK(windowName(w) == "PC-1500 EMU");
  CHECK(p.pc->state() == "connected to PC-1500 EMU");
  CHECK(p.server->state() == "connected to PC-1500 (it connected to us)");
  w = window();
  CHECK(p.server->command(0x44, w) == kError);  // the server can't also be a client
  CHECK(p.pc->command(0x43, w) == kOk);         // DISCONNECT
  CHECK(!p.wire.connected);
  CHECK(p.pc->state() == "idle");
  w = window();
  nameSlot(w, "pc-1500 emu");
  CHECK(p.pc->command(0x42, w) == kOk);  // CONNECT_NAME, any case
  CHECK(windowName(w) == "PC-1500 EMU");
  CHECK(p.pc->command(0x43, w) == kOk);
  p.server->setName("RENAMED");  // MCONF HOSTNAME on the server's side
  w = window();
  nameSlot(w, "PC-1500 EMU");     // still advertised under its old name here
  CHECK(p.pc->command(0x42, w) == kOk);
  CHECK(windowName(w) == "RENAMED");  // but HELLO carries the new one
}

// TEXT lands in the server's console; CR is a new line, FF clears it.
void testText() {
  Pair p("ble_core_text");
  p.server->setAdvertising(true);
  auto w = window();
  nameSlot(w, "PC-1500 EMU");
  CHECK(p.pc->command(0x42, w) == kOk);
  auto sendText = [&](const std::string& t) {
    auto tw = window();
    tw[0] = static_cast<uint8_t>(t.size() >> 8);
    tw[1] = static_cast<uint8_t>(t.size());
    std::copy(t.begin(), t.end(), tw.begin() + 2);
    return p.pc->command(0x44, tw);
  };
  std::string long_(200, 'x');  // several frames at a 64-byte frame size
  CHECK(sendText("HELLO\r") == kOk);
  CHECK(sendText(long_ + "\r") == kOk);
  CHECK(p.server->consoleText() == "HELLO\n" + long_ + "\n");
  CHECK(sendText("\fAFTER\r") == kOk);
  CHECK(p.server->consoleText() == "AFTER\n");
}

// BLSAVE then BLLOAD through WRITE/READ/CLOSE, as the ROM drives them.
void testSaveLoad() {
  Pair p("ble_core_files");
  p.server->setAdvertising(true);
  auto w = window();
  nameSlot(w, "PC-1500 EMU");
  CHECK(p.pc->command(0x42, w) == kOk);

  std::vector<uint8_t> data(700);
  for (size_t i = 0; i < data.size(); i++) data[i] = static_cast<uint8_t>(i * 7);
  auto put = [&](const std::string& name, uint8_t flags) {
    auto pw = window();
    nameSlot(pw, name);
    pw[42] = 0;  // kind: BASIC
    pw[43] = flags;
    pw[44] = pw[45] = pw[46] = pw[47] = 0xFF;  // size unknown
    uint8_t s = p.pc->command(0x45, pw);
    return std::make_pair(s, pw[0]);
  };
  CHECK(put("T", 0).first == kOk);
  auto dw = window();
  std::copy(data.begin(), data.end(), dw.begin());
  dw[kLengthPort] = static_cast<uint8_t>(data.size() >> 8);
  dw[kLengthPort + 1] = static_cast<uint8_t>(data.size());
  CHECK(p.pc->write(dw) == kOk);
  CHECK(p.pc->close() == kOk);
  std::ifstream in(p.dir[1] / "T", std::ios::binary);
  std::vector<uint8_t> saved((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK(saved == data);

  auto again = put("T", 0);
  CHECK(again.first == kError && again.second == 4);  // EXISTS
  CHECK(put("T", 1).first == kOk);                     // overwrite
  CHECK(p.pc->close() == kOk);                         // an empty file now
  std::ofstream(p.dir[1] / "T", std::ios::binary).write(reinterpret_cast<const char*>(data.data()),
                                                        static_cast<std::streamsize>(data.size()));

  auto gw = window();
  nameSlot(gw, "T");
  CHECK(p.pc->command(0x46, gw) == kOk);
  std::vector<uint8_t> loaded;
  for (;;) {
    auto rw = window();
    rw[kLengthPort] = 0x04;  // 1024
    rw[kLengthPort + 1] = 0x00;
    CHECK(p.pc->read(rw) == kOk);
    size_t n = static_cast<size_t>(rw[kLengthPort] << 8 | rw[kLengthPort + 1]);
    if (n == 0) break;
    loaded.insert(loaded.end(), rw.begin(), rw.begin() + static_cast<long>(n));
  }
  CHECK(p.pc->close() == kOk);
  CHECK(loaded == data);

  gw = window();
  nameSlot(gw, "NONE");
  CHECK(p.pc->command(0x46, gw) == kError && gw[0] == 3);  // NOT_FOUND
  gw = window();
  nameSlot(gw, "A/B");
  CHECK(put("A/B", 0).second == 5);  // IO: not a plain file name
}

// A server that stops answering: the request times out, the link drops,
// and the save fails (the ROM then raises ERROR 40).
void testPeerGoesQuiet() {
  Pair p("ble_core_quiet");
  p.server->setAdvertising(true);
  auto w = window();
  nameSlot(w, "PC-1500 EMU");
  CHECK(p.pc->command(0x42, w) == kOk);
  auto pw = window();
  nameSlot(pw, "Q");
  pw[44] = pw[45] = pw[46] = pw[47] = 0xFF;
  CHECK(p.pc->command(0x45, pw) == kOk);
  p.wire.mute[1] = true;
  auto dw = window();
  dw[kLengthPort + 1] = 10;
  CHECK(p.pc->write(dw) == kError);
  CHECK(p.pc->close() == kError);
  CHECK(!p.wire.connected);
  CHECK(!fs::exists(p.dir[1] / "Q"));
}

// Peer-to-peer (2026-09-28, BLE_PROTOCOL.md "Peer-to-peer files"), both
// cores playing PC-1500s: BLADV, then offers and answers each way.
constexpr uint8_t kLinked = 0x01, kAdvertising = 0x02, kOfferIn = 0x04, kAnswered = 0x08, kAccepted = 0x10;

// STATUS's flags once all of `flags` are set (offers arrive on the other
// core's server thread), or whatever they are after a second.
uint8_t waitStatus(LinkCore& core, uint8_t flags, bool clear = false) {
  uint8_t s = 0;
  for (int i = 0; i < 100; i++) {
    auto w = window();
    core.command(0x48, w);
    s = w[0];
    if (clear ? (s & flags) == 0 : (s & flags) == flags) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return s;
}

uint8_t offerFrom(LinkCore& core, const std::string& name, uint8_t kind, uint32_t size, uint8_t* err = nullptr) {
  auto w = window();
  nameSlot(w, name);
  w[42] = kind;
  for (int i = 0; i < 4; i++) w[44 + i] = static_cast<uint8_t>(size >> (24 - 8 * i));
  uint8_t s = core.command(0x49, w);
  if (err) *err = w[0];
  return s;
}

uint8_t answerWith(LinkCore& core, bool accept, bool routed) {
  auto w = window();
  w[0] = accept;
  w[1] = routed;
  return core.command(0x4C, w);
}

void testPeerToPeer() {
  Pair p("ble_core_p2p");
  auto w = window();
  w[0] = 1;
  CHECK(p.server->command(0x47, w) == kOk);  // BLADV
  CHECK(waitStatus(*p.server, kAdvertising) & kAdvertising);
  w = window();
  nameSlot(w, "PC-1500 EMU");
  CHECK(p.pc->command(0x42, w) == kOk);  // the other PC-1500's BLCON
  CHECK(waitStatus(*p.server, kLinked) & kLinked);
  w = window();
  p.server->command(0x48, w);
  CHECK(std::string(w.begin() + 2, w.begin() + 2 + w[1]) == "PC-1500");
  CHECK(!(w[0] & kAdvertising));

  // BLPUT SD on one side, BLGET "name" on the other: unrouted both ends.
  std::vector<uint8_t> data(700);
  for (size_t i = 0; i < data.size(); i++) data[i] = static_cast<uint8_t>(i * 5 + 1);
  CHECK(offerFrom(*p.pc, "F.BIN", 1, 700) == kOk);
  CHECK(waitStatus(*p.server, kOfferIn) & kOfferIn);
  auto gw = window();
  CHECK(p.server->command(0x4B, gw) == kOk);  // OFFER_GET
  CHECK(std::string(gw.begin() + 2, gw.begin() + 2 + gw[1]) == "F.BIN");
  CHECK(gw[42] == 1 && gw[46] == 0x02 && gw[47] == 0xBC);  // kind M, 700 bytes
  CHECK(answerWith(*p.server, true, false) == kOk);
  CHECK((waitStatus(*p.pc, kAnswered | kAccepted) & (kAnswered | kAccepted)) == (kAnswered | kAccepted));
  w = window();
  CHECK(p.pc->command(0x4D, w) == kOk);  // SEND, unrouted
  CHECK(!p.pc->transferOpen() && !p.server->transferOpen());
  uint8_t sentClose = 0;
  std::thread sender([&] {  // each FILE_DATA waits for the receiver's ACK
    auto dw = window();
    std::copy(data.begin(), data.end(), dw.begin());
    dw[kLengthPort] = static_cast<uint8_t>(data.size() >> 8);
    dw[kLengthPort + 1] = static_cast<uint8_t>(data.size());
    p.pc->command(0x4E, dw);
    auto cw = window();
    sentClose = p.pc->command(0x50, cw);
  });
  std::vector<uint8_t> got;
  for (;;) {
    auto rw = window();
    rw[kLengthPort] = 0x04;
    if (p.server->command(0x4F, rw) != kOk) break;
    size_t n = static_cast<size_t>(rw[kLengthPort] << 8 | rw[kLengthPort + 1]);
    if (n == 0) break;
    got.insert(got.end(), rw.begin(), rw.begin() + static_cast<long>(n));
  }
  auto cw = window();
  CHECK(p.server->command(0x50, cw) == kOk);
  sender.join();
  CHECK(sentClose == kOk);
  CHECK(got == data);

  // The other way, refused.
  CHECK(offerFrom(*p.server, "", 0, 0xFFFFFFFFu) == kOk);
  CHECK(waitStatus(*p.pc, kOfferIn) & kOfferIn);
  CHECK(answerWith(*p.pc, false, true) == kOk);
  uint8_t s = waitStatus(*p.server, kAnswered);
  CHECK((s & kAnswered) && !(s & kAccepted));
  w = window();
  CHECK(p.server->command(0x4D, w) == kError);  // nothing to send

  // Withdrawn while held; and one held offer at a time.
  CHECK(offerFrom(*p.pc, "W", 0, 1) == kOk);
  CHECK(waitStatus(*p.server, kOfferIn) & kOfferIn);
  w = window();
  CHECK(p.pc->command(0x4A, w) == kOk);  // WITHDRAW
  CHECK(!(waitStatus(*p.server, kOfferIn, true) & kOfferIn));
  CHECK(offerFrom(*p.pc, "A", 0, 1) == kOk);
  CHECK(waitStatus(*p.server, kOfferIn) & kOfferIn);
  uint8_t err = 0;
  CHECK(offerFrom(*p.pc, "B", 0, 1, &err) == kError && err == 6);  // BUSY

  // A dropped link drops the offers.
  w = window();
  CHECK(p.pc->command(0x43, w) == kOk);
  CHECK(!(waitStatus(*p.server, kOfferIn | kLinked, true) & (kOfferIn | kLinked)));
}

// Peer messaging (2026-09-29, BLE_PROTOCOL.md "Peer messaging"): MSGs into
// the other side's 8-message inbox, in either role.
uint8_t msgSend(LinkCore& core, const std::vector<uint8_t>& chunks, uint8_t* err = nullptr) {
  auto w = window();
  w[0] = static_cast<uint8_t>(chunks.size() >> 8);
  w[1] = static_cast<uint8_t>(chunks.size());
  std::copy(chunks.begin(), chunks.end(), w.begin() + 2);
  uint8_t s = core.command(0x51, w);
  if (err) *err = w[0];
  return s;
}

std::pair<int, int> msgCount(LinkCore& core) {  // waiting, linked
  auto w = window();
  core.command(0x54, w);
  return {w[0], w[1]};
}

void testMessages() {
  Pair p("ble_core_msg");
  p.server->setAdvertising(true);
  auto w = window();
  nameSlot(w, "PC-1500 EMU");
  CHECK(p.pc->command(0x42, w) == kOk);
  CHECK(waitStatus(*p.server, kLinked) & kLinked);

  const std::vector<uint8_t> hi = {'S', 2, 'H', 'I'};
  CHECK(msgSend(*p.pc, hi) == kOk);  // ACKed once stored
  CHECK(msgCount(*p.server) == std::make_pair(1, 1));
  w = window();
  CHECK(p.server->command(0x53, w) == kOk);  // MSG_RECV
  CHECK(w[0] == 0 && w[1] == 4 && std::vector<uint8_t>(w.begin() + 2, w.begin() + 6) == hi);
  w = window();  // MSG_WAIT 0 s: an empty inbox is "time's up" at once
  CHECK(p.server->command(0x52, w) == kOk);
  CHECK(p.server->command(0x53, w) == kError && w[0] == 1);
  w = window();
  w[0] = w[1] = 0xFF;  // for ever: "keep waiting"
  p.server->command(0x52, w);
  CHECK(p.server->command(0x53, w) == kError && w[0] == 0);

  // The other way, up to a full inbox.
  for (int i = 0; i < 8; i++) CHECK(msgSend(*p.server, hi) == kOk);
  uint8_t err = 0;
  CHECK(msgSend(*p.server, hi, &err) == kError && err == 6);  // BUSY
  CHECK(msgCount(*p.pc).first == 8);
  CHECK(msgSend(*p.server, {'X'}, &err) == kError && err == 1);  // BAD_FRAME

  // A new link empties the inbox; a dropped one keeps it.
  w = window();
  CHECK(p.pc->command(0x43, w) == kOk);
  CHECK(msgCount(*p.pc) == std::make_pair(8, 0));
  w = window();
  nameSlot(w, "PC-1500 EMU");
  CHECK(p.pc->command(0x42, w) == kOk);
  CHECK(msgCount(*p.pc) == std::make_pair(0, 1));
}

int main() {
  testScanConnectDisconnect();
  testText();
  testSaveLoad();
  testPeerGoesQuiet();
  testPeerToPeer();
  testMessages();
  if (g_failures == 0) {
    std::printf("All tests passed.\n");
    return 0;
  }
  std::printf("%d test(s) failed.\n", g_failures);
  return 1;
}
