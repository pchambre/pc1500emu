// Host Bluetooth on Linux (2026-09-28): a pc1500::ble::Transport over
// BlueZ's D-Bus API, through sd-bus (libsystemd) -- see ble_host.h and
// ble_link_core.h.
//
// Written without a Linux Bluetooth machine to hand: compile-checked here,
// run-tested by the user. Needs BlueZ 5.51+ (write-without-response via
// WriteValue's "type" option) with LE peripheral support in the adapter.
//
// sd-bus isn't thread-safe, so one thread owns the connection: it runs an
// sd-event loop, and every call from elsewhere is a job it runs (woken
// through an eventfd). BlueZ calls back into this process while it
// registers our GATT application and advertisement (GetManagedObjects,
// property reads), so those two registrations are asynchronous calls --
// a blocking call there would deadlock against our own objects.
//
// As the advertiser this process exports, under /org/pc1500emu:
//   /org/pc1500emu                  ObjectManager (sd-bus's own)
//   /org/pc1500emu/service0         org.bluez.GattService1 (the Link service)
//   /org/pc1500emu/service0/char0   RX: write, write-without-response
//   /org/pc1500emu/service0/char1   TX: notify (Value changes are notifications)
//   /org/pc1500emu/adv0             org.bluez.LEAdvertisement1
#include "ble_host.h"

#include <poll.h>
#include <sys/eventfd.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pc1500host {
namespace {

// RP2350/BLE_PROTOCOL.md sec.2 (BlueZ reports UUIDs in lower case)
const char* const kService = "c31f0001-92a3-40ab-b63d-7cdb0a37aed0";
const char* const kRx = "c31f0002-92a3-40ab-b63d-7cdb0a37aed0";
const char* const kTx = "c31f0003-92a3-40ab-b63d-7cdb0a37aed0";
const char* const kAppPath = "/org/pc1500emu";
const char* const kServicePath = "/org/pc1500emu/service0";
const char* const kRxPath = "/org/pc1500emu/service0/char0";
const char* const kTxPath = "/org/pc1500emu/service0/char1";
const char* const kAdvPath = "/org/pc1500emu/adv0";
constexpr size_t kMaxFrame = 512;
constexpr uint64_t kCallTimeoutUs = 15'000'000;

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string errorText(const sd_bus_error* e, int r) {
  if (e && e->message) return e->message;
  return "error " + std::to_string(-r);
}

// One D-Bus object's properties, as far as this backend needs them.
struct Props {
  std::map<std::string, std::string> str;  // s, o
  std::map<std::string, int64_t> num;      // n, q, i, u
  std::map<std::string, bool> flag;        // b
  std::map<std::string, std::vector<std::string>> list;  // as
};
using Objects = std::map<std::string, std::map<std::string, Props>>;  // path -> interface -> props

int readVariant(sd_bus_message* m, const std::string& key, Props* p) {
  char type;
  const char* contents;
  int r = sd_bus_message_peek_type(m, &type, &contents);
  if (r < 0) return r;
  if ((r = sd_bus_message_enter_container(m, 'v', contents)) < 0) return r;
  std::string sig = contents;
  if (sig == "s" || sig == "o") {
    const char* s;
    if ((r = sd_bus_message_read_basic(m, sig[0], &s)) < 0) return r;
    p->str[key] = s;
  } else if (sig == "b") {
    int b;
    if ((r = sd_bus_message_read_basic(m, 'b', &b)) < 0) return r;
    p->flag[key] = b != 0;
  } else if (sig == "n") {
    int16_t v;
    if ((r = sd_bus_message_read_basic(m, 'n', &v)) < 0) return r;
    p->num[key] = v;
  } else if (sig == "q") {
    uint16_t v;
    if ((r = sd_bus_message_read_basic(m, 'q', &v)) < 0) return r;
    p->num[key] = v;
  } else if (sig == "as") {
    char** strv = nullptr;
    if ((r = sd_bus_message_read_strv(m, &strv)) < 0) return r;
    std::vector<std::string> v;
    for (char** s = strv; s && *s; s++) {
      v.push_back(*s);
      free(*s);
    }
    free(strv);
    p->list[key] = v;
  } else if ((r = sd_bus_message_skip(m, contents)) < 0) {
    return r;
  }
  return sd_bus_message_exit_container(m);
}

// An a{sv} of properties.
int readProps(sd_bus_message* m, Props* p) {
  int r = sd_bus_message_enter_container(m, 'a', "{sv}");
  if (r < 0) return r;
  while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
    const char* key;
    if ((r = sd_bus_message_read_basic(m, 's', &key)) < 0) return r;
    if ((r = readVariant(m, key, p)) < 0) return r;
    if ((r = sd_bus_message_exit_container(m)) < 0) return r;
  }
  if (r < 0) return r;
  return sd_bus_message_exit_container(m);
}

// GetManagedObjects' a{oa{sa{sv}}}.
int readObjects(sd_bus_message* m, Objects* out) {
  int r = sd_bus_message_enter_container(m, 'a', "{oa{sa{sv}}}");
  if (r < 0) return r;
  while ((r = sd_bus_message_enter_container(m, 'e', "oa{sa{sv}}")) > 0) {
    const char* path;
    if ((r = sd_bus_message_read_basic(m, 'o', &path)) < 0) return r;
    if ((r = sd_bus_message_enter_container(m, 'a', "{sa{sv}}")) < 0) return r;
    while ((r = sd_bus_message_enter_container(m, 'e', "sa{sv}")) > 0) {
      const char* iface;
      if ((r = sd_bus_message_read_basic(m, 's', &iface)) < 0) return r;
      if ((r = readProps(m, &(*out)[path][iface])) < 0) return r;
      if ((r = sd_bus_message_exit_container(m)) < 0) return r;
    }
    if (r < 0) return r;
    if ((r = sd_bus_message_exit_container(m)) < 0 || (r = sd_bus_message_exit_container(m)) < 0) return r;
  }
  if (r < 0) return r;
  return sd_bus_message_exit_container(m);
}

class BluezTransport : public pc1500::ble::Transport {
 public:
  BluezTransport() {
    efd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    thread_ = std::thread([this] { loop(); });
    std::unique_lock<std::mutex> lock(jobMutex_);
    jobCv_.wait(lock, [&] { return started_; });
    lock.unlock();
    if (ready_) run([this] { probe(); });
  }

  ~BluezTransport() override {
    if (ready_) {
      run([this] {
        stopAdvertisingOnLoop();
        disconnectOnLoop();
        sd_event_exit(event_, 0);
      });
    }
    thread_.join();
    if (efd_ >= 0) close(efd_);
  }

  std::string problem() override {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return problem_;
  }
  std::string describe() override {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return describe_;
  }

  std::vector<pc1500::ble::Peer> scan(int ms) override;
  bool connect(const pc1500::ble::Peer& peer) override;
  bool advertise(bool on, const std::string& name) override;
  bool send(const std::vector<uint8_t>& frame) override;
  void disconnect() override;
  size_t frameMax() override {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return frameMax_;
  }

 private:
  // ---- the loop thread ----
  void loop() {
    int r = sd_bus_open_system(&bus_);
    if (r >= 0) r = sd_event_new(&event_);
    if (r >= 0) r = sd_bus_attach_event(bus_, event_, 0);
    if (r >= 0 && efd_ >= 0) r = sd_event_add_io(event_, nullptr, efd_, EPOLLIN, &BluezTransport::onJobs, this);
    {
      std::lock_guard<std::mutex> lock(jobMutex_);
      ready_ = r >= 0 && efd_ >= 0;
      started_ = true;
    }
    if (!ready_) {
      std::lock_guard<std::mutex> lock(stateMutex_);
      problem_ = "Can't reach the system D-Bus (is BlueZ/dbus running?): error " + std::to_string(-r);
      describe_ = "Linux (BlueZ): " + problem_;
    }
    jobCv_.notify_all();
    if (ready_) sd_event_loop(event_);
    sd_bus_flush_close_unref(bus_);
    sd_event_unref(event_);
  }

  static int onJobs(sd_event_source*, int fd, uint32_t, void* self) {
    uint64_t n;
    (void)!read(fd, &n, sizeof n);
    auto* t = static_cast<BluezTransport*>(self);
    for (;;) {
      std::function<void()> job;
      {
        std::lock_guard<std::mutex> lock(t->jobMutex_);
        if (t->jobs_.empty()) break;
        job = std::move(t->jobs_.front());
        t->jobs_.pop_front();
      }
      job();
    }
    return 0;
  }

  // Runs `f` on the loop thread and waits for it.
  template <class F>
  auto run(F f) -> decltype(f()) {
    auto task = std::make_shared<std::packaged_task<decltype(f())()>>(std::move(f));
    auto result = task->get_future();
    {
      std::lock_guard<std::mutex> lock(jobMutex_);
      jobs_.push_back([task] { (*task)(); });
    }
    uint64_t one = 1;
    (void)!write(efd_, &one, sizeof one);
    return result.get();
  }

  void say(const std::string& line) {
    if (onLog) onLog(line);
  }

  // ---- on the loop thread ----
  bool objects(Objects* out) {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    int r = sd_bus_call_method(bus_, "org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", &err,
                               &reply, "");
    if (r >= 0) r = readObjects(reply, out);
    if (r < 0) say("BlueZ: " + errorText(&err, r));
    sd_bus_error_free(&err);
    sd_bus_message_unref(reply);
    return r >= 0;
  }

  // A method with no arguments and no result on a BlueZ object.
  bool call(const std::string& path, const char* iface, const char* member) {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message* msg = nullptr;
    sd_bus_message* reply = nullptr;
    int r = sd_bus_message_new_method_call(bus_, &msg, "org.bluez", path.c_str(), iface, member);
    if (r >= 0) r = sd_bus_call(bus_, msg, kCallTimeoutUs, &err, &reply);
    if (r < 0) say(std::string(member) + ": " + errorText(&err, r));
    sd_bus_error_free(&err);
    sd_bus_message_unref(msg);
    sd_bus_message_unref(reply);
    return r >= 0;
  }

  void probe() {
    Objects objs;
    std::string problem, line = "Linux (BlueZ): ";
    if (!objects(&objs)) {
      problem = "BlueZ isn't answering on D-Bus (is the bluetooth service running?).";
    } else {
      for (auto& [path, ifaces] : objs)
        if (ifaces.count("org.bluez.Adapter1")) {
          adapter_ = path;
          break;
        }
      if (adapter_.empty()) {
        problem = "No Bluetooth adapter was found.";
      } else {
        Props& a = objs[adapter_]["org.bluez.Adapter1"];
        bool powered = a.flag["Powered"];
        bool peripheral = objs[adapter_].count("org.bluez.LEAdvertisingManager1") > 0;
        bool gatt = objs[adapter_].count("org.bluez.GattManager1") > 0;
        line += adapter_ + ", powered " + (powered ? "yes" : "no") + ", advertise (peripheral) " +
                (peripheral && gatt ? "yes" : "no");
        if (!powered) problem = "Bluetooth is turned off (e.g. bluetoothctl power on).";
      }
    }
    std::lock_guard<std::mutex> lock(stateMutex_);
    problem_ = problem;
    describe_ = problem.empty() ? line : line + " -- " + problem;
  }

  // ---- connector ----
  bool startDiscovery() {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message* msg = nullptr;
    sd_bus_message* reply = nullptr;
    int r = sd_bus_message_new_method_call(bus_, &msg, "org.bluez", adapter_.c_str(), "org.bluez.Adapter1",
                                           "SetDiscoveryFilter");
    // {"Transport": "le", "UUIDs": [Link service]}
    if (r >= 0) r = sd_bus_message_open_container(msg, 'a', "{sv}");
    if (r >= 0) r = sd_bus_message_append(msg, "{sv}", "Transport", "s", "le");
    if (r >= 0) r = sd_bus_message_append(msg, "{sv}", "UUIDs", "as", 1, kService);
    if (r >= 0) r = sd_bus_message_close_container(msg);
    if (r >= 0) r = sd_bus_call(bus_, msg, kCallTimeoutUs, &err, &reply);
    if (r < 0) say("SetDiscoveryFilter: " + errorText(&err, r));
    sd_bus_error_free(&err);
    sd_bus_message_unref(msg);
    sd_bus_message_unref(reply);
    return call(adapter_, "org.bluez.Adapter1", "StartDiscovery");
  }

  bool connectOnLoop(const std::string& device);
  void disconnectOnLoop();
  static int onTxChanged(sd_bus_message* m, void* self, sd_bus_error*);
  static int onDeviceChanged(sd_bus_message* m, void* self, sd_bus_error*);

  // ---- advertiser ----
  bool advertiseOnLoop(const std::string& name);
  void stopAdvertisingOnLoop();
  static int onRegistered(sd_bus_message* m, void* self, sd_bus_error*);
  static int onRxWrite(sd_bus_message* m, void* self, sd_bus_error*);
  static int onStartNotify(sd_bus_message* m, void* self, sd_bus_error*);
  static int onStopNotify(sd_bus_message* m, void* self, sd_bus_error*);
  static int onRelease(sd_bus_message* m, void*, sd_bus_error*) { return sd_bus_reply_method_return(m, ""); }
  static int getString(sd_bus*, const char* path, const char*, const char* property, sd_bus_message* reply,
                       void* self, sd_bus_error*);
  static int getBool(sd_bus*, const char* path, const char*, const char* property, sd_bus_message* reply, void* self,
                     sd_bus_error*);
  static int getStrings(sd_bus*, const char* path, const char*, const char* property, sd_bus_message* reply,
                        void* self, sd_bus_error*);
  static int getValue(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* self,
                      sd_bus_error*);

  void linkChanged(bool connected, bool asAdvertiser) {
    if (onLink) onLink(connected, asAdvertiser);
  }

  // loop thread
  std::thread thread_;
  int efd_ = -1;
  sd_bus* bus_ = nullptr;
  sd_event* event_ = nullptr;
  std::mutex jobMutex_;
  std::condition_variable jobCv_;
  std::deque<std::function<void()>> jobs_;
  bool started_ = false, ready_ = false;

  std::mutex stateMutex_;
  std::string problem_, describe_;
  size_t frameMax_ = 20;
  std::string adapter_;

  // connector
  std::string device_, rxChar_, txChar_;
  sd_bus_slot* txSlot_ = nullptr;
  sd_bus_slot* deviceSlot_ = nullptr;

  // advertiser
  std::vector<sd_bus_slot*> objectSlots_;
  std::string advertName_;
  std::vector<uint8_t> txValue_;
  bool notifying_ = false;
  int registrations_ = 0;  // answered of the two async registrations
  bool registrationFailed_ = false;
  std::string registrationError_;
  std::mutex regMutex_;
  std::condition_variable regCv_;
};

// ---- connector ----

std::vector<pc1500::ble::Peer> BluezTransport::scan(int ms) {
  if (!ready_ || !run([this] { return !adapter_.empty() && startDiscovery(); })) return {};
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  return run([this] {
    call(adapter_, "org.bluez.Adapter1", "StopDiscovery");
    std::vector<pc1500::ble::Peer> out;
    Objects objs;
    if (!objects(&objs)) return out;
    for (auto& [path, ifaces] : objs) {
      auto it = ifaces.find("org.bluez.Device1");
      if (it == ifaces.end() || path.rfind(adapter_ + "/", 0) != 0) continue;
      Props& d = it->second;
      bool link = false;
      for (const std::string& u : d.list["UUIDs"])
        if (lower(u) == kService) link = true;
      if (!link || !d.num.count("RSSI")) continue;  // RSSI only on devices seen by this discovery
      pc1500::ble::Peer p;
      p.address = path;
      p.name = d.str.count("Alias") ? d.str["Alias"] : d.str["Name"];
      p.rssi = static_cast<int>(d.num["RSSI"]);
      out.push_back(p);
    }
    return out;
  });
}

bool BluezTransport::connect(const pc1500::ble::Peer& peer) {
  if (!ready_) return false;
  if (!run([&] { return connectOnLoop(peer.address); })) {
    run([this] { disconnectOnLoop(); });
    return false;
  }
  // BlueZ resolves the GATT database after connecting.
  for (int i = 0; i < 100; i++) {
    bool resolved = run([&] {
      Objects objs;
      if (!objects(&objs)) return false;
      if (!objs[device_]["org.bluez.Device1"].flag["ServicesResolved"]) return false;
      std::string service;
      for (auto& [path, ifaces] : objs) {
        auto it = ifaces.find("org.bluez.GattService1");
        if (it != ifaces.end() && path.rfind(device_ + "/", 0) == 0 && lower(it->second.str["UUID"]) == kService)
          service = path;
      }
      for (auto& [path, ifaces] : objs) {
        auto it = ifaces.find("org.bluez.GattCharacteristic1");
        if (it == ifaces.end() || service.empty() || it->second.str["Service"] != service) continue;
        std::string uuid = lower(it->second.str["UUID"]);
        if (uuid == kRx) rxChar_ = path;
        if (uuid == kTx) {
          txChar_ = path;
          if (it->second.num.count("MTU")) {
            std::lock_guard<std::mutex> lock(stateMutex_);
            frameMax_ = std::min<size_t>(static_cast<size_t>(it->second.num["MTU"]) - 3, kMaxFrame);
          }
        }
      }
      return !rxChar_.empty() && !txChar_.empty();
    });
    if (resolved) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  bool ok = run([this] {
    if (rxChar_.empty() || txChar_.empty()) {
      say("The peer doesn't offer the Link service");
      return false;
    }
    sd_bus_match_signal(bus_, &txSlot_, "org.bluez", txChar_.c_str(), "org.freedesktop.DBus.Properties",
                        "PropertiesChanged", &BluezTransport::onTxChanged, this);
    return call(txChar_, "org.bluez.GattCharacteristic1", "StartNotify");
  });
  if (!ok) {
    run([this] { disconnectOnLoop(); });
    return false;
  }
  linkChanged(true, false);
  return true;
}

bool BluezTransport::connectOnLoop(const std::string& device) {
  device_ = device;
  rxChar_.clear();
  txChar_.clear();
  sd_bus_match_signal(bus_, &deviceSlot_, "org.bluez", device_.c_str(), "org.freedesktop.DBus.Properties",
                      "PropertiesChanged", &BluezTransport::onDeviceChanged, this);
  return call(device_, "org.bluez.Device1", "Connect");
}

void BluezTransport::disconnectOnLoop() {
  if (txSlot_) {
    call(txChar_, "org.bluez.GattCharacteristic1", "StopNotify");
    txSlot_ = sd_bus_slot_unref(txSlot_);
  }
  deviceSlot_ = sd_bus_slot_unref(deviceSlot_);
  if (!device_.empty()) call(device_, "org.bluez.Device1", "Disconnect");
  device_.clear();
  rxChar_.clear();
  txChar_.clear();
}

// A TX value change from the peer: one frame.
int BluezTransport::onTxChanged(sd_bus_message* m, void* self, sd_bus_error*) {
  auto* t = static_cast<BluezTransport*>(self);
  const char* iface;
  if (sd_bus_message_read_basic(m, 's', &iface) < 0) return 0;
  if (sd_bus_message_enter_container(m, 'a', "{sv}") < 0) return 0;
  while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
    const char* key;
    if (sd_bus_message_read_basic(m, 's', &key) < 0) return 0;
    if (std::string(key) == "Value" && sd_bus_message_enter_container(m, 'v', "ay") >= 0) {
      const void* data;
      size_t size;
      if (sd_bus_message_read_array(m, 'y', &data, &size) >= 0 && t->onFrame) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        t->onFrame(std::vector<uint8_t>(p, p + size));
      }
      sd_bus_message_exit_container(m);
    } else {
      sd_bus_message_skip(m, "v");
    }
    sd_bus_message_exit_container(m);
  }
  return 0;
}

// The device's own properties: "Connected" false is the link going.
int BluezTransport::onDeviceChanged(sd_bus_message* m, void* self, sd_bus_error*) {
  auto* t = static_cast<BluezTransport*>(self);
  const char* iface;
  Props p;
  if (sd_bus_message_read_basic(m, 's', &iface) < 0 || readProps(m, &p) < 0) return 0;
  if (p.flag.count("Connected") && !p.flag["Connected"]) {
    t->say("The peer disconnected");
    t->linkChanged(false, false);
  }
  return 0;
}

// ---- advertiser ----

bool BluezTransport::advertiseOnLoop(const std::string& name) {
  if (!objectSlots_.empty()) return true;
  advertName_ = name;
  static const sd_bus_vtable service[] = {
      SD_BUS_VTABLE_START(0),
      SD_BUS_PROPERTY("UUID", "s", &BluezTransport::getString, 0, SD_BUS_VTABLE_PROPERTY_CONST),
      SD_BUS_PROPERTY("Primary", "b", &BluezTransport::getBool, 0, SD_BUS_VTABLE_PROPERTY_CONST),
      SD_BUS_VTABLE_END};
  static const sd_bus_vtable rx[] = {
      SD_BUS_VTABLE_START(0),
      SD_BUS_PROPERTY("UUID", "s", &BluezTransport::getString, 0, SD_BUS_VTABLE_PROPERTY_CONST),
      SD_BUS_PROPERTY("Service", "o", &BluezTransport::getString, 0, SD_BUS_VTABLE_PROPERTY_CONST),
      SD_BUS_PROPERTY("Flags", "as", &BluezTransport::getStrings, 0, SD_BUS_VTABLE_PROPERTY_CONST),
      SD_BUS_METHOD("WriteValue", "aya{sv}", "", &BluezTransport::onRxWrite, SD_BUS_VTABLE_UNPRIVILEGED),
      SD_BUS_VTABLE_END};
  static const sd_bus_vtable tx[] = {
      SD_BUS_VTABLE_START(0),
      SD_BUS_PROPERTY("UUID", "s", &BluezTransport::getString, 0, SD_BUS_VTABLE_PROPERTY_CONST),
      SD_BUS_PROPERTY("Service", "o", &BluezTransport::getString, 0, SD_BUS_VTABLE_PROPERTY_CONST),
      SD_BUS_PROPERTY("Flags", "as", &BluezTransport::getStrings, 0, SD_BUS_VTABLE_PROPERTY_CONST),
      SD_BUS_PROPERTY("Value", "ay", &BluezTransport::getValue, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
      SD_BUS_PROPERTY("Notifying", "b", &BluezTransport::getBool, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
      SD_BUS_METHOD("StartNotify", "", "", &BluezTransport::onStartNotify, SD_BUS_VTABLE_UNPRIVILEGED),
      SD_BUS_METHOD("StopNotify", "", "", &BluezTransport::onStopNotify, SD_BUS_VTABLE_UNPRIVILEGED),
      SD_BUS_VTABLE_END};
  static const sd_bus_vtable adv[] = {
      SD_BUS_VTABLE_START(0),
      SD_BUS_PROPERTY("Type", "s", &BluezTransport::getString, 0, SD_BUS_VTABLE_PROPERTY_CONST),
      SD_BUS_PROPERTY("ServiceUUIDs", "as", &BluezTransport::getStrings, 0, SD_BUS_VTABLE_PROPERTY_CONST),
      SD_BUS_PROPERTY("LocalName", "s", &BluezTransport::getString, 0, SD_BUS_VTABLE_PROPERTY_CONST),
      SD_BUS_METHOD("Release", "", "", &BluezTransport::onRelease, SD_BUS_VTABLE_UNPRIVILEGED),
      SD_BUS_VTABLE_END};

  auto add = [&](const char* path, const char* iface, const sd_bus_vtable* vt) {
    sd_bus_slot* slot = nullptr;
    int r = sd_bus_add_object_vtable(bus_, &slot, path, iface, vt, this);
    if (r >= 0) objectSlots_.push_back(slot);
    return r >= 0;
  };
  sd_bus_slot* manager = nullptr;
  bool ok = sd_bus_add_object_manager(bus_, &manager, kAppPath) >= 0;
  if (ok) objectSlots_.push_back(manager);
  ok = ok && add(kServicePath, "org.bluez.GattService1", service) &&
       add(kRxPath, "org.bluez.GattCharacteristic1", rx) && add(kTxPath, "org.bluez.GattCharacteristic1", tx) &&
       add(kAdvPath, "org.bluez.LEAdvertisement1", adv);
  if (!ok) {
    say("Couldn't export the Link service on D-Bus");
    stopAdvertisingOnLoop();
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(regMutex_);
    registrations_ = 0;
    registrationFailed_ = false;
  }
  // Both asynchronous: BlueZ reads our objects while it registers them.
  int r1 = sd_bus_call_method_async(bus_, nullptr, "org.bluez", adapter_.c_str(), "org.bluez.GattManager1",
                                    "RegisterApplication", &BluezTransport::onRegistered, this, "oa{sv}", kAppPath, 0);
  int r2 = sd_bus_call_method_async(bus_, nullptr, "org.bluez", adapter_.c_str(), "org.bluez.LEAdvertisingManager1",
                                    "RegisterAdvertisement", &BluezTransport::onRegistered, this, "oa{sv}", kAdvPath, 0);
  if (r1 < 0 || r2 < 0) {
    say("Couldn't ask BlueZ to register the Link service");
    stopAdvertisingOnLoop();
    return false;
  }
  return true;
}

int BluezTransport::onRegistered(sd_bus_message* m, void* self, sd_bus_error*) {
  auto* t = static_cast<BluezTransport*>(self);
  const sd_bus_error* e = sd_bus_message_get_error(m);
  {
    std::lock_guard<std::mutex> lock(t->regMutex_);
    t->registrations_++;
    if (e) {
      t->registrationFailed_ = true;
      t->registrationError_ = e->message ? e->message : e->name;
    }
  }
  t->regCv_.notify_all();
  return 0;
}

void BluezTransport::stopAdvertisingOnLoop() {
  if (objectSlots_.empty()) return;
  sd_bus_call_method_async(bus_, nullptr, "org.bluez", adapter_.c_str(), "org.bluez.LEAdvertisingManager1",
                           "UnregisterAdvertisement", nullptr, nullptr, "o", kAdvPath);
  sd_bus_call_method_async(bus_, nullptr, "org.bluez", adapter_.c_str(), "org.bluez.GattManager1",
                           "UnregisterApplication", nullptr, nullptr, "o", kAppPath);
  for (sd_bus_slot* s : objectSlots_) sd_bus_slot_unref(s);
  objectSlots_.clear();
  if (notifying_) {
    notifying_ = false;
    linkChanged(false, false);
  }
}

int BluezTransport::onRxWrite(sd_bus_message* m, void* self, sd_bus_error*) {
  auto* t = static_cast<BluezTransport*>(self);
  const void* data;
  size_t size;
  int r = sd_bus_message_read_array(m, 'y', &data, &size);
  if (r < 0) return r;
  std::vector<uint8_t> frame(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
  Props options;
  if (readProps(m, &options) >= 0 && options.num.count("mtu")) {
    std::lock_guard<std::mutex> lock(t->stateMutex_);
    t->frameMax_ = std::min<size_t>(static_cast<size_t>(options.num["mtu"]) - 3, kMaxFrame);
  }
  if (t->onFrame) t->onFrame(frame);
  return sd_bus_reply_method_return(m, "");
}

int BluezTransport::onStartNotify(sd_bus_message* m, void* self, sd_bus_error*) {
  auto* t = static_cast<BluezTransport*>(self);
  if (!t->notifying_) {
    t->notifying_ = true;
    sd_bus_emit_properties_changed(t->bus_, kTxPath, "org.bluez.GattCharacteristic1", "Notifying", nullptr);
    t->linkChanged(true, true);  // a connector subscribed: a link
  }
  return sd_bus_reply_method_return(m, "");
}

int BluezTransport::onStopNotify(sd_bus_message* m, void* self, sd_bus_error*) {
  auto* t = static_cast<BluezTransport*>(self);
  if (t->notifying_) {
    t->notifying_ = false;
    sd_bus_emit_properties_changed(t->bus_, kTxPath, "org.bluez.GattCharacteristic1", "Notifying", nullptr);
    t->linkChanged(false, false);
  }
  return sd_bus_reply_method_return(m, "");
}

int BluezTransport::getString(sd_bus*, const char* path, const char*, const char* property, sd_bus_message* reply,
                              void* self, sd_bus_error*) {
  auto* t = static_cast<BluezTransport*>(self);
  std::string p = path, prop = property;
  if (prop == "Service") return sd_bus_message_append_basic(reply, 'o', kServicePath);
  if (prop == "Type") return sd_bus_message_append_basic(reply, 's', "peripheral");
  if (prop == "LocalName") return sd_bus_message_append_basic(reply, 's', t->advertName_.c_str());
  const char* uuid = p == kRxPath ? kRx : p == kTxPath ? kTx : kService;
  return sd_bus_message_append_basic(reply, 's', uuid);
}

int BluezTransport::getBool(sd_bus*, const char*, const char*, const char* property, sd_bus_message* reply,
                            void* self, sd_bus_error*) {
  auto* t = static_cast<BluezTransport*>(self);
  int value = std::string(property) == "Notifying" ? t->notifying_ : 1;  // Primary: yes
  return sd_bus_message_append_basic(reply, 'b', &value);
}

int BluezTransport::getStrings(sd_bus*, const char* path, const char*, const char* property, sd_bus_message* reply,
                               void*, sd_bus_error*) {
  std::string p = path;
  if (std::string(property) == "ServiceUUIDs") return sd_bus_message_append(reply, "as", 1, kService);
  if (p == kRxPath) return sd_bus_message_append(reply, "as", 2, "write", "write-without-response");
  return sd_bus_message_append(reply, "as", 1, "notify");
}

int BluezTransport::getValue(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* self,
                             sd_bus_error*) {
  auto* t = static_cast<BluezTransport*>(self);
  return sd_bus_message_append_array(reply, 'y', t->txValue_.data(), t->txValue_.size());
}

bool BluezTransport::advertise(bool on, const std::string& name) {
  if (!ready_) return false;
  if (!on) {
    run([this] { stopAdvertisingOnLoop(); });
    return true;
  }
  if (!run([&] { return !adapter_.empty() && advertiseOnLoop(name); })) return false;
  std::unique_lock<std::mutex> lock(regMutex_);
  regCv_.wait_for(lock, std::chrono::seconds(10), [&] { return registrations_ == 2; });
  if (registrations_ == 2 && !registrationFailed_) return true;
  std::string why = registrationFailed_ ? registrationError_ : "BlueZ didn't answer";
  lock.unlock();
  say("Couldn't advertise: " + why);
  run([this] { stopAdvertisingOnLoop(); });
  return false;
}

// ---- both ----

bool BluezTransport::send(const std::vector<uint8_t>& frame) {
  if (!ready_) return false;
  return run([&] {
    if (!rxChar_.empty()) {  // connector: write RX without response
      sd_bus_error err = SD_BUS_ERROR_NULL;
      sd_bus_message* msg = nullptr;
      sd_bus_message* reply = nullptr;
      int r = sd_bus_message_new_method_call(bus_, &msg, "org.bluez", rxChar_.c_str(), "org.bluez.GattCharacteristic1",
                                             "WriteValue");
      if (r >= 0) r = sd_bus_message_append_array(msg, 'y', frame.data(), frame.size());
      if (r >= 0) r = sd_bus_message_open_container(msg, 'a', "{sv}");
      if (r >= 0) r = sd_bus_message_append(msg, "{sv}", "type", "s", "command");
      if (r >= 0) r = sd_bus_message_close_container(msg);
      if (r >= 0) r = sd_bus_call(bus_, msg, kCallTimeoutUs, &err, &reply);
      if (r < 0) say("Send failed: " + errorText(&err, r));
      sd_bus_error_free(&err);
      sd_bus_message_unref(msg);
      sd_bus_message_unref(reply);
      return r >= 0;
    }
    if (notifying_) {  // advertiser: a new TX value is a notification
      txValue_ = frame;
      return sd_bus_emit_properties_changed(bus_, kTxPath, "org.bluez.GattCharacteristic1", "Value", nullptr) >= 0;
    }
    return false;
  });
}

void BluezTransport::disconnect() {
  if (!ready_) return;
  bool wasCentral = run([this] {
    bool central = !device_.empty();
    disconnectOnLoop();
    return central;
  });
  // As the advertiser there's no call to drop one connector; it times out
  // or disconnects itself (BYE).
  if (wasCentral) linkChanged(false, false);
}

}  // namespace

std::unique_ptr<pc1500::ble::Transport> createHostBleTransport(std::string* why) {
  auto t = std::make_unique<BluezTransport>();
  if (std::string p = t->problem(); !p.empty()) *why = p;
  return t;  // even with a problem: the panel shows it, and it may clear (Bluetooth switched on)
}

}  // namespace pc1500host
