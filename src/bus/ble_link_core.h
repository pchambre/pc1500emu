#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ble_backend.h"
#include "plot_paper.h"

namespace pc1500::ble {

// The PC-1500 Link (RP2350/BLE_PROTOCOL.md) over a real Bluetooth adapter
// (2026-09-28). Portable: the protocol lives here, in LinkCore, and each
// platform supplies only a Transport (WinRT, CoreBluetooth, BlueZ) that
// moves frames. A loopback Transport pairs two LinkCores in the tests.

// A device advertising the Link service, from Transport::scan().
struct Peer {
  std::string address;  // the platform's own handle for it
  std::string name;     // advertised local name; may be empty
  int rssi = 0;
};

// Moves Link frames over one platform's Bluetooth. Every call may block;
// LinkCore calls them from the emulator's per-command worker threads and
// its own server thread, never from the UI thread. One link at a time.
class Transport {
 public:
  virtual ~Transport() = default;

  // Empty when this build and adapter can do BLE; otherwise the reason, in
  // words a user can act on ("Bluetooth is off", "built without sdbus-c++").
  virtual std::string problem() = 0;
  // A line for the Bluetooth window: platform, adapter, what it supports.
  virtual std::string describe() = 0;

  // Connector (central): Link-service advertisers found within `ms`, each
  // once, matched on the service UUID -- never on the name, which Windows
  // replaces with the computer's.
  virtual std::vector<Peer> scan(int ms) = 0;
  // Connects, finds the Link service, subscribes to TX; true once frames
  // can flow both ways.
  virtual bool connect(const Peer& peer) = 0;

  // Advertiser (peripheral): host the Link service and advertise it (or
  // stop). A connector subscribing to TX is a link (onLink).
  virtual bool advertise(bool on, const std::string& name) = 0;

  // One frame to the other side: a write without response to RX as the
  // connector, a TX notification as the advertiser. False if it can't go.
  virtual bool send(const std::vector<uint8_t>& frame) = 0;
  virtual void disconnect() = 0;
  // The largest frame the link carries: ATT MTU - 3.
  virtual size_t frameMax() = 0;

  // Set by LinkCore before anything else; called from any thread.
  std::function<void(const std::vector<uint8_t>& frame)> onFrame;
  std::function<void(bool connected, bool asAdvertiser)> onLink;
  std::function<void(const std::string& line)> onLog;
};

// The keywords' side of the link (a BleBackend), in both roles:
// - connector: BLSCAN/BLCON/BLPRINT/BLSAVE/BLLOAD from the emulated
//   PC-1500, exactly as the firmware's ble_link.c does them;
// - advertiser: another device (a real PC-1500) connects to this emulator
//   and uses it as a feature server, as the laptop app is used -- console
//   text for the UI, files in `filesDir`.
class LinkCore : public BleBackend {
 public:
  LinkCore(std::unique_ptr<Transport> transport, std::filesystem::path filesDir, std::string name);
  ~LinkCore() override;

  uint8_t command(uint8_t cmd, std::vector<uint8_t>& window) override;
  // Only a routed transfer takes over the SD file commands (pc_exp.h).
  bool transferOpen() const override { return xfer_ != Xfer::kNone && routed_; }
  uint8_t write(std::vector<uint8_t>& window) override;
  uint8_t read(std::vector<uint8_t>& window) override;
  uint8_t close() override;
  void setName(const std::string& name) override;

  // The UI's controls and views.
  bool setAdvertising(bool on);
  bool advertising() const { return advertising_; }
  std::string problem() { return transport_->problem(); }
  std::string describe() { return transport_->describe(); }
  std::string state() const;             // "idle", "advertising", "connected to X"
  std::string takeConsoleText();         // text received as a server, since last taken
  std::string consoleText() const;       // everything since the last FF
  // What a connected PC-1500's CE-150 stand-in drew here (PLOT frames).
  PlotPaper& paper() { return paper_; }
  std::vector<std::string> logLines() const;

  // Shortened for the tests (BLE_PROTOCOL.md sec.4 says 5 s).
  void setAnswerTimeoutMs(int ms) { answerTimeoutMs_ = ms; }

 private:
  enum class Xfer { kNone, kPut, kGet };
  struct Frame {
    uint8_t type = 0, seq = 0;
    std::vector<uint8_t> payload;
  };

  void onFrame(const std::vector<uint8_t>& frame);
  void onLink(bool connected, bool asAdvertiser);
  void log(const std::string& line);
  bool isLinked() const;

  bool sendFrame(uint8_t type, uint8_t seq, const std::vector<uint8_t>& payload);
  int request(uint8_t type, const std::vector<uint8_t>& payload);
  bool receive(Frame* out);
  void answer(uint8_t seq, uint8_t error = 0);
  std::vector<uint8_t> hello();
  void dropLink(const std::string& why);

  // connector
  uint8_t scan(std::vector<uint8_t>& w);
  uint8_t connectTo(const Peer& peer, std::vector<uint8_t>& w);
  void disconnectLink();
  uint8_t text(std::vector<uint8_t>& w);
  uint8_t filePut(std::vector<uint8_t>& w);
  uint8_t fileGet(std::vector<uint8_t>& w);

  // peer-to-peer (2026-09-28, BLE_PROTOCOL.md "Peer-to-peer files")
  uint8_t status(std::vector<uint8_t>& w);
  uint8_t offer(std::vector<uint8_t>& w);
  uint8_t offerGet(std::vector<uint8_t>& w);
  uint8_t answerOffer(std::vector<uint8_t>& w);
  uint8_t sendAccepted(std::vector<uint8_t>& w);

  // advertiser (server), and the frames that may come in either role while
  // no command is waiting for them (offers, their answers)
  void serverLoop();
  void serve(const Frame& f);
  void serveFileGet(const Frame& f);

  std::unique_ptr<Transport> transport_;
  std::filesystem::path filesDir_;
  std::string name_;  // under mutex_: MCONF HOSTNAME may change it at any time
  int answerTimeoutMs_ = 5000;

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool linked_ = false;
  bool asServer_ = false;
  bool advertising_ = false;
  std::string peerName_;
  std::vector<Peer> listed_;
  uint8_t txSeq_ = 0;
  // the two receive slots (BLE_PROTOCOL.md sec.4): the answer to our frame
  // in flight, and the peer's own frames -- queued for the server thread
  bool answerReady_ = false;
  uint8_t answerType_ = 0, answerSeq_ = 0, answerCode_ = 0;
  std::deque<Frame> incoming_;  // for the command waiting (receive())
  std::deque<Frame> toServe_;   // for the server thread (serve())
  bool helloDone_ = false;      // HELLOs exchanged: STATUS's "linked"

  // peer-to-peer: the offer the peer made us (until BLGET takes it), ours
  // (until the peer answers), as BLE_PROTOCOL.md sec.5 describes
  bool offerIn_ = false;
  uint8_t offerInKind_ = 0;
  uint32_t offerInSize_ = 0;
  std::string offerInName_;
  bool offerOut_ = false, answered_ = false, accepted_ = false;

  // peer messaging (2026-09-29): up to 8 MSGs, oldest first, for BLRECV
  std::deque<std::vector<uint8_t>> inbox_;
  bool recvHasDeadline_ = false;
  std::chrono::steady_clock::time_point recvDeadline_;

  // a transfer in progress (a routed one owns WRITE/READ/CLOSE_SD_FILE)
  Xfer xfer_ = Xfer::kNone;
  bool routed_ = true;
  bool xferFailed_ = false;
  bool getEnded_ = false;
  std::vector<uint8_t> getBuf_;
  size_t getPos_ = 0;

  // a server-side save in progress
  std::string putName_;
  uint32_t putSize_ = 0;
  std::vector<uint8_t> putData_;
  bool putActive_ = false;

  PlotPaper paper_;
  std::string console_;      // since the last FF
  std::string consoleNew_;   // not yet taken
  std::deque<std::string> log_;

  bool stopping_ = false;
  std::thread serverThread_;
};

}  // namespace pc1500::ble
