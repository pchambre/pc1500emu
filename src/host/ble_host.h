#pragma once

#include <memory>
#include <string>

#include "ble_link_core.h"

namespace pc1500host {

// This platform's Bluetooth Transport for the expansion board's BLE link
// (2026-09-28), or null with the reason in *why. Exactly one implementation
// is built (src/host/CMakeLists.txt):
// - ble_host_winrt.cpp  Windows, MSVC (C++/WinRT)
// - ble_host_macos.mm   macOS (CoreBluetooth)
// - ble_host_bluez.cpp  Linux with libsystemd (BlueZ over D-Bus, sd-bus)
// - ble_host_none.cpp   anything else, e.g. the MinGW Windows build
std::unique_ptr<pc1500::ble::Transport> createHostBleTransport(std::string* why);

}  // namespace pc1500host
