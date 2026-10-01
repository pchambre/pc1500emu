#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

namespace pc1500 {

// The CE-150 stand-in's paper (2026-09-30): what the PC-1500 Link's PLOT
// frames (RP2350/BLE_PROTOCOL.md, "Plotter") have drawn -- lines in four
// pens, in quarter steps (0.05 mm), x 0-860 across the paper, y up it.
// Fed by the thread that receives the frames, read by the UI's; every
// member takes the lock.
class PlotPaper {
 public:
  struct Line {
    uint8_t pen;
    int32_t x0, y0, x1, y1;
  };

  // One PLOT payload, decoded and drawn (the expansion firmware's
  // plot_decode()). False for a malformed one, which then adds nothing
  // past where it went wrong.
  bool addPayload(const uint8_t* payload, size_t len);

  std::vector<Line> lines() const;
  size_t lineCount() const;
  // Where the pen rests, and whether anything has come yet.
  bool penKnown() const;
  int32_t penX() const;
  int32_t penY() const;
  uint8_t pen() const;
  // The y range everything so far spans (the pen's rest included).
  void extent(int32_t* top, int32_t* bottom) const;
  // Grows whenever anything changes, so a viewer knows to redraw/scroll.
  uint64_t version() const;
  void clear();

 private:
  mutable std::mutex mutex_;
  std::vector<Line> lines_;
  bool penKnown_ = false;
  uint8_t pen_ = 0;
  int32_t x_ = 0, y_ = 0;
  int32_t top_ = 0, bottom_ = 0;
  uint64_t version_ = 0;
};

}  // namespace pc1500
