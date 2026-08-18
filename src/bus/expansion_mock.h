// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace pc1500 {

// Mocks a real expansion-board MCU's command processing (e.g. the
// PC1500-PSOC5 project's main.c DoCommand()) for a Bus::RomModule with a
// writable data window. Bus calls processCommand() synchronously whenever
// the CPU writes to that module's configured instruction address -- same
// contract as the real board's DoCommand(). SD-card commands are backed by
// a real directory on the host filesystem (setRootDir()) rather than an
// in-memory list, so the still-stub SSAVE/SLOAD/SRM/SDF ROM routines have
// something real to develop and test against -- drop files into that
// directory and SLS/SLOAD see them; SSAVE writes real files there.
// Command/status constants below mirror PC_EXP.h exactly; keep them in
// sync by hand (same caveat as rom_defs.inc's own mirror of PC_EXP.h).
class ExpansionMock {
 public:
  // Root directory standing in for the SD card. Empty (the default) means
  // "no card inserted" -- every SD command below reports EXP_STATUS_ERROR,
  // which is itself a useful state to be able to test. `dir` is used as-is
  // (not created); the caller is responsible for it existing. Resets the
  // current directory (see changeSdDir) back to the root, matching a fresh
  // card insert -- any prior SDCD navigation doesn't carry over.
  void setRootDir(std::filesystem::path dir) {
    rootDir_ = std::move(dir);
    currentDir_ = rootDir_;
  }
  const std::filesystem::path& rootDir() const { return rootDir_; }

  // Current directory, as last set by CHANGE_SD_DIR (or rootDir_ itself if
  // never called) -- every other SD command that takes a bare filename
  // (createSdFile, openSdFileRead, listSdDir, ...) resolves relative to
  // this, matching real emFile's single global FS_ChDir concept (see
  // PC_EXP.h's own comment).
  const std::filesystem::path& currentDir() const { return currentDir_; }

  // Mock "bytes free" fallback for listSdDir's summary line when no real
  // rootDir_ is configured (matches earlier, pre-filesystem behavior);
  // once a rootDir_ is set, GET_SD_FREE_SPACE/the summary line use real
  // std::filesystem::space() numbers instead.
  void setFreeSpaceBytes(uint32_t bytes) { freeSpaceBytes_ = bytes; }
  uint32_t freeSpaceBytes() const { return freeSpaceBytes_; }

  // Processes `cmd` against `window` (a module's own writable data-window
  // buffer, indexed from 0 -- i.e. window[0] is EXP_BUFFER_START_PAGE/
  // EXP_BUFFER_START_ADDRESS, window[256] is EXP_SCRATCH_PAGE/0, in
  // PC_EXP.h's own addressing). Returns the final status byte to store
  // back at the instruction address, exactly like DoCommand()'s own
  // WriteStatus() calls -- BUSY is never externally observable here since
  // (unlike real SD I/O) every command completes within the one call.
  uint8_t processCommand(uint8_t cmd, std::vector<uint8_t>& window);

  // PC_EXP.h mirrors -- see that file for the authoritative definitions.
  static constexpr uint8_t kStatusReady = 0;
  static constexpr uint8_t kStatusBusy = 1;
  static constexpr uint8_t kStatusSuccess = 2;
  static constexpr uint8_t kStatusNotImplemented = 64;
  static constexpr uint8_t kStatusError = 128;

  static constexpr uint8_t kCommandGetSdFreeSpace = 1;
  static constexpr uint8_t kCommandCreateSdFile = 2;
  static constexpr uint8_t kCommandWriteToSdFile = 3;
  static constexpr uint8_t kCommandCloseSdFile = 4;
  static constexpr uint8_t kCommandGetSdFileSize = 5;
  static constexpr uint8_t kCommandReadSdVolumeLabel = 6;
  static constexpr uint8_t kCommandGetSdFileName = 7;
  static constexpr uint8_t kCommandGetSdFileStatus = 8;
  static constexpr uint8_t kCommandFormatSdCard = 9;
  static constexpr uint8_t kCommandOpenSdFileRead = 10;
  static constexpr uint8_t kCommandReadFromSdFile = 11;
  static constexpr uint8_t kCommandListSdDir = 12;
  static constexpr uint8_t kCommandRemoveSdFile = 14;
  static constexpr uint8_t kCommandGetSdVolumeSize = 15;
  static constexpr uint8_t kCommandChangeSdDir = 16;
  static constexpr uint8_t kCommandMakeSdDir = 17;
  static constexpr uint8_t kCommandRemoveSdDir = 18;
  static constexpr uint8_t kCommandGetSdCwd = 19;
  static constexpr uint8_t kCommandCopySdFile = 20;
  static constexpr uint8_t kCommandMoveSdFile = 21;
  static constexpr uint8_t kCommandGetSdDfText = 22;
  static constexpr uint8_t kCommandCheckSdCopyMoveDestExists = 23;
  static constexpr uint8_t kCommandClearStatus = 0xFF;

  // Max length of a single quoted path/name argument -- see PC_EXP.h's own
  // comment. Deliberately separate from kDirNameLen (the SDLS display
  // column width, unrelated). Every SD command accepts a full path now: a
  // plain filename, a relative path ("SUB/FILE.BAS", "../FILE.BAS"), or
  // an absolute one from the SD root ("/SUB/FILE.BAS") -- see
  // resolvePath's own comment.
  static constexpr int kPathArgLen = 40;

  // SDCP/SDMV's wire layout: two fixed kTwoNameSlotLen-byte slots
  // back-to-back at window offset 0 (source, then destination), each
  // shaped like any other quoted-name argument (2-byte BE length + up to
  // kPathArgLen bytes) -- see PC_EXP.h's own comment for why fixed-width.
  static constexpr int kTwoNameSlotLen = 2 + kPathArgLen;

  static constexpr uint8_t kFileStatusClosed = 0;
  static constexpr uint8_t kFileStatusOpenWrite = 1;
  static constexpr uint8_t kFileStatusOpenRead = 2;

  static constexpr int kScratchOffset = 256;  // EXP_SCRATCH_PAGE(1) * 256

  static constexpr int kDirNameLen = 16;
  static constexpr int kDirSizeTextLen = 10;
  static constexpr int kDirRecordSize = 30;
  // Summary line rendered right after the last directory entry -- see
  // listSdDir's own comment. 26 matches SLS_LINE_WIDTH (rom.asm) -- the
  // real PC-1500 LCD's own max single-line width (156 dots / 6 per char).
  static constexpr int kSummaryLineLen = 26;
  // (4096 - 1 - 2 - kSummaryLineLen) / kDirRecordSize = 135 -- leaves room
  // for entries *and* the summary line inside the 4K window, clear of the
  // instruction byte at its last address. Matches PC_EXP.h's own mirror.
  static constexpr int kDirMaxEntries = 135;

 private:
  static void formatSizeText(uint32_t value, std::vector<uint8_t>& window, size_t offset,
                              int width);
  static void writeText(const std::string& text, std::vector<uint8_t>& window, size_t offset,
                         int width);
  static std::string readLengthPrefixedString(const std::vector<uint8_t>& window,
                                               size_t offset = 0);
  static void writeLengthPrefixedString(const std::string& text, std::vector<uint8_t>& window,
                                         size_t offset);

  // '+' is this project's typable stand-in for a real FAT short name's '~'
  // (the PC-1500 keyboard has no '~' key) -- see rom.asm's SD_PARSE_QUOTED_
  // NAME comment. Every SD command now enforces uppercase 8.3 shape on its
  // own argument, so '+' can never legitimately appear in a name for any
  // other reason: the swap is unconditional and unambiguous in both
  // directions, unlike '-' (a legal FAT 8.3 character that could collide
  // with a real hyphenated name). convertPlusToTilde is applied to any name
  // arriving from the wire before it touches the real filesystem (inside
  // resolvePath); convertTildeToPlus is applied to any real on-disk name
  // before it's staged back onto the wire (listSdDir, getSdCwd).
  static std::string convertPlusToTilde(const std::string& name);
  static std::string convertTildeToPlus(const std::string& name);

  // Validates `name` (untrusted, straight off the wire) and resolves it to
  // a real path, or returns an empty path if `name` is unsafe (raw Windows
  // separators/drive letters, or would resolve outside rootDir_) or no
  // rootDir_ is configured. `name` may be a plain filename (resolved
  // against currentDir_), a relative path with '.'/'..'/multiple
  // components (e.g. "SUB/DIR", "../OTHER"), or an absolute one starting
  // with '/' (resolved against rootDir_ -- "/" itself means the SD root).
  // Every SD command goes through this now (SDLOAD/SDSAVE/SDRM/SDCD/
  // SDMKDIR/SDRMDIR/SDCP/SDMV's own source and, via
  // resolveCopyOrMoveDestination below, destination too) -- previously
  // plain-filename commands (resolvePath) and directory commands
  // (resolveDirPath) had separate, near-duplicate implementations; merged
  // once both needed the same relative+absolute path support. The
  // weakly_canonical containment check below is the primary defense
  // against escaping the sandbox (not just defense in depth) -- this is a
  // local dev tool, not network-exposed, but a ROM bug writing/deleting
  // files outside the intended sandbox directory would be a bad surprise
  // worth deliberately preventing.
  std::filesystem::path resolvePath(const std::string& name) const;

  // For SDCP/SDMV's destination argument: resolves `destArg` via
  // resolvePath, then -- if that resolves to an *existing directory* --
  // returns (that directory)/srcBasename instead, matching Unix cp/mv's
  // own "copy/move INTO a directory" behavior. `srcBasename` is the
  // source's own filename component (SDCP/SDMV's source is itself a full
  // path now, so this is its resolved path's .filename(), not the raw
  // argument). Returns an empty path if destArg doesn't resolve.
  std::filesystem::path resolveCopyOrMoveDestination(const std::string& srcBasename,
                                                       const std::string& destArg) const;

  uint8_t listSdDir(std::vector<uint8_t>& window);
  uint8_t getSdFreeSpace(std::vector<uint8_t>& window);
  uint8_t getSdVolumeSize(std::vector<uint8_t>& window);
  uint8_t createSdFile(std::vector<uint8_t>& window);
  uint8_t openSdFileRead(std::vector<uint8_t>& window);
  uint8_t writeToSdFile(std::vector<uint8_t>& window);
  uint8_t readFromSdFile(std::vector<uint8_t>& window);
  uint8_t closeSdFile(std::vector<uint8_t>& window);
  uint8_t getSdFileSize(std::vector<uint8_t>& window);
  uint8_t getSdFileStatus(std::vector<uint8_t>& window);
  uint8_t getSdFileName(std::vector<uint8_t>& window);
  uint8_t removeSdFile(std::vector<uint8_t>& window);
  uint8_t readSdVolumeLabel(std::vector<uint8_t>& window);
  uint8_t formatSdCard(std::vector<uint8_t>& window);
  uint8_t changeSdDir(std::vector<uint8_t>& window);
  uint8_t makeSdDir(std::vector<uint8_t>& window);
  uint8_t removeSdDir(std::vector<uint8_t>& window);
  uint8_t getSdCwd(std::vector<uint8_t>& window);
  uint8_t copySdFile(std::vector<uint8_t>& window);
  uint8_t moveSdFile(std::vector<uint8_t>& window);
  uint8_t getSdDfText(std::vector<uint8_t>& window);
  uint8_t checkSdCopyMoveDestExists(std::vector<uint8_t>& window);

  std::filesystem::path rootDir_;
  // Defaults to rootDir_ whenever that's (re)set -- see setRootDir's own
  // comment. Always an absolute path under rootDir_, never a bare relative
  // fragment, so resolvePath can just join a name onto it directly.
  std::filesystem::path currentDir_;
  uint32_t freeSpaceBytes_ = 2122343;

  // Single open-file state -- matches main.c's own globals exactly (one
  // file open at a time, not a handle table), since that's what the real
  // ROM protocol assumes.
  std::fstream openFile_;
  std::string openFileName_;
  uint8_t fileStatus_ = kFileStatusClosed;
  uint32_t bytesWrittenTotal_ = 0;  // matches main.c's fileEnd
};

}  // namespace pc1500
