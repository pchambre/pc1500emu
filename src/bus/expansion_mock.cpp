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

std::string ExpansionMock::convertPlusToTilde(const std::string& name) {
  std::string result = name;
  for (char& c : result) {
    if (c == '+') c = '~';
  }
  return result;
}

std::string ExpansionMock::convertTildeToPlus(const std::string& name) {
  std::string result = name;
  for (char& c : result) {
    if (c == '~') c = '+';
  }
  return result;
}

// `name` is untrusted, straight off the wire from whatever's running on
// the LH5801 -- resolve it and confirm it stays inside rootDir_ before
// ever touching the filesystem. This is a local dev tool, not
// network-exposed, but a ROM bug (or a fuzz test) writing/deleting files
// outside the intended sandbox directory would be a bad surprise worth
// deliberately preventing.
//
// `name` may be a plain filename (resolved against currentDir_, SDCD's
// own state), a relative path with '.'/'..'/multiple components (e.g.
// "SUB/DIR", "../OTHER"), or an absolute one starting with '/' (resolved
// against rootDir_ instead -- a bare "/" means the SD root itself). This
// used to be two near-duplicate functions (resolvePath, plain filenames
// only; resolveDirPath, relative paths for SDCD/MKDIR/RMDIR but no
// absolute support) -- merged once SDLOAD/SDSAVE/SDRM/SDCP/SDMV all
// needed the same relative+absolute support resolveDirPath already had
// for directories, matching rom.asm's SD_PARSE_QUOTED_NAME, which already
// validates 8.3 shape per '/'-segment for every one of these commands
// uniformly regardless of what the destination command actually does
// with the result.
//
// The weakly_canonical containment check below is the *primary* defense
// against escaping the sandbox (not just defense in depth) -- it
// correctly handles arbitrarily nested "up and down" traversal via
// fs::path's own lexically_normal() first.
fs::path ExpansionMock::resolvePath(const std::string& name) const {
  if (rootDir_.empty() || name.empty()) return {};
  std::string translated = convertPlusToTilde(name);
  if (translated.find('\\') != std::string::npos) return {};  // no raw Windows separators
  if (translated.find(':') != std::string::npos) return {};   // e.g. a drive letter

  fs::path base;
  std::string rest = translated;
  if (rest.front() == '/') {
    base = rootDir_;
    rest = rest.substr(1);
  } else {
    base = currentDir_;
  }
  fs::path candidate = rest.empty() ? base : (base / rest).lexically_normal();
  std::error_code ec;
  fs::path canonicalRoot = fs::weakly_canonical(rootDir_, ec);
  if (ec) return {};
  fs::path canonicalCandidate = fs::weakly_canonical(candidate, ec);
  if (ec) return {};

  const auto& rootStr = canonicalRoot.native();
  const auto& candStr = canonicalCandidate.native();
  if (candStr.size() < rootStr.size() || candStr.compare(0, rootStr.size(), rootStr) != 0) {
    return {};  // would escape the sandbox root (e.g. ".." from the root itself)
  }
  return candidate;
}

// For SDCP/SDMV's destination argument: resolves destArg via resolvePath,
// then -- if that resolves to an *existing directory* -- returns (that
// directory)/srcBasename instead, matching Unix cp/mv's own "copy/move
// INTO a directory" behavior.
fs::path ExpansionMock::resolveCopyOrMoveDestination(const std::string& srcBasename,
                                                      const std::string& destArg) const {
  fs::path resolved = resolvePath(destArg);
  if (resolved.empty()) return {};
  std::error_code ec;
  if (fs::is_directory(resolved, ec) && !ec) {
    return resolved / srcBasename;
  }
  return resolved;
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
    case kCommandChangeSdDir:
      return changeSdDir(window);
    case kCommandMakeSdDir:
      return makeSdDir(window);
    case kCommandRemoveSdDir:
      return removeSdDir(window);
    case kCommandGetSdCwd:
      return getSdCwd(window);
    case kCommandCopySdFile:
      return copySdFile(window);
    case kCommandMoveSdFile:
      return moveSdFile(window);
    case kCommandGetSdDfText:
      return getSdDfText(window);
    case kCommandCheckSdCopyMoveDestExists:
      return checkSdCopyMoveDestExists(window);
    case kCommandSdOpenChannel:
      return openSdChannel(window);
    case kCommandSdCloseChannel:
      return closeSdChannel(window);
    case kCommandSdListChannels:
      return listSdChannels(window);
    case kCommandSdWriteValue:
      return writeSdValue(window);
    case kCommandSdReadValue:
      return readSdValue(window);
    case kCommandSdSkipValues:
      return skipSdValues(window);
    case kCommandValidateSdName:
      return validateAndFoldSdName(window);
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
    // currentDir_, not always rootDir_ -- SDLS must reflect SDCD's own
    // state, same as every other command resolvePath already honors (see
    // that function's own comment). This mock previously always listed
    // the root regardless of any prior SDCD; found and fixed alongside
    // this same directory-listing change.
    for (const auto& entry : fs::directory_iterator(currentDir_, ec)) {
      if (count >= kDirMaxEntries) break;
      bool isDir = entry.is_directory();
      if (!isDir && !entry.is_regular_file()) continue;  // skip anything else (dangling symlinks, ...)
      size_t entryOffset = 2 + static_cast<size_t>(count) * kDirRecordSize;
      if (entryOffset + kDirRecordSize > window.size()) break;
      std::string name = convertTildeToPlus(entry.path().filename().string());
      uint64_t size = 0;
      if (!isDir) {
        size = entry.file_size(ec);
        if (ec) continue;
      }
      for (int i = 0; i < kDirNameLen; i++) {
        window[entryOffset + i] = (i < static_cast<int>(name.size()))
                                       ? static_cast<uint8_t>(name[i])
                                       : static_cast<uint8_t>(' ');
      }
      if (isDir) {
        // Right-justified "<DIR>" in the size-text field, same column
        // numeric sizes occupy -- matches main.c's own convention exactly
        // (kept in sync by hand, same caveat as everywhere else in this
        // file).
        static constexpr char kDirText[] = "<DIR>";
        constexpr int kDirTextLen = sizeof(kDirText) - 1;
        for (int i = 0; i < kDirSizeTextLen; i++) {
          window[entryOffset + kDirNameLen + i] = static_cast<uint8_t>(' ');
        }
        for (int i = 0; i < kDirTextLen; i++) {
          window[entryOffset + kDirNameLen + kDirSizeTextLen - kDirTextLen + i] =
              static_cast<uint8_t>(kDirText[i]);
        }
      } else {
        formatSizeText(static_cast<uint32_t>(size), window, entryOffset + kDirNameLen,
                        kDirSizeTextLen);
      }
      size_t binOffset = entryOffset + kDirNameLen + kDirSizeTextLen;
      window[binOffset + 0] = static_cast<uint8_t>(size >> 24);
      window[binOffset + 1] = static_cast<uint8_t>(size >> 16);
      window[binOffset + 2] = static_cast<uint8_t>(size >> 8);
      window[binOffset + 3] = static_cast<uint8_t>(size);
      if (!isDir) totalBytes += size;
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
  // SDRM (rom.asm) must only ever delete a file, never a directory --
  // SDRMDIR is the only sanctioned way to remove one. Without this,
  // fs::remove below would happily remove an *empty* directory too.
  if (!fs::is_regular_file(path, ec) || ec) return kStatusError;
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

uint8_t ExpansionMock::changeSdDir(std::vector<uint8_t>& window) {
  std::string name = readLengthPrefixedString(window);
  if (name.empty()) return kStatusError;
  fs::path target = resolvePath(name);
  if (target.empty()) return kStatusError;
  std::error_code ec;
  if (!fs::is_directory(target, ec) || ec) return kStatusError;
  currentDir_ = target;
  return kStatusSuccess;
}

uint8_t ExpansionMock::makeSdDir(std::vector<uint8_t>& window) {
  std::string name = readLengthPrefixedString(window);
  if (name.empty()) return kStatusError;
  fs::path target = resolvePath(name);
  if (target.empty()) return kStatusError;
  std::error_code ec;
  bool created = fs::create_directory(target, ec);
  return (created && !ec) ? kStatusSuccess : kStatusError;
}

uint8_t ExpansionMock::removeSdDir(std::vector<uint8_t>& window) {
  std::string name = readLengthPrefixedString(window);
  if (name.empty()) return kStatusError;
  fs::path target = resolvePath(name);
  if (target.empty()) return kStatusError;
  std::error_code ec;
  // fs::remove only removes an empty directory (or a single file) -- never
  // recurses -- matching real emFile's own FS_RmDir semantics exactly
  // (fails outright on a non-empty directory rather than deleting its
  // contents).
  bool removed = fs::remove(target, ec);
  return (removed && !ec) ? kStatusSuccess : kStatusError;
}

uint8_t ExpansionMock::getSdCwd(std::vector<uint8_t>& window) {
  if (rootDir_.empty()) return kStatusError;
  // Mirrors main.c's own GET_SD_CWD response format (length-prefixed into
  // EXP_SCRATCH_PAGE) -- reports the path *relative to the SD root*
  // (e.g. "/" at the root, "/SUBDIR" after changeSdDir("SUBDIR")), not a
  // real host filesystem path, since that's what's actually meaningful to
  // a PC-1500 user; the host rootDir_ location is a pc1500emu/testing
  // implementation detail. This is a mock convention, not verified
  // against what the real emFile FS_GetCWD returns on real hardware
  // (likely includes its own volume-name prefix) -- fine here since
  // SDPWD_ROUTINE just blits back whatever text comes over the wire,
  // with no parsing on the ROM side either way.
  std::error_code ec;
  fs::path rel = fs::relative(currentDir_, rootDir_, ec);
  if (ec) return kStatusError;
  std::string cwd = convertTildeToPlus("/" + (rel == "." ? std::string() : rel.generic_string()));
  writeLengthPrefixedString(cwd, window, kScratchOffset);
  return kStatusSuccess;
}

// Ported from main.c's EXP_COMMAND_COPY_SD_FILE case: two fixed
// kTwoNameSlotLen-byte slots back-to-back at window offset 0 (source, then
// destination) -- see expansion_mock.h's own comment on kTwoNameSlotLen
// for why fixed-width. Both source and destination are full paths now
// (resolvePath); if the destination resolves to an existing directory,
// the real target is that directory plus the source's own basename
// (resolveCopyOrMoveDestination), matching Unix cp's own "copy INTO a
// directory" behavior. Overwriting an existing destination is confirmed
// first by the ROM (EXP_COMMAND_CHECK_SD_COPY_MOVE_DEST_EXISTS, below)
// unless -Y was given -- this command itself always overwrites
// unconditionally, same as fs::copy_options::overwrite_existing always
// has; the confirmation gate lives entirely on the ROM side.
uint8_t ExpansionMock::copySdFile(std::vector<uint8_t>& window) {
  std::string srcArg = readLengthPrefixedString(window, 0);
  std::string destArg = readLengthPrefixedString(window, kTwoNameSlotLen);
  if (srcArg.empty() || destArg.empty()) return kStatusError;
  fs::path srcPath = resolvePath(srcArg);
  if (srcPath.empty()) return kStatusError;
  std::error_code ec;
  if (!fs::is_regular_file(srcPath, ec) || ec) return kStatusError;
  fs::path destPath = resolveCopyOrMoveDestination(srcPath.filename().string(), destArg);
  if (destPath.empty()) return kStatusError;
  bool copied = fs::copy_file(srcPath, destPath, fs::copy_options::overwrite_existing, ec);
  return (copied && !ec) ? kStatusSuccess : kStatusError;
}

// Ported from main.c's EXP_COMMAND_MOVE_SD_FILE case -- same wire layout
// and destination resolution as copySdFile above. fs::rename mirrors
// POSIX rename() semantics (silently replaces an existing destination).
uint8_t ExpansionMock::moveSdFile(std::vector<uint8_t>& window) {
  std::string srcArg = readLengthPrefixedString(window, 0);
  std::string destArg = readLengthPrefixedString(window, kTwoNameSlotLen);
  if (srcArg.empty() || destArg.empty()) return kStatusError;
  fs::path srcPath = resolvePath(srcArg);
  if (srcPath.empty()) return kStatusError;
  std::error_code ec;
  if (!fs::is_regular_file(srcPath, ec) || ec) return kStatusError;
  fs::path destPath = resolveCopyOrMoveDestination(srcPath.filename().string(), destArg);
  if (destPath.empty()) return kStatusError;
  fs::rename(srcPath, destPath, ec);
  return ec ? kStatusError : kStatusSuccess;
}

// Ported from main.c's EXP_COMMAND_CHECK_SD_COPY_MOVE_DEST_EXISTS case:
// same wire layout and destination resolution as copySdFile/moveSdFile
// above (shared, since the resolution logic -- including directory-target
// basename-join -- is identical for both) -- SUCCESS means the real
// (resolved) target already exists, so SDCP_ROUTINE/SDMV_ROUTINE (rom.asm)
// know to show the overwrite-confirmation prompt unless -Y was given.
uint8_t ExpansionMock::checkSdCopyMoveDestExists(std::vector<uint8_t>& window) {
  std::string srcArg = readLengthPrefixedString(window, 0);
  std::string destArg = readLengthPrefixedString(window, kTwoNameSlotLen);
  if (srcArg.empty() || destArg.empty()) return kStatusError;
  fs::path srcPath = resolvePath(srcArg);
  if (srcPath.empty()) return kStatusError;
  fs::path destPath = resolveCopyOrMoveDestination(srcPath.filename().string(), destArg);
  if (destPath.empty()) return kStatusError;
  std::error_code ec;
  return (fs::exists(destPath, ec) && !ec) ? kStatusSuccess : kStatusError;
}

void ExpansionMock::closeSdChannelAt(int index) {
  SdChannel& ch = channels_[index];
  if (ch.isOpen) {
    ch.file.close();
    ch.file.clear();
  }
  ch.isOpen = false;
  ch.name.clear();
  ch.readPos = 0;
}

// Ported from main.c's EXP_COMMAND_SD_OPEN_CHANNEL case: window[0]=channel
// (1-kMaxSdChannels), then a length-prefixed filename at window[1]. Opens
// for combined read+append (creating the file if it doesn't exist, never
// truncating one that does) -- SDPRINT#/SDINPUT#/SDSKIP# all operate on
// whatever's already there. Reusing an already-open channel number closes
// it first, matching SDOPEN's own documented behavior.
uint8_t ExpansionMock::openSdChannel(std::vector<uint8_t>& window) {
  if (window.empty()) return kStatusError;
  uint8_t channel = window[0];
  if (channel < 1 || channel > kMaxSdChannels) return kStatusError;
  std::string name = readLengthPrefixedString(window, 1);
  if (name.empty()) return kStatusError;
  fs::path path = resolvePath(name);
  if (path.empty()) return kStatusError;

  int index = channel - 1;
  closeSdChannelAt(index);

  SdChannel& ch = channels_[index];
  ch.file.open(path, std::ios::binary | std::ios::in | std::ios::out);
  if (!ch.file.is_open()) {
    // Doesn't exist yet -- plain in|out can't create a file on most
    // implementations, so create it out-only first, then reopen in|out.
    ch.file.clear();
    ch.file.open(path, std::ios::binary | std::ios::out);
    ch.file.close();
    ch.file.clear();
    ch.file.open(path, std::ios::binary | std::ios::in | std::ios::out);
  }
  if (!ch.file.is_open()) return kStatusError;
  ch.isOpen = true;
  ch.name = name;
  ch.readPos = 0;
  return kStatusSuccess;
}

// Ported from main.c's EXP_COMMAND_SD_CLOSE_CHANNEL case: window[0]=channel,
// or 0 to close every open channel.
uint8_t ExpansionMock::closeSdChannel(std::vector<uint8_t>& window) {
  if (window.empty()) return kStatusError;
  uint8_t channel = window[0];
  if (channel == 0) {
    for (int i = 0; i < kMaxSdChannels; i++) closeSdChannelAt(i);
    return kStatusSuccess;
  }
  if (channel > kMaxSdChannels) return kStatusError;
  closeSdChannelAt(channel - 1);
  return kStatusSuccess;
}

// Ported from main.c's EXP_COMMAND_SD_LIST_CHANNELS case: reuses
// EXP_COMMAND_LIST_SD_DIR's own wire format exactly (see listSdDir's own
// comment) so rom.asm's existing SD_LIST_INIT/SD_LIST_DISPLAY browse
// machinery can draw it unmodified -- one record per open channel, name
// field showing "<n>:<filename>".
uint8_t ExpansionMock::listSdChannels(std::vector<uint8_t>& window) {
  uint16_t count = 0;
  for (int i = 0; i < kMaxSdChannels && count < kDirMaxEntries; i++) {
    if (!channels_[i].isOpen) continue;
    size_t entryOffset = 2 + static_cast<size_t>(count) * kDirRecordSize;
    if (entryOffset + kDirRecordSize > window.size()) break;
    std::string label = std::to_string(i + 1) + ":" + channels_[i].name;
    for (int j = 0; j < kDirNameLen; j++) {
      window[entryOffset + j] = (j < static_cast<int>(label.size()))
                                     ? static_cast<uint8_t>(label[j])
                                     : static_cast<uint8_t>(' ');
    }
    // Size-text field left blank -- an open channel doesn't have a
    // meaningful "size" the way a directory entry does.
    for (int j = 0; j < kDirSizeTextLen; j++) {
      window[entryOffset + kDirNameLen + j] = static_cast<uint8_t>(' ');
    }
    size_t binOffset = entryOffset + kDirNameLen + kDirSizeTextLen;
    for (int j = 0; j < 4; j++) window[binOffset + j] = 0;
    count++;
  }
  if (window.size() >= 2) {
    window[0] = static_cast<uint8_t>(count >> 8);
    window[1] = static_cast<uint8_t>(count & 0xFF);
  }
  size_t summaryOffset = 2 + static_cast<size_t>(count) * kDirRecordSize;
  if (summaryOffset + kSummaryLineLen <= window.size()) {
    std::string summary = std::to_string(count) + " OPEN";
    writeText(summary, window, summaryOffset, kSummaryLineLen);
  }
  return kStatusSuccess;
}

// Ported from main.c's EXP_COMMAND_SD_WRITE_VALUE case: window[0]=channel,
// window[1..]=one chunk (['N']+8 bytes, or ['S']+1-byte length+that many
// bytes -- see PC_EXP.h's own comment) built by rom.asm from a looked-up
// variable's own storage. This mock never interprets the payload, just
// appends the whole chunk's bytes to the end of the channel's file,
// regardless of the channel's own read position.
uint8_t ExpansionMock::writeSdValue(std::vector<uint8_t>& window) {
  if (window.empty()) return kStatusError;
  uint8_t channel = window[0];
  if (channel < 1 || channel > kMaxSdChannels) return kStatusError;
  SdChannel& ch = channels_[channel - 1];
  if (!ch.isOpen) return kStatusError;
  if (window.size() < 2) return kStatusError;
  uint8_t tag = window[1];
  size_t chunkLen;
  if (tag == 'N') {
    chunkLen = 1 + 8;
  } else if (tag == 'S') {
    if (window.size() < 3) return kStatusError;
    chunkLen = 1 + 1 + window[2];
  } else {
    return kStatusError;
  }
  if (window.size() < 1 + chunkLen) return kStatusError;
  ch.file.clear();
  ch.file.seekp(0, std::ios::end);
  ch.file.write(reinterpret_cast<const char*>(window.data() + 1), static_cast<std::streamsize>(chunkLen));
  if (!ch.file.good()) return kStatusError;
  ch.file.flush();
  return kStatusSuccess;
}

// Ported from main.c's EXP_COMMAND_SD_READ_VALUE case: window[0]=channel
// (request); response overwrites window[0..] with the next chunk read
// from the channel's own persistent read position (advanced past it on
// return, independent of writeSdValue's own always-append behavior), or
// kStatusEof if the channel has no more stored values.
uint8_t ExpansionMock::readSdValue(std::vector<uint8_t>& window) {
  if (window.empty()) return kStatusError;
  uint8_t channel = window[0];
  if (channel < 1 || channel > kMaxSdChannels) return kStatusError;
  SdChannel& ch = channels_[channel - 1];
  if (!ch.isOpen) return kStatusError;
  ch.file.clear();
  ch.file.seekg(static_cast<std::streamoff>(ch.readPos), std::ios::beg);
  char tag = 0;
  ch.file.read(&tag, 1);
  if (ch.file.gcount() != 1) return kStatusEof;
  size_t payloadLen;
  if (tag == 'N') {
    payloadLen = 8;
  } else if (tag == 'S') {
    char lenByte = 0;
    ch.file.read(&lenByte, 1);
    if (ch.file.gcount() != 1) return kStatusEof;  // truncated chunk -- treat as EOF
    payloadLen = static_cast<uint8_t>(lenByte);
  } else {
    return kStatusError;  // corrupt file
  }
  std::vector<char> payload(payloadLen);
  if (payloadLen > 0) {
    ch.file.read(payload.data(), static_cast<std::streamsize>(payloadLen));
    if (static_cast<size_t>(ch.file.gcount()) != payloadLen) return kStatusEof;
  }
  size_t pos = 0;
  window[pos++] = static_cast<uint8_t>(tag);
  if (tag == 'S') {
    if (window.size() < pos + 1) return kStatusError;
    window[pos++] = static_cast<uint8_t>(payloadLen);
  }
  for (size_t i = 0; i < payloadLen && pos < window.size(); i++) {
    window[pos++] = static_cast<uint8_t>(payload[i]);
  }
  ch.readPos = static_cast<uint32_t>(ch.file.tellg());
  return kStatusSuccess;
}

// Ported from main.c's EXP_COMMAND_SD_SKIP_VALUES case: window[0]=channel,
// window[1..2]=count (2-byte BE). Advances the channel's read position
// past that many whole chunks without transferring their content --
// reads each chunk's own tag/length bytes only, using them to compute how
// far to seek. If the channel runs out before finishing the count, the
// read position is left completely unchanged (all-or-nothing) and
// kStatusError is returned -- SDSKIP# (rom.asm) raises a real ERROR 40
// for that.
uint8_t ExpansionMock::skipSdValues(std::vector<uint8_t>& window) {
  if (window.size() < 3) return kStatusError;
  uint8_t channel = window[0];
  if (channel < 1 || channel > kMaxSdChannels) return kStatusError;
  SdChannel& ch = channels_[channel - 1];
  if (!ch.isOpen) return kStatusError;
  uint16_t count = (static_cast<uint16_t>(window[1]) << 8) | window[2];

  uint32_t pos = ch.readPos;
  for (uint16_t i = 0; i < count; i++) {
    ch.file.clear();
    ch.file.seekg(static_cast<std::streamoff>(pos), std::ios::beg);
    char tag = 0;
    ch.file.read(&tag, 1);
    if (ch.file.gcount() != 1) return kStatusError;  // ran out -- readPos left unchanged
    if (tag == 'N') {
      pos += 1 + 8;
    } else if (tag == 'S') {
      char lenByte = 0;
      ch.file.read(&lenByte, 1);
      if (ch.file.gcount() != 1) return kStatusError;
      pos += 1 + 1 + static_cast<uint8_t>(lenByte);
    } else {
      return kStatusError;  // corrupt file
    }
  }
  ch.readPos = pos;
  return kStatusSuccess;
}

// Ported from main.c's EXP_COMMAND_VALIDATE_SD_NAME case (and, before that,
// from the LH5801 state machine that used to live in rom.asm's
// SD_PARSE_QUOTED_NAME) -- see this method's own header comment for the
// exact per-'/'-segment 8.3 shape rule. window[0..1]=length (2-byte BE),
// window[2..]=the raw characters, folded/validated in place on success.
uint8_t ExpansionMock::validateAndFoldSdName(std::vector<uint8_t>& window) {
  if (window.size() < 2) return kStatusError;
  size_t nameLen = (static_cast<size_t>(window[0]) << 8) | window[1];
  if (nameLen == 0 || window.size() < 2 + nameLen) return kStatusError;

  uint8_t nameCount = 0, extCount = 0;
  bool dotSeen = false, dotOnly = true;
  for (size_t i = 0; i < nameLen; i++) {
    uint8_t c = window[2 + i];
    if (c >= 'a' && c <= 'z') c = static_cast<uint8_t>(c - 'a' + 'A');
    if (c == '/') {
      nameCount = 0;
      extCount = 0;
      dotSeen = false;
      dotOnly = true;
    } else if (c == '.') {
      if (dotOnly) {
        // "." or ".." so far -- allow, don't touch counters
      } else if (dotSeen) {
        return kStatusError;  // second '.' in a real name
      } else {
        dotSeen = true;
      }
    } else {
      dotOnly = false;
      if (!dotSeen) {
        if (++nameCount > 8) return kStatusError;
      } else {
        if (++extCount > 3) return kStatusError;
      }
    }
    window[2 + i] = c;
  }
  return kStatusSuccess;
}

// Ported from main.c's EXP_COMMAND_GET_SD_DF_TEXT case: pre-rendered
// "<free>F / <total>T" text, matching listSdDir's own summary-line "B"/"F"
// suffix convention -- the ROM has no decimal-to-ASCII conversion of its
// own, so this arrives pre-formatted rather than as the raw 4-byte values
// getSdFreeSpace/getSdVolumeSize return. Kept in sync by hand with
// main.c's own EXP_COMMAND_GET_SD_DF_TEXT case, same caveat as
// listSdDir's own summary line.
uint8_t ExpansionMock::getSdDfText(std::vector<uint8_t>& window) {
  if (rootDir_.empty()) return kStatusError;
  std::error_code ec;
  fs::space_info info = fs::space(rootDir_, ec);
  if (ec) return kStatusError;
  uint32_t freeBytes = static_cast<uint32_t>(info.available);
  uint32_t totalBytes = static_cast<uint32_t>(info.capacity);
  std::string text = std::to_string(freeBytes) + "F / " + std::to_string(totalBytes) + "T";
  writeLengthPrefixedString(text, window, kScratchOffset);
  return kStatusSuccess;
}

}  // namespace pc1500
