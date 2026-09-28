// Host Bluetooth on macOS (2026-09-28): a pc1500::ble::Transport over
// CoreBluetooth -- see ble_host.h and ble_link_core.h.
//
// Written without a Mac to hand: built and tested by the user on one.
//
// CoreBluetooth calls back on a private serial dispatch queue; the
// Transport's blocking calls (scan, connect, send...) wait on condition
// variables from the emulator's worker threads, never on that queue.
//
// Notes for macOS:
// - Using Bluetooth needs NSBluetoothAlwaysUsageDescription in the
//   Info.plist; the emulator is a bare executable, so CMake embeds one
//   (macos_Info.plist) into the binary's __TEXT,__info_plist section. The
//   first use asks the user for permission; a refusal shows up here as
//   "not allowed" (System Settings > Privacy & Security > Bluetooth).
// - macOS never reveals a device's Bluetooth address: peers are identified
//   by the per-Mac UUID CoreBluetooth assigns them.
// - As the advertiser, macOS advertises the name given here, but a peer
//   still learns ours from HELLO (BLE_PROTOCOL.md).
#include "ble_host.h"

#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace {

// RP2350/BLE_PROTOCOL.md sec.2
NSString* const kServiceUuid = @"c31f0001-92a3-40ab-b63d-7cdb0a37aed0";
NSString* const kRxUuid = @"c31f0002-92a3-40ab-b63d-7cdb0a37aed0";
NSString* const kTxUuid = @"c31f0003-92a3-40ab-b63d-7cdb0a37aed0";
constexpr size_t kMaxFrame = 512;
constexpr auto kStepTimeout = std::chrono::seconds(10);

std::string stateProblem(CBManagerState s) {
  switch (s) {
    case CBManagerStatePoweredOn: return "";
    case CBManagerStatePoweredOff: return "Bluetooth is turned off (System Settings > Bluetooth).";
    case CBManagerStateUnauthorized:
      return "The emulator isn't allowed to use Bluetooth (System Settings > Privacy & Security > Bluetooth).";
    case CBManagerStateUnsupported: return "This Mac doesn't support Bluetooth Low Energy.";
    case CBManagerStateResetting: return "Bluetooth is resetting; try again in a moment.";
    default: return "Bluetooth isn't ready yet.";
  }
}

std::vector<uint8_t> bytes(NSData* d) {
  const uint8_t* p = static_cast<const uint8_t*>(d.bytes);
  return std::vector<uint8_t>(p, p + d.length);
}

}  // namespace

namespace pc1500host {
class MacTransport;
}

// The one delegate for both managers and the connected peripheral.
@interface PC1500BleDelegate : NSObject <CBCentralManagerDelegate, CBPeripheralDelegate, CBPeripheralManagerDelegate>
@property(nonatomic, assign) pc1500host::MacTransport* owner;
@end

namespace pc1500host {

class MacTransport : public pc1500::ble::Transport {
 public:
  MacTransport() {
    queue_ = dispatch_queue_create("pc1500emu.ble", DISPATCH_QUEUE_SERIAL);
    delegate_ = [[PC1500BleDelegate alloc] init];
    delegate_.owner = this;
    central_ = [[CBCentralManager alloc] initWithDelegate:delegate_ queue:queue_];
    peripheralManager_ = [[CBPeripheralManager alloc] initWithDelegate:delegate_ queue:queue_];
    // The managers report their state asynchronously; give them a moment.
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, std::chrono::seconds(3), [&] { return centralState_ != CBManagerStateUnknown; });
  }

  ~MacTransport() override {
    disconnect();
    advertise(false, "");
    PC1500BleDelegate* d = delegate_;
    dispatch_sync(queue_, ^{
      d.owner = nullptr;
    });
  }

  std::string problem() override {
    std::lock_guard<std::mutex> lock(mutex_);
    return stateProblem(centralState_);
  }

  std::string describe() override {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string p = stateProblem(centralState_);
    return "macOS (CoreBluetooth): " + (p.empty() ? std::string("ready -- connect and advertise") : p);
  }

  std::vector<pc1500::ble::Peer> scan(int ms) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      found_.clear();
    }
    CBCentralManager* central = central_;
    dispatch_sync(queue_, ^{
      [central scanForPeripheralsWithServices:@[ [CBUUID UUIDWithString:kServiceUuid] ]
                                      options:@{CBCentralManagerScanOptionAllowDuplicatesKey : @NO}];
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    dispatch_sync(queue_, ^{
      [central stopScan];
    });
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<pc1500::ble::Peer> out;
    for (auto& [id, f] : found_) out.push_back(f.peer);
    return out;
  }

  bool connect(const pc1500::ble::Peer& peer) override {
    CBPeripheral* p = nil;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = found_.find(peer.address);
      if (it == found_.end()) return false;
      p = it->second.peripheral;
      ready_ = failed_ = false;
    }
    peripheral_ = p;
    CBCentralManager* central = central_;
    PC1500BleDelegate* d = delegate_;
    dispatch_sync(queue_, ^{
      p.delegate = d;
      [central connectPeripheral:p options:nil];
    });
    std::unique_lock<std::mutex> lock(mutex_);
    bool done = cv_.wait_for(lock, kStepTimeout, [&] { return ready_ || failed_; });
    if (!done || !ready_) {
      lock.unlock();
      say("Couldn't connect to " + peer.address);
      disconnect();
      return false;
    }
    lock.unlock();
    if (onLink) onLink(true, false);
    return true;
  }

  bool advertise(bool on, const std::string& name) override {
    CBPeripheralManager* manager = peripheralManager_;
    if (!on) {
      CBMutableService* service = service_;
      dispatch_sync(queue_, ^{
        [manager stopAdvertising];
        if (service) [manager removeService:service];
      });
      service_ = nil;
      rxLocal_ = txLocal_ = nil;
      return true;
    }
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait_for(lock, std::chrono::seconds(3), [&] { return peripheralState_ != CBManagerStateUnknown; });
      if (peripheralState_ != CBManagerStatePoweredOn) {
        std::string why = stateProblem(peripheralState_);
        lock.unlock();
        say("Can't advertise: " + why);
        return false;
      }
      serviceAdded_ = serviceFailed_ = false;
    }
    rxLocal_ = [[CBMutableCharacteristic alloc]
        initWithType:[CBUUID UUIDWithString:kRxUuid]
          properties:CBCharacteristicPropertyWrite | CBCharacteristicPropertyWriteWithoutResponse
               value:nil
         permissions:CBAttributePermissionsWriteable];
    txLocal_ = [[CBMutableCharacteristic alloc] initWithType:[CBUUID UUIDWithString:kTxUuid]
                                                  properties:CBCharacteristicPropertyNotify
                                                       value:nil
                                                 permissions:CBAttributePermissionsReadable];
    service_ = [[CBMutableService alloc] initWithType:[CBUUID UUIDWithString:kServiceUuid] primary:YES];
    service_.characteristics = @[ rxLocal_, txLocal_ ];
    CBMutableService* service = service_;
    dispatch_sync(queue_, ^{
      [manager addService:service];
    });
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait_for(lock, kStepTimeout, [&] { return serviceAdded_ || serviceFailed_; });
      if (!serviceAdded_) {
        lock.unlock();
        say("Couldn't add the Link service");
        return false;
      }
    }
    NSString* local = [NSString stringWithUTF8String:name.c_str()];
    dispatch_sync(queue_, ^{
      [manager startAdvertising:@{
        CBAdvertisementDataServiceUUIDsKey : @[ [CBUUID UUIDWithString:kServiceUuid] ],
        CBAdvertisementDataLocalNameKey : local
      }];
    });
    return true;
  }

  bool send(const std::vector<uint8_t>& frame) override {
    NSData* data = [NSData dataWithBytes:frame.data() length:frame.size()];
    auto deadline = std::chrono::steady_clock::now() + kStepTimeout;
    for (;;) {
      __block BOOL sent = NO;
      __block BOOL linked = NO;
      CBPeripheral* peripheral = peripheral_;
      CBCharacteristic* rx = rx_;
      CBMutableCharacteristic* tx = txLocal_;
      CBCentral* subscriber = subscriber_;
      CBPeripheralManager* manager = peripheralManager_;
      dispatch_sync(queue_, ^{
        if (peripheral && rx) {  // connector: write RX without response
          linked = YES;
          if (peripheral.canSendWriteWithoutResponse) {
            [peripheral writeValue:data forCharacteristic:rx type:CBCharacteristicWriteWithoutResponse];
            sent = YES;
          }
        } else if (tx && subscriber) {  // advertiser: notify TX
          linked = YES;
          sent = [manager updateValue:data forCharacteristic:tx onSubscribedCentrals:nil];
        }
      });
      if (sent) return true;
      if (!linked || std::chrono::steady_clock::now() > deadline) return false;
      std::unique_lock<std::mutex> lock(mutex_);  // "ready to send again" wakes this
      cv_.wait_for(lock, std::chrono::milliseconds(50));
    }
  }

  void disconnect() override {
    __block bool had = false;
    CBPeripheral* peripheral = peripheral_;
    CBCentralManager* central = central_;
    dispatch_sync(queue_, ^{
      if (peripheral) {
        had = true;
        [central cancelPeripheralConnection:peripheral];
      }
    });
    peripheral_ = nil;
    rx_ = tx_ = nil;
    // As the advertiser there's no call to drop one central; it times out
    // or disconnects itself (BYE).
    if (had && onLink) onLink(false, false);
  }

  size_t frameMax() override {
    std::lock_guard<std::mutex> lock(mutex_);
    return frameMax_;
  }

  // ---- called on the CoreBluetooth queue by the delegate ----
  void say(const std::string& line) {
    if (onLog) onLog(line);
  }
  void wake() { cv_.notify_all(); }
  void centralState(CBManagerState s) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      centralState_ = s;
    }
    wake();
  }
  void peripheralState(CBManagerState s) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      peripheralState_ = s;
    }
    wake();
  }
  void discovered(CBPeripheral* p, NSDictionary* adv, NSNumber* rssi) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string id = p.identifier.UUIDString.UTF8String;
    Found& f = found_[id];
    f.peripheral = p;
    f.peer.address = id;
    f.peer.rssi = rssi.intValue;
    NSString* name = adv[CBAdvertisementDataLocalNameKey] ?: p.name;
    if (name) f.peer.name = name.UTF8String;
  }
  void connected(CBPeripheral* p) { [p discoverServices:@[ [CBUUID UUIDWithString:kServiceUuid] ]]; }
  void failed(const std::string& why) {
    say(why);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      failed_ = true;
    }
    wake();
  }
  void servicesFound(CBPeripheral* p) {
    for (CBService* s in p.services)
      if ([s.UUID isEqual:[CBUUID UUIDWithString:kServiceUuid]]) {
        [p discoverCharacteristics:@[ [CBUUID UUIDWithString:kRxUuid], [CBUUID UUIDWithString:kTxUuid] ]
                        forService:s];
        return;
      }
    failed("The peer doesn't offer the Link service");
  }
  void characteristicsFound(CBPeripheral* p, CBService* s) {
    for (CBCharacteristic* c in s.characteristics) {
      if ([c.UUID isEqual:[CBUUID UUIDWithString:kRxUuid]]) rx_ = c;
      if ([c.UUID isEqual:[CBUUID UUIDWithString:kTxUuid]]) tx_ = c;
    }
    if (!rx_ || !tx_) return failed("The Link service is incomplete");
    [p setNotifyValue:YES forCharacteristic:tx_];
  }
  void notifying(CBPeripheral* p, NSError* error) {
    if (error) return failed("Couldn't subscribe to the peer's TX");
    {
      std::lock_guard<std::mutex> lock(mutex_);
      frameMax_ = std::min<size_t>([p maximumWriteValueLengthForType:CBCharacteristicWriteWithoutResponse], kMaxFrame);
      ready_ = true;
    }
    wake();
  }
  void value(CBCharacteristic* c) {
    if (c == tx_ && c.value && onFrame) onFrame(bytes(c.value));
  }
  void peripheralDisconnected(CBPeripheral* p) {
    if (p != peripheral_) return;
    peripheral_ = nil;
    rx_ = tx_ = nil;
    failed("Disconnected");
    if (onLink) onLink(false, false);
  }
  void serviceAdded(NSError* error) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      (error ? serviceFailed_ : serviceAdded_) = true;
    }
    wake();
  }
  void writes(NSArray<CBATTRequest*>* requests) {
    for (CBATTRequest* r in requests)
      if ([r.characteristic.UUID isEqual:[CBUUID UUIDWithString:kRxUuid]] && r.value && onFrame) onFrame(bytes(r.value));
    // One response for the whole batch -- only a write WITH response waits
    // for it; to verify on a Mac that CoreBluetooth ignores it otherwise.
    if (requests.count) [peripheralManager_ respondToRequest:requests.firstObject withResult:CBATTErrorSuccess];
  }
  void subscribed(CBCentral* c, bool on) {
    subscriber_ = on ? c : nil;
    if (on) {
      std::lock_guard<std::mutex> lock(mutex_);
      frameMax_ = std::min<size_t>(c.maximumUpdateValueLength, kMaxFrame);
    }
    if (onLink) onLink(on, on);
  }

 private:
  struct Found {
    CBPeripheral* peripheral = nil;
    pc1500::ble::Peer peer;
  };

  dispatch_queue_t queue_;
  PC1500BleDelegate* delegate_;
  CBCentralManager* central_;
  CBPeripheralManager* peripheralManager_;
  std::mutex mutex_;
  std::condition_variable cv_;
  CBManagerState centralState_ = CBManagerStateUnknown;
  CBManagerState peripheralState_ = CBManagerStateUnknown;
  std::map<std::string, Found> found_;
  size_t frameMax_ = 20;

  // connector
  CBPeripheral* peripheral_ = nil;
  CBCharacteristic* rx_ = nil;
  CBCharacteristic* tx_ = nil;
  bool ready_ = false, failed_ = false;

  // advertiser
  CBMutableService* service_ = nil;
  CBMutableCharacteristic* rxLocal_ = nil;
  CBMutableCharacteristic* txLocal_ = nil;
  CBCentral* subscriber_ = nil;
  bool serviceAdded_ = false, serviceFailed_ = false;
};

}  // namespace pc1500host

@implementation PC1500BleDelegate

- (void)centralManagerDidUpdateState:(CBCentralManager*)central {
  if (_owner) _owner->centralState(central.state);
}
- (void)centralManager:(CBCentralManager*)central
    didDiscoverPeripheral:(CBPeripheral*)peripheral
        advertisementData:(NSDictionary<NSString*, id>*)advertisementData
                     RSSI:(NSNumber*)RSSI {
  if (_owner) _owner->discovered(peripheral, advertisementData, RSSI);
}
- (void)centralManager:(CBCentralManager*)central didConnectPeripheral:(CBPeripheral*)peripheral {
  if (_owner) _owner->connected(peripheral);
}
- (void)centralManager:(CBCentralManager*)central
    didFailToConnectPeripheral:(CBPeripheral*)peripheral
                         error:(NSError*)error {
  if (_owner) _owner->failed("Connect failed");
}
- (void)centralManager:(CBCentralManager*)central
    didDisconnectPeripheral:(CBPeripheral*)peripheral
                      error:(NSError*)error {
  if (_owner) _owner->peripheralDisconnected(peripheral);
}
- (void)peripheral:(CBPeripheral*)peripheral didDiscoverServices:(NSError*)error {
  if (!_owner) return;
  if (error) _owner->failed("Service discovery failed");
  else _owner->servicesFound(peripheral);
}
- (void)peripheral:(CBPeripheral*)peripheral
    didDiscoverCharacteristicsForService:(CBService*)service
                                   error:(NSError*)error {
  if (!_owner) return;
  if (error) _owner->failed("Characteristic discovery failed");
  else _owner->characteristicsFound(peripheral, service);
}
- (void)peripheral:(CBPeripheral*)peripheral
    didUpdateNotificationStateForCharacteristic:(CBCharacteristic*)characteristic
                                          error:(NSError*)error {
  if (_owner) _owner->notifying(peripheral, error);
}
- (void)peripheral:(CBPeripheral*)peripheral
    didUpdateValueForCharacteristic:(CBCharacteristic*)characteristic
                              error:(NSError*)error {
  if (_owner && !error) _owner->value(characteristic);
}
- (void)peripheralIsReadyToSendWriteWithoutResponse:(CBPeripheral*)peripheral {
  if (_owner) _owner->wake();
}
- (void)peripheralManagerDidUpdateState:(CBPeripheralManager*)peripheral {
  if (_owner) _owner->peripheralState(peripheral.state);
}
- (void)peripheralManager:(CBPeripheralManager*)peripheral didAddService:(CBService*)service error:(NSError*)error {
  if (_owner) _owner->serviceAdded(error);
}
- (void)peripheralManager:(CBPeripheralManager*)peripheral didReceiveWriteRequests:(NSArray<CBATTRequest*>*)requests {
  if (_owner) _owner->writes(requests);
}
- (void)peripheralManager:(CBPeripheralManager*)peripheral
                         central:(CBCentral*)central
    didSubscribeToCharacteristic:(CBCharacteristic*)characteristic {
  if (_owner) _owner->subscribed(central, true);
}
- (void)peripheralManager:(CBPeripheralManager*)peripheral
                             central:(CBCentral*)central
    didUnsubscribeFromCharacteristic:(CBCharacteristic*)characteristic {
  if (_owner) _owner->subscribed(central, false);
}
- (void)peripheralManagerIsReadyToUpdateSubscribers:(CBPeripheralManager*)peripheral {
  if (_owner) _owner->wake();
}

@end

namespace pc1500host {

std::unique_ptr<pc1500::ble::Transport> createHostBleTransport(std::string* why) {
  auto t = std::make_unique<MacTransport>();
  if (std::string p = t->problem(); !p.empty()) *why = p;
  return t;  // even with a problem: the panel shows it, and it may clear (Bluetooth switched on)
}

}  // namespace pc1500host
