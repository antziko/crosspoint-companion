#pragma once

#include <EpdFontFamily.h>
#include <Utf8.h>

namespace BidiUtils {
// Mirrors the real GfxRenderer.h, which declares this itself; lib/MiniBidi/BidiUtils.h
// does not. TextBlock.cpp needs it for the getTextWidth calls below.
enum class BidiBaseDir : signed char { AUTO = -1, LTR = 0, RTL = 1 };
}  // namespace BidiUtils

#include <deque>
#include <string>

// Korean-suite renderer stub. Deliberately separate from the one in
// test/chapter_html_slim_parser/stubs: that one advances 8 px per BYTE, which is
// fine for the ASCII fixtures it serves but makes a three-syllable Hangul word
// measure 72 px instead of 24. This one advances per CODEPOINT so the widths in
// the Korean expectations mean what they say, and adds getTextWidth, which
// TextBlock.cpp needs and the shared stub never did (that suite does not compile
// TextBlock.cpp).
class GfxRenderer {
 public:
  enum class TextMeasureMode { Layout, Rendered };
  int getScreenWidth() const { return 480; }
  int getScreenHeight() const { return 800; }
  int getLineHeight(int, float = 1.0f) const { return 16; }
  int getFontAscenderSize(int) const { return 12; }
  int getSpaceWidth(int, EpdFontFamily::Style) const { return 4; }
  static int trackingBetween(uint32_t left, uint32_t right, int8_t tracking) {
    return left == 0 || left == ' ' || right == ' ' ? 0 : tracking;
  }
  int getTextAdvanceX(int, const char* text, EpdFontFamily::Style, int8_t tracking = 0,
                      BidiUtils::BidiBaseDir = BidiUtils::BidiBaseDir::AUTO,
                      TextMeasureMode = TextMeasureMode::Layout) const {
    int width = 0;
    uint32_t previous = 0;
    const auto* p = reinterpret_cast<const uint8_t*>(text);
    while (const uint32_t cp = utf8NextCodepoint(&p)) {
      if (utf8IsCombiningMark(cp)) continue;
      width += 8 + trackingBetween(previous, cp, tracking);
      previous = cp;
    }
    return width;
  }
  int getTextWidth(int font, const char* text, EpdFontFamily::Style style,
                   BidiUtils::BidiBaseDir = BidiUtils::BidiBaseDir::AUTO) const {
    return getTextAdvanceX(font, text, style);
  }
  bool isFontCacheScanning() const { return false; }
  void drawLine(int, int, int, int, int, bool) const {}
  void drawText(int, int, int, const char*, bool, EpdFontFamily::Style,
                BidiUtils::BidiBaseDir = BidiUtils::BidiBaseDir::AUTO, int8_t = 0) const {}
  int getKerning(int, uint32_t left, uint32_t right, EpdFontFamily::Style, int8_t tracking = 0) const {
    return trackingBetween(left, right, tracking);
  }
  int getSpaceAdvance(int, uint32_t, uint32_t, EpdFontFamily::Style) const { return 4; }
  bool isSdCardFont(int) const { return false; }
  void ensureSdCardFontReady(int, const std::deque<std::string>&, bool, uint8_t) const {}
};
