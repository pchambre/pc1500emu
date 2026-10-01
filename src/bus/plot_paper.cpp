#include "plot_paper.h"

#include <algorithm>

#ifdef PC1500_HAVE_EXPANSION_KEYWORDS
#include "plotter.h"  // plot_decode(): the firmware's own
#endif

namespace pc1500 {

bool PlotPaper::addPayload(const uint8_t* payload, size_t len) {
#ifdef PC1500_HAVE_EXPANSION_KEYWORDS
  if (len < PLOT_PREFIX || len > 0xFFFF) return false;
  std::vector<Line> added;
  plot_pen_t pen;
  auto line = [](void* ctx, uint8_t p, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    static_cast<std::vector<Line>*>(ctx)->push_back({p, x0, y0, x1, y1});
  };
  bool ok = plot_decode(payload, static_cast<uint16_t>(len), &pen, line, &added);
  std::lock_guard<std::mutex> lock(mutex_);
  auto span = [&](int32_t y) {
    if (!penKnown_ && lines_.empty()) {
      top_ = bottom_ = y;
      penKnown_ = true;
    }
    top_ = std::max(top_, y);
    bottom_ = std::min(bottom_, y);
  };
  for (const Line& l : added) {
    span(l.y0);
    span(l.y1);
    lines_.push_back(l);
  }
  span(pen.y);
  pen_ = pen.pen;
  x_ = pen.x;
  y_ = pen.y;
  version_++;
  return ok;
#else
  (void)payload;
  (void)len;
  return false;
#endif
}

std::vector<PlotPaper::Line> PlotPaper::lines() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return lines_;
}

size_t PlotPaper::lineCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return lines_.size();
}

bool PlotPaper::penKnown() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return penKnown_;
}

int32_t PlotPaper::penX() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return x_;
}

int32_t PlotPaper::penY() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return y_;
}

uint8_t PlotPaper::pen() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return pen_;
}

void PlotPaper::extent(int32_t* top, int32_t* bottom) const {
  std::lock_guard<std::mutex> lock(mutex_);
  *top = top_;
  *bottom = bottom_;
}

uint64_t PlotPaper::version() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return version_;
}

void PlotPaper::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  lines_.clear();
  penKnown_ = false;
  top_ = bottom_ = y_;
  version_++;
}

}  // namespace pc1500
