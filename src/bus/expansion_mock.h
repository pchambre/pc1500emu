// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace pc1500 {

// Mocks a real expansion-board MCU's command processing (e.g. the
// PC1500-PSOC5 project's RP2350 monitor.c DoCommand()) for a Bus::RomModule
// with a writable data window. Bus calls processCommand() whenever the CPU
// writes to that module's configured instruction address -- same contract
// as the real board's DoCommand()/WriteStatus() protocol (write a command
// byte, poll the same address for a non-BUSY status). Genuinely
// asynchronous (2026-09-17, see processCommand()'s own comment): a
// background thread does the real work while the calling/main thread
// returns immediately, mirroring the real firmware's own core0/core1
// split -- needed so a ROM-side wait loop (HLT/timer-poll or a tight
// busy-loop, either way) has real, multi-instruction-cycle BUSY duration
// to actually exercise, not an already-resolved status the instant it
// checks. SD-card commands are backed by a real directory on the host
// filesystem (setRootDir()) rather than an in-memory list, so the still-stub
// SSAVE/SLOAD/SRM/SDF ROM routines have something real to develop and test
// against -- drop files into that directory and SLS/SLOAD see them; SSAVE
// writes real files there.
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

  // Simulates a slow real-world SD link for testing ROM/BASIC-side
  // robustness (polling loops, watchdog timeouts, perceived UI
  // responsiveness) without needing real hardware -- see the RP2350
  // Pico2W dongle's own 2026-09 session, where the real SC18IS602B
  // I2C-bridge link turned out to run at only ~5-10KB/s in practice.
  // 0 (default) = unlimited/instant, this mock's original behavior.
  // Applies to every real file read/write's actual transferred byte
  // count (readFromSdFile/writeToSdFile) via a real
  // std::this_thread::sleep_for() -- safe now that command dispatch runs
  // on its own worker thread (see processCommand()'s own comment), so
  // this genuinely holds BUSY for the simulated duration instead of
  // blocking the caller/main thread. Also applies to
  // simulatedFatScanBytes() below, since real hardware's own dominant
  // SDLS cost turned out to be free-space accounting, not file data
  // volume -- see that setter's own comment.
  void setSdRateLimitBytesPerSec(uint32_t bytesPerSec) { sdRateLimitBytesPerSec_ = bytesPerSec; }
  uint32_t sdRateLimitBytesPerSec() const { return sdRateLimitBytesPerSec_; }

  // Real hardware's own SDLS slowness (2026-09 session) traced to FatFs's
  // f_getfree() doing a full FAT-table scan on the first free-space query
  // per mount/boot -- proportional to the card/FAT's own size, not to how
  // many files are listed or their sizes, so it can't be modeled as an
  // ordinary byte-count charge on listSdDir()'s own directory entries.
  // 0 (default) = no simulated scan cost. Charged (at the same simulated
  // rate as setSdRateLimitBytesPerSec()) every time free space is
  // computed -- by both listSdDir()'s own summary line and
  // getSdFreeSpace() -- not just once per mount; this mock doesn't model
  // FatFs's own free-cluster-count caching, so set this to 0 between
  // calls in a test that specifically wants to observe a "second call is
  // fast" pattern.
  void setSimulatedFatScanBytes(uint32_t bytes) { simulatedFatScanBytes_ = bytes; }
  uint32_t simulatedFatScanBytes() const { return simulatedFatScanBytes_; }

  // Starts processing `cmd` against `window` (a module's own writable
  // data-window buffer, indexed from 0 -- i.e. window[0] is
  // EXP_BUFFER_START_PAGE/EXP_BUFFER_START_ADDRESS, window[256] is
  // EXP_SCRATCH_PAGE/0, in PC_EXP.h's own addressing) on a background
  // thread and returns immediately -- mirrors the real RP2350 firmware's
  // own core0-hands-off-to-core1 architecture (2026-09-17; the old,
  // fully-synchronous version couldn't hold BUSY for any real duration,
  // so it never exercised the ROM's own HLT/status-poll wait loop at
  // all). `instructionOffset` is where the status byte lives within
  // `window` (Bus already knows this as m.instructionAddr-m.dataWindowBase).
  // Stamps BUSY into `window` synchronously before returning; see
  // pollStatus() for how a caller observes the real final status once the
  // worker thread reports it, and this class's own .cpp comment for the
  // full synchronization contract.
  void processCommand(uint8_t cmd, std::vector<uint8_t>& window, size_t instructionOffset);

  // The status of whatever command processCommand() most recently began
  // -- Bus::readME0() serves this (not window[instructionOffset] read
  // directly) on every read of a loaded module's instruction address, so
  // a real in-progress delay is genuinely observable as BUSY rather than
  // Bus reading back whatever the worker thread's own writes have gotten
  // to so far. See this class's own .cpp comment for the acquire/release
  // pairing this relies on.
  uint8_t pollStatus() const { return pendingStatus_.load(std::memory_order_acquire); }

  // Test-only: blocks until whatever command processCommand() most
  // recently started has fully finished. Real ROM/CPU code never needs
  // this -- it always polls pollStatus() instead, the same way real
  // hardware polls the status byte -- but a test calling processCommand()
  // directly (bypassing Bus/the CPU entirely) needs an explicit way to
  // wait for the async result before checking pollStatus().
  void waitUntilIdleForTest() {
    if (worker_.joinable()) worker_.join();
  }

  ~ExpansionMock();

  // PC_EXP.h mirrors -- see that file for the authoritative definitions.
  // 4, not 0, since 2026-09-24 -- a sleeping RP2350 leaves the data window
  // reading 0xFF, and READY must differ from both 0x00 and 0xFF (see the
  // real firmware's pc_exp.h EXP_STATUS_READY/EXP_COMMAND_DONE).
  static constexpr uint8_t kStatusReady = 4;
  static constexpr uint8_t kStatusBusy = 1;
  static constexpr uint8_t kStatusSuccess = 2;
  static constexpr uint8_t kStatusEof = 3;  // kCommandSdReadValue only -- see PC_EXP.h's own comment
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
  static constexpr uint8_t kCommandTestDelay = 13;  // diagnostic only, see PC_EXP.h's own comment
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
  static constexpr uint8_t kCommandSdOpenChannel = 24;
  static constexpr uint8_t kCommandSdCloseChannel = 25;
  static constexpr uint8_t kCommandSdListChannels = 26;
  static constexpr uint8_t kCommandSdWriteValue = 27;
  static constexpr uint8_t kCommandSdReadValue = 28;
  static constexpr uint8_t kCommandSdSkipValues = 29;
  // Validates+uppercase-folds, in place, the length-prefixed raw name
  // SD_PARSE_QUOTED_NAME (rom.asm) has already staged at window offset 0
  // -- ported from the LH5801 state machine that used to live there (see
  // validateAndFoldSdName's own comment for the full rule). Moved
  // 2026-08-19: pure character classification, much more naturally
  // expressed in C++, and this side already receives the full name for
  // every command that uses one.
  static constexpr uint8_t kCommandValidateSdName = 30;

  // STAGE keyword support (2026-09-23) -- mocks the real board's GreenPAK-
  // driven "Remap" mechanism: normally the 6K ROM region (this module's own
  // `data`) answers every read; STAGE tells the (real) GreenPAK to instead
  // answer that same address range directly from an external SRAM chip,
  // bypassing the ROM image entirely until reverted. Here, `remapActive_`
  // is that same on/off switch, and `sram_` is the mock stand-in for the
  // external chip -- see Bus::readME0/writeME0 for the actual address-range
  // redirection this flag gates (RomModule itself stays unaware of any of
  // this, same as real hardware's own GreenPAK-vs-MCU split). Real
  // PC_EXP.h values, kept in sync by hand like every other constant here.
  static constexpr uint8_t kCommandRomFromMcu = 0x20;
  static constexpr uint8_t kCommandRomFromSram = 0x21;
  static constexpr uint8_t kCommandRomCopyBegin = 0x22;
  static constexpr uint8_t kCommandRomCopyGetBlock = 0x23;
  static constexpr uint8_t kCommandRomCopyFinish = 0x24;
  static constexpr uint8_t kCommandRomGetMode = 0x25;
  // Diagnostic-only on real hardware (logged, never gates STAGE's own
  // outcome) -- mocked as accept-and-succeed, no actual logging needed.
  static constexpr uint8_t kCommandLogBlockChecksum = 0x29;
  static constexpr uint8_t kCommandStageByteMismatch = 0x2A;

  // End-of-keyword marker (2026-09-24) -- the real RP2350 goes DORMANT on
  // this in STAGE RAM mode. The mock has no sleep to model, so it just
  // succeeds; rom.asm sends it from KEYWORD_RETURN and SD_RAISE_ERROR_*.
  static constexpr uint8_t kCommandDone = 0x2C;
  // MLOGMSG (2026-09-24): 'S' + length + characters at window[1]. The
  // mock has no MCU log, so it just records the (truncated) text for
  // tests -- see lastUserLogMessage().
  static constexpr uint8_t kCommandLogUserMessage = 0x2D;

  // MLOG (RP2350 only). The mock keeps just the VERBOSE/QUIET flag and an
  // always-empty log.
  static constexpr uint8_t kCommandLogList = 0x26;
  static constexpr uint8_t kCommandLogClear = 0x27;
  static constexpr uint8_t kCommandLogSetInfoEnabled = 0x28;
  static constexpr uint8_t kCommandLogGetInfoEnabled = 0x2B;

  // Keyword executor (2026-09-25) -- the expansion ROM hands each keyword's
  // raw argument text to the MCU, which parses it and returns actions for
  // the ROM to carry out. Handled by the real firmware's own keywords.c,
  // compiled into this library (see src/bus/CMakeLists.txt).
  static constexpr uint8_t kCommandKeyword = 0x2E;
  static constexpr uint8_t kCommandKeywordContinue = 0x2F;

  // MCONF settings (2026-09-25): byte 0 = setting number, bytes 1-2 = BE
  // value. The mock keeps them in memory (real firmware: flash).
  static constexpr uint8_t kCommandConfigGet = 0x30;
  static constexpr uint8_t kCommandConfigSet = 0x31;
  static constexpr int kConfigCount = 2;  // LED, SLEEPWAIT -- mcu_config.h

  static constexpr uint8_t kCommandClearStatus = 0xFF;

  // EXP_BLOCK_CHECKSUM_PAGE(7)*256 + EXP_BLOCK_CHECKSUM_ADDRESS(0xFB) -
  // window base -- 2-byte BE per-block checksum GET_BLOCK stamps into the
  // window alongside the payload, same page as kLengthPortOffset, just
  // before it. Matches PC_EXP.h's own EXP_BLOCK_CHECKSUM_ABS layout.
  static constexpr int kBlockChecksumOffset = 0x7FB;

  // Sets the module's own ROM image bytes for STAGE's GET_BLOCK to copy
  // from, and (re)sizes+resets the mock SRAM chip to match (0xFF-filled,
  // matching real RAM's confirmed power-up default -- same convention
  // loadExpansionModule() already uses for a fresh dataWindow). Called by
  // Bus::loadExpansionModule() right after it sets the module's own
  // `data`, so this never needs to be called separately -- mirrors
  // setRootDir()'s own "setter called once at load time" shape.
  void setRomImage(std::vector<uint8_t> image) {
    romImage_ = std::move(image);
    sram_.assign(romImage_.size(), 0xFF);
    remapActive_ = false;
    romCopyActive_ = false;
    romCopyBlockIndex_ = 0;
    romStagedVerified_ = false;
    romCopyBeginCount_ = 0;
  }

  // Mirrors the real firmware's romStagedVerified (monitor.c, 2026-09-24):
  // true only after a ROM_COPY_FINISH whose checksum matched, cleared by
  // BEGIN/ROM_FROM_MCU -- ROM_GET_MODE's second response byte, which the
  // ROM's boot hook and STAGE RAM use to skip re-staging.
  bool romStagedVerified() const { return romStagedVerified_; }
  // Test-only: how many ROM_COPY_BEGINs have been processed since the
  // image was loaded -- lets a test confirm a copy was (or wasn't) run.
  int romCopyBeginCount() const { return romCopyBeginCount_; }

  // Test-only: the text of the most recent MLOGMSG, as the real firmware
  // would log it (truncated to 23 characters).
  std::string lastUserLogMessage() const { return lastUserLogMessage_; }

  // Test-only: an MCONF setting's current value (0 = LED, 1 = SLEEPWAIT).
  uint16_t configValue(int id) const { return id >= 0 && id < kConfigCount ? config_[id] : 0; }

  // Whether the mock GreenPAK's Remap is currently active -- Bus::readME0/
  // writeME0 check this before falling back to the module's own static ROM
  // image, exactly mirroring the priority `tryReadWindow` already has over
  // `tryRead` for the 2K data window.
  bool remapActive() const { return remapActive_; }

  // Bounds-checked SRAM accessors for Bus::readME0/writeME0 -- an
  // out-of-range read returns 0xFF (matching an empty/unmapped socket
  // elsewhere in Bus), an out-of-range write is silently dropped (should
  // never actually happen if the caller's own bankSize math is right, but
  // cheap to guard rather than trust every call site).
  uint8_t sramByte(size_t offset) const { return offset < sram_.size() ? sram_[offset] : 0xFF; }
  void setSramByte(size_t offset, uint8_t value) {
    if (offset < sram_.size()) sram_[offset] = value;
  }

  // SDOPEN/SDCLOSE/SDINPUT#/SDPRINT#/SDSKIP# -- up to kMaxSdChannels files
  // open at once, numbered 1..kMaxSdChannels (0 is the "close all"
  // sentinel). This mock never sees a BASIC variable name or value --
  // rom.asm resolves those itself and sends/receives opaque "chunk" bytes
  // (['N']+8 raw bytes for numeric, ['S']+1-byte length+that many raw
  // ASCII bytes for string) -- see PC_EXP.h's own comment for the full
  // writeup and the reasoning behind the format.
  static constexpr int kMaxSdChannels = 16;

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

  // EXP_LENGTH_PORT_PAGE/ADDRESS -- readFromSdFile/writeToSdFile's 2-byte BE
  // length value, deliberately outside the payload region. Same page as the
  // instruction byte (EXP_INSTRUCTION_PAGE*256+EXP_INSTRUCTION_ADDRESS =
  // 2047, not part of this window/vector -- Bus special-cases that one
  // address), just before it. 2026 session: widened the single-call payload
  // cap from 254 to kMaxTransferLen, moving the length out of page 0 so the
  // whole 4-page payload region can be pure data with no +2 offset.
  static constexpr int kLengthPortOffset = 0x7FD;  // = EXP_LENGTH_PORT_PAGE*256 +
                                                     // EXP_LENGTH_PORT_ADDRESS - window base
  static constexpr int kMaxTransferLen = 1024;      // matches PC_EXP.h's EXP_MAX_TRANSFER_LEN

  static constexpr int kDirNameLen = 16;
  static constexpr int kDirSizeTextLen = 10;
  static constexpr int kDirRecordSize = 30;
  // Summary line rendered right after the last directory entry -- see
  // listSdDir's own comment. 26 matches SLS_LINE_WIDTH (rom.asm) -- the
  // real PC-1500 LCD's own max single-line width (156 dots / 6 per char).
  static constexpr int kSummaryLineLen = 26;
  // (2048 - 1 - 2 - kSummaryLineLen) / kDirRecordSize = 67 -- leaves room
  // for entries *and* the summary line inside the 2K data window (shrunk
  // from 4K when ROM_BASE moved 0x9000->0x8800 to grow the ROM region to
  // 6K, 2026-08-18 session), clear of the instruction byte at its last
  // address. Unchanged by the kLengthPortOffset addition (same slack
  // absorbs the extra 2 reserved bytes -- see PC_EXP.h's own comment).
  // Matches PC_EXP.h's own mirror.
  static constexpr int kDirMaxEntries = 66;  // 67 until 2026-09-25 (keyword action block)

 private:
  // The actual per-command work -- unchanged body from the old
  // synchronous processCommand() this was renamed from; now called only
  // from runCommandAsync() on the worker thread, never directly.
  uint8_t dispatchCommand(uint8_t cmd, std::vector<uint8_t>& window);
  // Worker-thread entry point: runs dispatchCommand(), then publishes the
  // real final status both into *window and pendingStatus_ (release
  // order) -- see processCommand()'s own .cpp comment for the full
  // synchronization contract this pairs with.
  void runCommandAsync(uint8_t cmd, std::vector<uint8_t>* window, size_t instructionOffset);

  // Sleeps for bytes/sdRateLimitBytesPerSec_ (real wall-clock time -- see
  // setSdRateLimitBytesPerSec()'s own comment for why that's safe here)
  // if a rate limit is set; a no-op (0 == unlimited) otherwise. Called
  // from dispatchCommand()'s own worker thread, never the caller's.
  void throttleForBytes(uint32_t bytes) const;

  std::atomic<uint8_t> pendingStatus_{kStatusReady};
  std::thread worker_;
  uint32_t sdRateLimitBytesPerSec_ = 0;
  uint32_t simulatedFatScanBytes_ = 0;

  static void formatSizeText(uint32_t value, std::vector<uint8_t>& window, size_t offset,
                              int width);
  static void writeText(const std::string& text, std::vector<uint8_t>& window, size_t offset,
                         int width);
  static std::string readLengthPrefixedString(const std::vector<uint8_t>& window,
                                               size_t offset = 0);
  static void writeLengthPrefixedString(const std::string& text, std::vector<uint8_t>& window,
                                         size_t offset);

  // '+' is this project's typable stand-in for a real FAT short name's '~'
  // (the PC-1500 keyboard has no '~' key) -- see validateAndFoldSdName's
  // own comment. Every SD command now enforces uppercase 8.3 shape on its
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
  uint8_t openSdChannel(std::vector<uint8_t>& window);
  uint8_t closeSdChannel(std::vector<uint8_t>& window);
  uint8_t listSdChannels(std::vector<uint8_t>& window);
  uint8_t writeSdValue(std::vector<uint8_t>& window);
  uint8_t readSdValue(std::vector<uint8_t>& window);
  uint8_t skipSdValues(std::vector<uint8_t>& window);
  uint8_t romFromMcu(std::vector<uint8_t>& window);
  uint8_t romFromSram(std::vector<uint8_t>& window);
  uint8_t romCopyBegin(std::vector<uint8_t>& window);
  uint8_t romCopyGetBlock(std::vector<uint8_t>& window);
  uint8_t romCopyFinish(std::vector<uint8_t>& window);
  uint8_t romGetMode(std::vector<uint8_t>& window);
  uint8_t logBlockChecksum(std::vector<uint8_t>& window);
  uint8_t stageByteMismatch(std::vector<uint8_t>& window);
  // Closes channels_[index] if open -- shared by closeSdChannel (one or
  // "all") and openSdChannel (reusing an already-open number).
  void closeSdChannelAt(int index);
  // Validates+uppercase-folds, in place, the length-prefixed raw name at
  // window offset 0 -- every SD command's name argument is a full path (a
  // plain filename, a relative path with '/'/'.'/'..' components, or an
  // absolute one starting with '/' from the SD root), so each
  // '/'-separated segment must independently be <=8 characters,
  // optionally followed by '.' and <=3 more, with at most one '.' --
  // except a segment that is exactly "." or "..", always allowed through
  // untouched. '+' needs no special handling here -- it's an ordinary
  // character for shape-counting purposes; the actual '+'<->'~'
  // translation happens later, in resolvePath/resolveDirPath. Returns
  // kStatusSuccess (window already updated in place) or kStatusError
  // (shape violation -- rom.asm's SD_PARSE_QUOTED_NAME maps this onto the
  // same "malformed" Carry-set exit it always had).
  uint8_t validateAndFoldSdName(std::vector<uint8_t>& window);

  // STAGE/Remap mock state -- see setRomImage()/remapActive()/sramByte()'s
  // own comments above.
  std::vector<uint8_t> romImage_;
  std::vector<uint8_t> sram_;
  bool remapActive_ = false;
  bool romCopyActive_ = false;
  int romCopyBlockIndex_ = 0;
  bool romStagedVerified_ = false;
  int romCopyBeginCount_ = 0;
  std::string lastUserLogMessage_;
  bool logInfoEnabled_ = false;
  uint16_t config_[kConfigCount] = {1, 0};  // mcu_config.c's defaults
  // The window of the keyword command in progress, for runKeywordCommand().
  std::vector<uint8_t>* kwWindow_ = nullptr;
  static uint8_t runKeywordCommand(uint8_t cmd, void* ctx);

  std::filesystem::path rootDir_;
  // Defaults to rootDir_ whenever that's (re)set -- see setRootDir's own
  // comment. Always an absolute path under rootDir_, never a bare relative
  // fragment, so resolvePath can just join a name onto it directly.
  std::filesystem::path currentDir_;
  uint32_t freeSpaceBytes_ = 2122343;

  // Single open-file state -- matches main.c's own globals exactly (one
  // file open at a time, not a handle table), since that's what
  // SDLOAD/SDSAVE's own established ROM protocol assumes. Entirely
  // separate from channels_ below (SDOPEN/etc.'s own real handle table) --
  // the two mechanisms don't interact.
  std::fstream openFile_;
  std::string openFileName_;
  uint8_t fileStatus_ = kFileStatusClosed;
  uint32_t bytesWrittenTotal_ = 0;  // matches main.c's fileEnd

  // SDOPEN/SDCLOSE/SDINPUT#/SDPRINT#/SDSKIP#'s own real multi-file state --
  // index i holds channel (i+1). name_ is kept only for listSdChannels'
  // own display; readPos_ is the persistent SDINPUT#/SDSKIP# cursor,
  // deliberately independent of the file's own internal position (which
  // writeSdValue moves to the end and back on every SDPRINT# call).
  struct SdChannel {
    std::fstream file;
    std::string name;
    bool isOpen = false;
    uint32_t readPos = 0;
  };
  std::array<SdChannel, kMaxSdChannels> channels_;
};

}  // namespace pc1500
