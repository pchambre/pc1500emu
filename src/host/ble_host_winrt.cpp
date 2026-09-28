// Host Bluetooth on Windows (2026-09-28): a pc1500::ble::Transport over
// C++/WinRT -- see ble_host.h and ble_link_core.h. MSVC only.
//
// Every WinRT call runs on one private thread in the multithreaded
// apartment (Strand), so a blocking .get() is legal whichever thread asks
// -- the UI thread included. WinRT's own events (advertisements, writes to
// RX, TX values, subscriptions, disconnects) arrive on its thread pool and
// go straight to LinkCore, which is thread-safe.
//
// Lessons carried over from the laptop app's plugin (ble_app/third_party/
// ble_peripheral/PATCHES.md): a write WITHOUT response must not be
// Respond()ed (it throws); every notification's result is checked; and
// Windows advertises under the computer's name, so peers are matched on
// the service UUID.
#include "ble_host.h"

#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <thread>

using namespace winrt;
using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Bluetooth::Advertisement;
using namespace winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace winrt::Windows::Devices::Radios;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Storage::Streams;

namespace pc1500host {
namespace {

// RP2350/BLE_PROTOCOL.md sec.2
const guid kService{0xc31f0001, 0x92a3, 0x40ab, {0xb6, 0x3d, 0x7c, 0xdb, 0x0a, 0x37, 0xae, 0xd0}};
const guid kRx{0xc31f0002, 0x92a3, 0x40ab, {0xb6, 0x3d, 0x7c, 0xdb, 0x0a, 0x37, 0xae, 0xd0}};
const guid kTx{0xc31f0003, 0x92a3, 0x40ab, {0xb6, 0x3d, 0x7c, 0xdb, 0x0a, 0x37, 0xae, 0xd0}};
constexpr size_t kMaxFrame = 512;

IBuffer toBuffer(const std::vector<uint8_t>& bytes) {
  DataWriter writer;
  writer.WriteBytes(array_view<const uint8_t>(bytes.data(), bytes.data() + bytes.size()));
  return writer.DetachBuffer();
}

std::vector<uint8_t> fromBuffer(const IBuffer& buffer) {
  std::vector<uint8_t> bytes(buffer.Length());
  DataReader::FromBuffer(buffer).ReadBytes(bytes);
  return bytes;
}

std::string hexAddress(uint64_t a) {
  char s[18];
  std::snprintf(s, sizeof s, "%02X:%02X:%02X:%02X:%02X:%02X", unsigned(a >> 40 & 0xFF), unsigned(a >> 32 & 0xFF),
                unsigned(a >> 24 & 0xFF), unsigned(a >> 16 & 0xFF), unsigned(a >> 8 & 0xFF), unsigned(a & 0xFF));
  return s;
}

// One MTA thread that runs every WinRT call.
class Strand {
 public:
  Strand() : thread_([this] { loop(); }) {}
  ~Strand() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
  }
  template <class F>
  auto run(F f) -> decltype(f()) {
    auto task = std::make_shared<std::packaged_task<decltype(f())()>>(std::move(f));
    auto result = task->get_future();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      jobs_.push_back([task] { (*task)(); });
    }
    cv_.notify_all();
    return result.get();
  }

 private:
  void loop() {
    init_apartment(apartment_type::multi_threaded);
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
      cv_.wait(lock, [&] { return stop_ || !jobs_.empty(); });
      if (stop_ && jobs_.empty()) break;
      auto job = std::move(jobs_.front());
      jobs_.pop_front();
      lock.unlock();
      job();
      lock.lock();
    }
    uninit_apartment();
  }
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> jobs_;
  bool stop_ = false;
  std::thread thread_;
};

class WinRtTransport : public pc1500::ble::Transport {
 public:
  WinRtTransport() {
    strand_.run([this] { probe(); });
  }

  ~WinRtTransport() override {
    strand_.run([this] {
      stopAdvertisingLocked();
      closeCentralLocked();
    });
  }

  std::string problem() override {
    std::lock_guard<std::mutex> lock(mutex_);
    return problem_;
  }
  std::string describe() override {
    std::lock_guard<std::mutex> lock(mutex_);
    return describe_;
  }

  std::vector<pc1500::ble::Peer> scan(int ms) override;
  bool connect(const pc1500::ble::Peer& peer) override;
  bool advertise(bool on, const std::string& name) override;
  bool send(const std::vector<uint8_t>& frame) override;
  void disconnect() override;
  size_t frameMax() override {
    std::lock_guard<std::mutex> lock(mutex_);
    return frameMax_;
  }

 private:
  void probe();
  void say(const std::string& line) {
    if (onLog) onLog(line);
  }
  void stopAdvertisingLocked();
  void closeCentralLocked();

  Strand strand_;
  std::mutex mutex_;
  std::string problem_, describe_;
  size_t frameMax_ = 20;

  // advertiser
  GattServiceProvider provider_{nullptr};
  GattLocalCharacteristic rxLocal_{nullptr}, txLocal_{nullptr};
  std::atomic<bool> serving_{false};  // a connector is subscribed to TX (set on WinRT's threads)

  // connector
  BluetoothLEDevice device_{nullptr};
  GattSession session_{nullptr};
  GattDeviceService service_{nullptr};
  GattCharacteristic rx_{nullptr}, tx_{nullptr};
  event_token txChanged_{}, statusChanged_{};
  std::map<std::string, std::pair<uint64_t, BluetoothAddressType>> addresses_;
};

void WinRtTransport::probe() {
  std::string problem, line = "Windows: ";
  try {
    BluetoothAdapter adapter = BluetoothAdapter::GetDefaultAsync().get();
    if (!adapter) {
      problem = "No Bluetooth adapter was found.";
    } else {
      Radio radio = adapter.GetRadioAsync().get();
      line += std::string("LE ") + (adapter.IsLowEnergySupported() ? "yes" : "no") + ", connect (central) " +
              (adapter.IsCentralRoleSupported() ? "yes" : "no") + ", advertise (peripheral) " +
              (adapter.IsPeripheralRoleSupported() ? "yes" : "no");
      if (!adapter.IsLowEnergySupported())
        problem = "The Bluetooth adapter doesn't support Bluetooth Low Energy.";
      else if (radio && radio.State() != RadioState::On)
        problem = "Bluetooth is turned off (Windows Settings > Bluetooth & devices).";
    }
  } catch (const hresult_error& e) {
    problem = "Bluetooth isn't available: " + to_string(e.message());
  }
  std::lock_guard<std::mutex> lock(mutex_);
  problem_ = problem;
  describe_ = problem.empty() ? line : line + (line.size() > 9 ? " -- " : "") + problem;
}

// ---- connector ----

std::vector<pc1500::ble::Peer> WinRtTransport::scan(int ms) {
  return strand_.run([&] {
    struct Seen {
      pc1500::ble::Peer peer;
      bool link = false;
    };
    std::mutex seenMutex;
    std::map<uint64_t, Seen> seen;
    std::map<uint64_t, BluetoothAddressType> types;
    BluetoothLEAdvertisementWatcher watcher;
    watcher.ScanningMode(BluetoothLEScanningMode::Active);  // names often come in the scan response
    auto token = watcher.Received([&](auto const&, BluetoothLEAdvertisementReceivedEventArgs const& args) {
      std::lock_guard<std::mutex> lock(seenMutex);
      Seen& s = seen[args.BluetoothAddress()];
      s.peer.address = hexAddress(args.BluetoothAddress());
      s.peer.rssi = args.RawSignalStrengthInDBm();
      types[args.BluetoothAddress()] = args.BluetoothAddressType();
      std::string name = to_string(args.Advertisement().LocalName());
      if (!name.empty()) s.peer.name = name;
      for (guid const& g : args.Advertisement().ServiceUuids())
        if (g == kService) s.link = true;
    });
    watcher.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    watcher.Stop();
    watcher.Received(token);
    std::vector<pc1500::ble::Peer> out;
    std::lock_guard<std::mutex> lock(seenMutex);
    std::lock_guard<std::mutex> lock2(mutex_);
    for (auto& [address, s] : seen) {
      if (!s.link) continue;
      addresses_[s.peer.address] = {address, types[address]};
      out.push_back(s.peer);
    }
    return out;
  });
}

bool WinRtTransport::connect(const pc1500::ble::Peer& peer) {
  bool ok = strand_.run([&] {
    closeCentralLocked();
    std::pair<uint64_t, BluetoothAddressType> address;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = addresses_.find(peer.address);
      if (it == addresses_.end()) return false;
      address = it->second;
    }
    try {
      device_ = BluetoothLEDevice::FromBluetoothAddressAsync(address.first, address.second).get();
      if (!device_) {
        say("Couldn't open " + peer.address);
        return false;
      }
      session_ = GattSession::FromDeviceIdAsync(device_.BluetoothDeviceId()).get();
      session_.MaintainConnection(true);
      auto services = device_.GetGattServicesForUuidAsync(kService, BluetoothCacheMode::Uncached).get();
      if (services.Status() != GattCommunicationStatus::Success || services.Services().Size() == 0) {
        say("The peer doesn't offer the Link service");
        return false;
      }
      service_ = services.Services().GetAt(0);
      auto rx = service_.GetCharacteristicsForUuidAsync(kRx, BluetoothCacheMode::Uncached).get();
      auto tx = service_.GetCharacteristicsForUuidAsync(kTx, BluetoothCacheMode::Uncached).get();
      if (rx.Characteristics().Size() == 0 || tx.Characteristics().Size() == 0) {
        say("The Link service is incomplete");
        return false;
      }
      rx_ = rx.Characteristics().GetAt(0);
      tx_ = tx.Characteristics().GetAt(0);
      txChanged_ = tx_.ValueChanged([this](auto const&, GattValueChangedEventArgs const& args) {
        if (onFrame) onFrame(fromBuffer(args.CharacteristicValue()));
      });
      statusChanged_ = device_.ConnectionStatusChanged([this](BluetoothLEDevice const& d, auto const&) {
        if (d.ConnectionStatus() == BluetoothConnectionStatus::Disconnected && onLink) onLink(false, false);
      });
      auto cccd = tx_.WriteClientCharacteristicConfigurationDescriptorAsync(
                         GattClientCharacteristicConfigurationDescriptorValue::Notify)
                      .get();
      if (cccd != GattCommunicationStatus::Success) {
        say("Couldn't subscribe to the peer's TX");
        return false;
      }
      std::lock_guard<std::mutex> lock(mutex_);
      frameMax_ = std::min<size_t>(session_.MaxPduSize() - 3u, kMaxFrame);
      return true;
    } catch (const hresult_error& e) {
      say("Connect failed: " + to_string(e.message()));
      return false;
    }
  });
  if (ok && onLink) onLink(true, false);
  if (!ok) strand_.run([this] { closeCentralLocked(); });
  return ok;
}

void WinRtTransport::closeCentralLocked() {
  try {
    if (tx_) tx_.ValueChanged(txChanged_);
    if (device_) device_.ConnectionStatusChanged(statusChanged_);
    if (session_) session_.MaintainConnection(false);
    if (service_) service_.Close();
    if (session_) session_.Close();
    if (device_) device_.Close();
  } catch (const hresult_error&) {
  }
  rx_ = tx_ = nullptr;
  service_ = nullptr;
  session_ = nullptr;
  device_ = nullptr;
}

// ---- advertiser ----

bool WinRtTransport::advertise(bool on, const std::string&) {
  // The name goes in HELLO: Windows advertises under the computer's name.
  return strand_.run([&] {
    if (!on) {
      stopAdvertisingLocked();
      return true;
    }
    if (provider_) return true;
    try {
      auto result = GattServiceProvider::CreateAsync(kService).get();
      if (result.Error() != BluetoothError::Success) {
        say("Couldn't create the Link service (error " + std::to_string(int(result.Error())) + ")");
        return false;
      }
      provider_ = result.ServiceProvider();

      GattLocalCharacteristicParameters rxParams;
      rxParams.CharacteristicProperties(GattCharacteristicProperties::Write |
                                        GattCharacteristicProperties::WriteWithoutResponse);
      rxParams.WriteProtectionLevel(GattProtectionLevel::Plain);
      rxLocal_ = provider_.Service().CreateCharacteristicAsync(kRx, rxParams).get().Characteristic();
      rxLocal_.WriteRequested([this](GattLocalCharacteristic const&, GattWriteRequestedEventArgs const& args) {
        auto deferral = args.GetDeferral();
        try {
          GattWriteRequest request = args.GetRequestAsync().get();
          if (request) {
            if (onFrame) onFrame(fromBuffer(request.Value()));
            if (request.Option() == GattWriteOption::WriteWithResponse) request.Respond();
          }
        } catch (const hresult_error& e) {
          say("Receive failed: " + to_string(e.message()));
        }
        deferral.Complete();
      });

      GattLocalCharacteristicParameters txParams;
      txParams.CharacteristicProperties(GattCharacteristicProperties::Notify);
      txParams.ReadProtectionLevel(GattProtectionLevel::Plain);
      txLocal_ = provider_.Service().CreateCharacteristicAsync(kTx, txParams).get().Characteristic();
      txLocal_.SubscribedClientsChanged([this](GattLocalCharacteristic const& c, auto const&) {
        auto clients = c.SubscribedClients();
        bool now = clients.Size() > 0;
        if (now) {
          std::lock_guard<std::mutex> lock(mutex_);
          frameMax_ = std::min<size_t>(clients.GetAt(0).Session().MaxPduSize() - 3u, kMaxFrame);
        }
        if (now != serving_) {
          serving_ = now;
          if (onLink) onLink(now, true);
        }
      });

      GattServiceProviderAdvertisingParameters params;
      params.IsConnectable(true);
      params.IsDiscoverable(true);
      provider_.StartAdvertising(params);
      return true;
    } catch (const hresult_error& e) {
      say("Couldn't advertise: " + to_string(e.message()));
      stopAdvertisingLocked();
      return false;
    }
  });
}

void WinRtTransport::stopAdvertisingLocked() {
  try {
    if (provider_ && provider_.AdvertisementStatus() == GattServiceProviderAdvertisementStatus::Started)
      provider_.StopAdvertising();
  } catch (const hresult_error&) {
  }
  provider_ = nullptr;
  rxLocal_ = txLocal_ = nullptr;
  if (serving_) {
    serving_ = false;
    if (onLink) onLink(false, false);
  }
}

// ---- both ----

bool WinRtTransport::send(const std::vector<uint8_t>& frame) {
  return strand_.run([&] {
    try {
      if (rx_) {  // connector: write RX without response
        return rx_.WriteValueWithResultAsync(toBuffer(frame), GattWriteOption::WriteWithoutResponse).get().Status() ==
               GattCommunicationStatus::Success;
      }
      if (txLocal_ && serving_) {  // advertiser: notify TX
        auto results = txLocal_.NotifyValueAsync(toBuffer(frame)).get();
        bool delivered = results.Size() > 0;
        for (auto const& r : results)
          if (r.Status() != GattCommunicationStatus::Success) delivered = false;
        if (!delivered) say("A notification wasn't delivered");
        return delivered;
      }
    } catch (const hresult_error& e) {
      say("Send failed: " + to_string(e.message()));
    }
    return false;
  });
}

void WinRtTransport::disconnect() {
  bool wasCentral = strand_.run([this] {
    bool central = static_cast<bool>(device_);
    closeCentralLocked();
    return central;
  });
  // As the advertiser there's no call to drop one client; the connector
  // times out, or disconnects itself (BYE).
  if (wasCentral && onLink) onLink(false, false);
}

}  // namespace

std::unique_ptr<pc1500::ble::Transport> createHostBleTransport(std::string* why) {
  try {
    auto t = std::make_unique<WinRtTransport>();
    if (std::string p = t->problem(); !p.empty()) *why = p;
    return t;  // even with a problem: the window shows it, and it may clear (radio switched on)
  } catch (const hresult_error& e) {
    *why = "Bluetooth isn't available: " + to_string(e.message());
    return nullptr;
  }
}

}  // namespace pc1500host
