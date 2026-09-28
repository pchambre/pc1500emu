// No host Bluetooth in this build -- see ble_host.h. The emulator still
// runs; its expansion board keeps the fake BLE peer, and the Bluetooth
// window shows the reason below.
#include "ble_host.h"

namespace pc1500host {

std::unique_ptr<pc1500::ble::Transport> createHostBleTransport(std::string* why) {
#if defined(_WIN32)
  *why = "Host Bluetooth needs the MSVC (Visual Studio) build of the emulator; this one was built with "
         "another compiler (C++/WinRT is MSVC-only).";
#elif defined(__linux__)
  *why = "This build has no host Bluetooth: it needs libsystemd (e.g. apt install libsystemd-dev) "
         "when the emulator is built, and BlueZ at run time.";
#else
  *why = "Host Bluetooth isn't supported on this platform.";
#endif
  return nullptr;
}

}  // namespace pc1500host
