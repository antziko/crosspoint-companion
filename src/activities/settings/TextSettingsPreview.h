#pragma once

#include <cstdint>
#include <memory>
#include <vector>

class GfxRenderer;
class TextBlock;

namespace textsettings {

// Settings + geometry that determine the laid-out lines; used to invalidate the cache.
struct PreviewKey {
  int fontId = -1;
  int fontPointSize = -1;
  int screenMargin = -1;
  int textWidth = -1;
  float lineCompression = -1.0f;
  uint8_t alignment = 0xFF;
  bool extraParagraphSpacing = false;
  bool focusReading = false;
  bool hyphenation = false;
  bool operator==(const PreviewKey&) const = default;
};

// Cached engine preview lines + the key that produced them
struct PreviewLayout {
  std::vector<std::shared_ptr<TextBlock>> lines;
  PreviewKey key;
};

// Draws the sample-text pane via the reader engine, reusing layout across redraws.
// sampleText overrides the built-in pangram when non-null/non-empty (e.g. the reader
// passes the current page's own text); the global Text Settings screen passes nothing
// and falls back to STR_FONT_PREVIEW_TEXT.
// showLabel draws the 'Preview "family, size"' caption at the bottom; pass false to drop it
// and let the sample text fill the whole pane (the in-reader picker uses this).
void renderPreview(GfxRenderer& renderer, PreviewLayout& layout, int previewPadding, int labelGap, int top, int height,
                   const char* familyName, const char* sizeName, const char* sampleText = nullptr,
                   bool showLabel = true);

}  // namespace textsettings
