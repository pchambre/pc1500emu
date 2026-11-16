#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pc1500::ble {

// The BLE server's folders (2026-11-13, RP2350/BLE_PROTOCOL.md "Host
// folders"): a PC-1500 path -- '/' between names, from the root if it
// starts with one, else from `cwd` -- as the names below the shared
// folder's root. False for ".." above the root, or a name a host can't
// store (the characters Windows refuses, a trailing space or dot). Used by
// ExpansionMock's fake server and LinkCore's real one alike.
inline bool resolveHostPath(const std::vector<std::string>& cwd, const std::string& path,
                            std::vector<std::string>* out) {
  std::vector<std::string> names;
  if (path.empty() || path[0] != '/') names = cwd;
  size_t at = 0;
  while (at <= path.size()) {
    size_t end = path.find('/', at);
    if (end == std::string::npos) end = path.size();
    std::string part = path.substr(at, end - at);
    at = end + 1;
    if (part.empty() || part == ".") continue;
    if (part == "..") {
      if (names.empty()) return false;
      names.pop_back();
      continue;
    }
    for (unsigned char c : part)
      if (c < 0x20 || std::string("\\:*?\"<>|").find(static_cast<char>(c)) != std::string::npos) return false;
    if (part.front() == ' ' || part.back() == ' ' || part.back() == '.') return false;
    names.push_back(part);
  }
  *out = std::move(names);
  return true;
}

// "A/B" for {"A", "B"}; "" for the root.
inline std::string joinHostPath(const std::vector<std::string>& names) {
  std::string s;
  for (const std::string& n : names) s += (s.empty() ? "" : "/") + n;
  return s;
}

// A file's kind for FILE_GET's FILE_PUT (2026-11-16, BLE_PROTOCOL.md
// "Files"): 0 (BASIC) when all of it is BASIC's line layout -- [number
// hi][lo][length][length bytes, the last CR], numbers above 0 and rising,
// then FF as its last byte -- or it's empty; else 1 (M). As the firmware's
// keywords.c file_is_basic() and the app's fileKind().
inline uint8_t fileKind(const std::vector<uint8_t>& b) {
  size_t i = 0;
  unsigned prev = 0;
  while (i < b.size()) {
    if (b[i] == 0xFF) return i == b.size() - 1 ? 0 : 1;
    if (i + 3 > b.size()) return 1;
    unsigned num = static_cast<unsigned>(b[i] << 8 | b[i + 1]), len = b[i + 2];
    if (num == 0 || num <= prev || len == 0) return 1;
    i += 3 + len;
    if (i > b.size() || b[i - 1] != 0x0D) return 1;
    prev = num;
  }
  return b.empty() ? 0 : 1;
}

// FS ops (pc_exp.h EXP_BLE_FS_*, BLE_PROTOCOL.md's FS)
constexpr uint8_t kFsCd = 1, kFsPwd = 2, kFsMkdir = 3, kFsRmdir = 4, kFsRm = 5, kFsCp = 6, kFsMv = 7;

}  // namespace pc1500::ble
