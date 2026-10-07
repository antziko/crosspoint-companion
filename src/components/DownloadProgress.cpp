#include "DownloadProgress.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "fontIds.h"

namespace DownloadProgress {
namespace {

constexpr int kBarHeight = 20;
constexpr int kBarRadius = 6;
constexpr int kBarBorder = 2;
constexpr int kInset = 4;  // border to fill
constexpr int kGapBelowBar = 8;
constexpr int kGapBetweenLines = 4;
constexpr int kHatchPitch = 8;
// The rate line waits for this much history, so the first figure is not a guess from
// one chunk.
constexpr uint32_t kMinRateMs = 2000;

void formatDuration(char* out, const size_t outSize, uint32_t seconds) {
  if (seconds >= 3600) {
    snprintf(out, outSize, "%lu:%02lu:%02lu", static_cast<unsigned long>(seconds / 3600),
             static_cast<unsigned long>((seconds / 60) % 60), static_cast<unsigned long>(seconds % 60));
  } else {
    snprintf(out, outSize, "%lu:%02lu", static_cast<unsigned long>(seconds / 60),
             static_cast<unsigned long>(seconds % 60));
  }
}

// Diagonal hatching across the inner track: the "working, size unknown" bar.
void drawHatch(const GfxRenderer& renderer, const int x, const int y, const int w, const int h) {
  for (int start = x - h; start < x + w; start += kHatchPitch) {
    for (int k = 0; k < h; k++) {
      const int py = y + h - 1 - k;
      for (int t = 0; t < 2; t++) {  // 2px stroke
        const int px = start + k + t;
        if (px >= x && px < x + w) renderer.drawPixel(px, py, true);
      }
    }
  }
}

}  // namespace

void formatBytes(char* out, const size_t outSize, const uint64_t bytes) {
  constexpr double kKb = 1024.0;
  const double b = static_cast<double>(bytes);
  if (bytes < 1024) {
    snprintf(out, outSize, "%u B", static_cast<unsigned>(bytes));
  } else if (b < kKb * kKb) {
    snprintf(out, outSize, "%.1f KB", b / kKb);
  } else if (b < kKb * kKb * kKb) {
    snprintf(out, outSize, "%.1f MB", b / (kKb * kKb));
  } else {
    snprintf(out, outSize, "%.2f GB", b / (kKb * kKb * kKb));
  }
}

int height(const GfxRenderer& renderer) {
  return kBarHeight + kGapBelowBar + renderer.getLineHeight(UI_10_FONT_ID) + kGapBetweenLines +
         renderer.getLineHeight(SMALL_FONT_ID);
}

int draw(const GfxRenderer& renderer, const int x, const int y, const int width, const State& state) {
  const bool known = state.total > 0;
  const size_t done = known ? std::min(state.done, state.total) : state.done;

  // Bar: rounded outline, then the track and fill inset inside it.
  renderer.drawRoundedRect(x, y, width, kBarHeight, kBarBorder, kBarRadius, true);
  const int ix = x + kInset;
  const int iy = y + kInset;
  const int iw = width - 2 * kInset;
  const int ih = kBarHeight - 2 * kInset;
  if (iw > 0 && ih > 0) {
    if (known) {
      if (state.shadedTrack) renderer.fillRoundedRect(ix, iy, iw, ih, ih / 2, Color::LightGray);
      const int fw = static_cast<int>(static_cast<uint64_t>(iw) * done / state.total);
      if (fw > 0) renderer.fillRoundedRect(ix, iy, fw, ih, std::min(ih / 2, fw / 2), Color::Black);
    } else if (done > 0) {
      drawHatch(renderer, ix, iy, iw, ih);
    }
  }

  // Size line: what has arrived (of the total), and the percentage at the right edge.
  int ty = y + kBarHeight + kGapBelowBar;
  char doneText[16];
  formatBytes(doneText, sizeof(doneText), done);
  char line[64];
  if (known) {
    char totalText[16];
    formatBytes(totalText, sizeof(totalText), state.total);
    snprintf(line, sizeof(line), tr(STR_PROGRESS_OF), doneText, totalText);
    char pct[8];
    snprintf(pct, sizeof(pct), "%u%%", static_cast<unsigned>(static_cast<uint64_t>(done) * 100 / state.total));
    const int pw = renderer.getTextWidth(UI_10_FONT_ID, pct, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, x + width - pw, ty, pct, true, EpdFontFamily::BOLD);
  } else {
    snprintf(line, sizeof(line), tr(STR_PROGRESS_DOWNLOADED), doneText);
  }
  renderer.drawText(UI_10_FONT_ID, x, ty, line);
  ty += renderer.getLineHeight(UI_10_FONT_ID) + kGapBetweenLines;

  // Rate line: speed with the time left when the total is known, the time taken when not.
  if (state.elapsedMs >= kMinRateMs && done > 0) {
    const uint64_t rate = static_cast<uint64_t>(done) * 1000 / state.elapsedMs;  // bytes/s
    char rateText[16];
    formatBytes(rateText, sizeof(rateText), rate);
    char timeText[16];
    if (known && rate > 0) {
      formatDuration(timeText, sizeof(timeText), static_cast<uint32_t>((state.total - done) / rate));
      snprintf(line, sizeof(line), tr(STR_PROGRESS_RATE_LEFT), rateText, timeText);
    } else {
      formatDuration(timeText, sizeof(timeText), state.elapsedMs / 1000);
      snprintf(line, sizeof(line), tr(STR_PROGRESS_RATE_ELAPSED), rateText, timeText);
    }
    renderer.drawText(SMALL_FONT_ID, x, ty, line);
  }
  return height(renderer);
}

}  // namespace DownloadProgress
