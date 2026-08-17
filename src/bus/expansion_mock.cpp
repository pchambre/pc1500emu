// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#include "expansion_mock.h"

#include <algorithm>

namespace pc1500 {

namespace fs = std::filesystem;

void ExpansionMock::formatSizeText(uint32_t value, std::vector<uint8_t>& window, size_t offset,
                                    int width) {
  for (int i = 0; i < width; i++) window[offset + i] = static_cast<uint8_t>(' ');
  int i = width;
  do {
    i--;
    window[offset + i] = static_cast<uint8_t>('0' + (value % 10));
    value /= 10;
  } while (value != 0 && i > 0);
}

void ExpansionMock::writeText(const std::string& text, std::vector<uint8_t>& window,
                               size_t offset, int width) {
  for (int i = 0; i < width; i++) {
    window[offset + i] = (i < static_cast<int>(text.size())) ? static_cast<uint8_t>(text[i])
                                                               : static_cast<uint8_t>(' ');
  }
}

// Mirrors main.c's own StringFromBuffer convention: 2-byte BE length at
// window[offset], then that many raw ASCII bytes right after.
std::string ExpansionMock::readLengthPrefixedString(const std::vector<uint8_t>& window,
                                                      size_t offset) {
  if (window.size() < offset + 2) return {};
  uint16_t len = (static_cast<uint16_t>(window[offset]) << 8) | window[offset + 1];
  std::string s;
  for (uint16_t i = 0; i < len && offset + 2 + i < window.size(); i++) {
    s += static_cast<char>(window[offset + 2 + i]);
  }
  return s;
}

// Mirrors main.c's EXP_COMMAND_GET_SD_FILE_NAME/READ_SD_VOLUME_LABEL
// responses: a 1-byte length followed by that many raw ASCII bytes,
// staged at EXP_SCRATCH_PAGE (window[kScratchOffset..]).
void ExpansionMock::writeLengthPrefixedString(const std::string& text, std::vector<uint8_t>& window,
                                               size_t offset) {
  uint8_t len = static_cast<uint8_t>(std::min<size_t>(text.size(), 255));
  if (offset >= window.size()) return;
  window[offset] = len;
  for (uint8_t i = 0; i < len && offset + 1 + i < window.size(); i++) {
    window[offset + 1 + i] = static_cast<uint8_t>(text[i]);
  }
}

// `name` is untrusted, straight off the wire from whatever's running on
// the LH5801 -- reject anything that isn't a bare filename before ever
// touching the filesystem. This is a local dev tool, not network-exposed,
// but a ROM bug (or a fuzz test) writing/deleting files outside the
// intended sandbox directory would be a bad surprise worth deliberately
// preventing. Rejecting any path separator outright (rather than trying
// to canonicalize ".." components away) is the simple, robust rule; the
// weakly_canonical containment check below is defense in depth for
// anything that slips past it (e.g. symlinks).
fs::path ExpansionMock::resolvePath(const std::string& name) const {
  if (rootDir_.empty() || name.empty()) return {};
  if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) return {};
  if (name == "." || name == "..") return {};
  if (name.find(':') != std::string::npos) return {};  // e.g. a Windows drive letter

  fs::path candidate = rootDir_ / name;
  std::error_code ec;
  fs::path canonicalRoot = fs::weakly_canonical(rootDir_, ec);
  if (ec) return {};
  fs::path canonicalCandidate = fs::weakly_canonical(candidate, ec);
  if (ec) return {};

  const auto& rootStr = canonicalRoot.native();
  const auto& candStr = canonicalCandidate.native();
  if (candStr.size() < rootStr.size() || candStr.compare(0, rootStr.size(), rootStr) != 0) {
    return {};
  }
  return candidate;
}

uint8_t ExpansionMock::processCommand(uint8_t cmd, std::vector<uint8_t>& window) {
  switch (cmd) {
    case kCommandListSdDir:
      return listSdDir(window);
    case kCommandGetSdFreeSpace:
      return getSdFreeSpace(window);
    case kCommandGetSdVolumeSize:
      return getSdVolumeSize(window);
    case kCommandCreateSdFile:
      return createSdFile(window);
    case kCommandOpenSdFileRead:
      return openSdFileRead(window);
    case kCommandWriteToSdFile:
      return writeToSdFile(window);
    case kCommandReadFromSdFile:
      return readFromSdFile(window);
    case kCommandCloseSdFile:
      return closeSdFile(window);
    case kCommandGetSdFileSize:
      return getSdFileSize(window);
    case kCommandGetSdFileStatus:
      return getSdFileStatus(window);
    case kCommandGetSdFileName:
      return getSdFileName(window);
    case kCommandRemoveSdFile:
      return removeSdFile(window);
    case kCommandReadSdVolumeLabel:
      return readSdVolumeLabel(window);
    case kCommandFormatSdCard:
      return formatSdCard(window);
    case kCommandClearStatus:
      return kStatusReady;
    default:
      // Matches DoCommand()'s own default: case -- every command this
      // mock doesn't implement (ROM_FROM_MCU/SRAM, TEST_COPY_STRING, ...)
      // reports NOT_IMPLEMENTED rather than silently succeeding, so ROM
      // code relying on a real result fails loudly.
      return kStatusNotImplemented;
  }
}

// Ported from main.c's EXP_COMMAND_LIST_SD_DIR case: 2-byte BE count, then
// per-entry kDirNameLen-byte space-padded/truncated name +
// kDirSizeTextLen-byte pre-rendered decimal text (back-to-back with the
// name so the ROM can blit both in one DISP_N_CHARS0 call) + 4-byte BE
// binary size trailing, starting at window[0]. A summary line
// (kSummaryLineLen bytes, plain text) follows right after the last entry
// -- e.g. "3 FILES 23051B 2122343F". SLS (rom.asm) hardcodes the offset
// for a fixed-size listing rather than reading the real count and
// locating it generally, since the LH5801 has no multiply instruction.
uint8_t ExpansionMock::listSdDir(std::vector<uint8_t>& window) {
  uint16_t count = 0;
  uint64_t totalBytes = 0;
  if (!rootDir_.empty()) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(rootDir_, ec)) {
      if (count >= kDirMaxEntries) break;
      if (!entry.is_regular_file()) continue;
      size_t entryOffset = 2 + static_cast<size_t>(count) * kDirRecordSize;
      if (entryOffset + kDirRecordSize > window.size()) break;
      std::string name = entry.path().filename().string();
      uint64_t size = entry.file_size(ec);
      if (ec) continue;
      for (int i = 0; i < kDirNameLen; i++) {
        window[entryOffset + i] = (i < static_cast<int>(name.size()))
                                       ? static_cast<uint8_t>(name[i])
                                       : static_cast<uint8_t>(' ');
      }
      formatSizeText(static_cast<uint32_t>(size), window, entryOffset + kDirNameLen,
                      kDirSizeTextLen);
      size_t binOffset = entryOffset + kDirNameLen + kDirSizeTextLen;
      window[binOffset + 0] = static_cast<uint8_t>(size >> 24);
      window[binOffset + 1] = static_cast<uint8_t>(size >> 16);
      window[binOffset + 2] = static_cast<uint8_t>(size >> 8);
      window[binOffset + 3] = static_cast<uint8_t>(size);
      totalBytes += size;
      count++;
    }
  }
  if (window.size() >= 2) {
    window[0] = static_cast<uint8_t>(count >> 8);
    window[1] = static_cast<uint8_t>(count & 0xFF);
  }
  size_t summaryOffset = 2 + static_cast<size_t>(count) * kDirRecordSize;
  if (summaryOffset + kSummaryLineLen <= window.size()) {
    uint32_t freeBytes = freeSpaceBytes_;
    if (!rootDir_.empty()) {
      std::error_code ec;
      fs::space_info info = fs::space(rootDir_, ec);
      if (!ec) freeBytes = static_cast<uint32_t>(info.available);
    }
    std::string summary =
        std::to_string(count) + " FILES " + std::to_string(totalBytes) + "B " +
        std::to_string(freeBytes) + "F";
    writeText(summary, window, summaryOffset, kSummaryLineLen);
  }
  return kStatusSuccess;
}

uint8_t ExpansionMock::getSdFreeSpace(std::vector<uint8_t>& window) {
  if (rootDir_.empty() || window.size() < 4) return kStatusError;
  std::error_code ec;
  fs::space_info info = fs::space(rootDir_, ec);
  if (ec) return kStatusError;
  uint32_t v = static_cast<uint32_t>(info.available);
  window[0] = static_cast<uint8_t>(v >> 24);
  window[1] = static_cast<uint8_t>(v >> 16);
  window[2] = static_cast<uint8_t>(v >> 8);
  window[3] = static_cast<uint8_t>(v);
  return kStatusSuccess;
}

uint8_t ExpansionMock::getSdVolumeSize(std::vector<uint8_t>& window) {
  if (rootDir_.empty() || window.size() < 4) return kStatusError;
  std::error_code ec;
  fs::space_info info = fs::space(rootDir_, ec);
  if (ec) return kStatusError;
  uint32_t v = static_cast<uint32_t>(info.capacity);
  window[0] = static_cast<uint8_t>(v >> 24);
  window[1] = static_cast<uint8_t>(v >> 16);
  window[2] = static_cast<uint8_t>(v >> 8);
  window[3] = static_cast<uint8_t>(v);
  return kStatusSuccess;
}

uint8_t ExpansionMock::createSdFile(std::vector<uint8_t>& window) {
  std::string name = readLengthPrefixedString(window);
  if (name.empty()) return kStatusError;
  fs::path path = resolvePath(name);
  if (path.empty()) return kStatusError;
  openFile_.close();
  openFile_.clear();
  openFile_.open(path, std::ios::binary | std::ios::out | std::ios::trunc);
  if (!openFile_.is_open()) return kStatusError;
  fileStatus_ = kFileStatusOpenWrite;
  openFileName_ = name;
  bytesWrittenTotal_ = 0;
  return kStatusSuccess;
}

uint8_t ExpansionMock::openSdFileRead(std::vector<uint8_t>& window) {
  std::string name = readLengthPrefixedString(window);
  if (name.empty()) return kStatusError;
  fs::path path = resolvePath(name);
  if (path.empty()) return kStatusError;
  openFile_.close();
  openFile_.clear();
  openFile_.open(path, std::ios::binary | std::ios::in);
  if (!openFile_.is_open()) return kStatusError;
  fileStatus_ = kFileStatusOpenRead;
  openFileName_ = name;
  return kStatusSuccess;
}

// dataLen is a 2-byte BE count at window[0..1] (not length-prefixed-string
// style -- matches main.c's own WRITE_TO_SD_FILE exactly), data starts at
// window[2]. main.c also echoes dataLen into page 15 offset 0-1
// (window[15*256..15*256+1]) as a debug artifact of the real firmware;
// mirrored here for fidelity even though nothing currently reads it back.
uint8_t ExpansionMock::writeToSdFile(std::vector<uint8_t>& window) {
  if (fileStatus_ != kFileStatusOpenWrite || !openFile_.is_open()) return kStatusError;
  if (window.size() < 2) return kStatusError;
  uint16_t dataLen = (static_cast<uint16_t>(window[0]) << 8) | window[1];
  if (dataLen == 0) return kStatusError;
  if (window.size() < static_cast<size_t>(2) + dataLen) return kStatusError;
  if (window.size() > 15 * 256 + 1) {
    window[15 * 256 + 0] = static_cast<uint8_t>(dataLen >> 8);
    window[15 * 256 + 1] = static_cast<uint8_t>(dataLen & 0xFF);
  }
  openFile_.write(reinterpret_cast<const char*>(window.data() + 2), dataLen);
  if (!openFile_.good()) return kStatusError;
  openFile_.flush();
  bytesWrittenTotal_ += dataLen;
  return kStatusSuccess;
}

// requestLen capped at 254 (not 255) matching main.c's own comment: data
// goes back into the same page starting at +2, so it must fit in what's
// left of a 256-byte page.
uint8_t ExpansionMock::readFromSdFile(std::vector<uint8_t>& window) {
  if (fileStatus_ != kFileStatusOpenRead || !openFile_.is_open()) return kStatusError;
  if (window.size() < 2) return kStatusError;
  uint16_t requestLen = (static_cast<uint16_t>(window[0]) << 8) | window[1];
  if (requestLen == 0 || requestLen > 254) return kStatusError;
  if (window.size() < static_cast<size_t>(2) + requestLen) return kStatusError;
  openFile_.read(reinterpret_cast<char*>(window.data() + 2), requestLen);
  std::streamsize bytesRead = openFile_.gcount();
  openFile_.clear();  // clear eof/fail from a short read -- not an error here
  window[0] = static_cast<uint8_t>(bytesRead >> 8);
  window[1] = static_cast<uint8_t>(bytesRead & 0xFF);
  return kStatusSuccess;
}

uint8_t ExpansionMock::closeSdFile(std::vector<uint8_t>& window) {
  if (!openFile_.is_open() || fileStatus_ == kFileStatusClosed) return kStatusError;
  openFile_.close();
  fileStatus_ = kFileStatusClosed;
  if (window.size() >= 4) {
    window[0] = static_cast<uint8_t>(bytesWrittenTotal_ >> 24);
    window[1] = static_cast<uint8_t>(bytesWrittenTotal_ >> 16);
    window[2] = static_cast<uint8_t>(bytesWrittenTotal_ >> 8);
    window[3] = static_cast<uint8_t>(bytesWrittenTotal_);
  }
  bytesWrittenTotal_ = 0;
  return kStatusSuccess;
}

uint8_t ExpansionMock::getSdFileSize(std::vector<uint8_t>& window) {
  if (fileStatus_ == kFileStatusClosed || openFileName_.empty()) return kStatusError;
  fs::path path = resolvePath(openFileName_);
  if (path.empty()) return kStatusError;
  std::error_code ec;
  uint64_t size = fs::file_size(path, ec);
  if (ec || window.size() < 4) return kStatusError;
  window[0] = static_cast<uint8_t>(size >> 24);
  window[1] = static_cast<uint8_t>(size >> 16);
  window[2] = static_cast<uint8_t>(size >> 8);
  window[3] = static_cast<uint8_t>(size);
  return kStatusSuccess;
}

uint8_t ExpansionMock::getSdFileStatus(std::vector<uint8_t>& window) {
  if (window.empty()) return kStatusError;
  window[0] = fileStatus_;
  return kStatusSuccess;
}

uint8_t ExpansionMock::getSdFileName(std::vector<uint8_t>& window) {
  if (fileStatus_ == kFileStatusClosed || openFileName_.empty()) return kStatusError;
  writeLengthPrefixedString(openFileName_, window, kScratchOffset);
  return kStatusSuccess;
}

uint8_t ExpansionMock::removeSdFile(std::vector<uint8_t>& window) {
  std::string name = readLengthPrefixedString(window);
  if (name.empty()) return kStatusError;
  fs::path path = resolvePath(name);
  if (path.empty()) return kStatusError;
  std::error_code ec;
  bool removed = fs::remove(path, ec);
  return (removed && !ec) ? kStatusSuccess : kStatusError;
}

// No real host equivalent of an SD volume label -- returns a fixed mock
// label (the configured root directory's own folder name) regardless of
// the requested volume name, matching this being a mock, not real FS_*
// volume-label lookup logic.
uint8_t ExpansionMock::readSdVolumeLabel(std::vector<uint8_t>& window) {
  std::string requestedVolume = readLengthPrefixedString(window);
  if (requestedVolume.empty() || rootDir_.empty()) return kStatusError;
  std::string label = rootDir_.filename().string();
  if (label.empty()) label = "MOCKSD";
  writeLengthPrefixedString(label, window, kScratchOffset);
  return kStatusSuccess;
}

// Deletes every regular file directly inside rootDir_ -- destructive by
// design, mirroring the real FORMAT command's own intent. Does not touch
// subdirectories. Callers should not point rootDir_ at anything they care
// about (see README.md's own warning).
uint8_t ExpansionMock::formatSdCard(std::vector<uint8_t>& window) {
  std::string requestedVolume = readLengthPrefixedString(window);
  if (requestedVolume.empty() || rootDir_.empty()) return kStatusError;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(rootDir_, ec)) {
    if (entry.is_regular_file()) {
      std::error_code removeEc;
      fs::remove(entry.path(), removeEc);
    }
  }
  return ec ? kStatusError : kStatusSuccess;
}

}  // namespace pc1500
