#include "TextSettingsPreview.h"

#include <EpdFontFamily.h>
#include <Epub/ParsedText.h>
#include <Epub/blocks/BlockStyle.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>

#include "CrossPointSettings.h"
#include "fontIds.h"

namespace textsettings {

namespace {

// Map the paragraph-alignment setting to the engine's CssTextAlign (BOOK_STYLE = justified)
CssTextAlign toCssAlign(uint8_t align) {
  if (align == CrossPointSettings::BOOK_STYLE) return CssTextAlign::Justify;
  return static_cast<CssTextAlign>(align);
}

// Lay the sample text out through the reader engine into layout.lines.
// Layout-affecting settings are read through the getReader*() accessors so the pane
// mirrors exactly what the reader will render: per-book values when a reader override is
// active (ReaderOptionsActivity), and the globals otherwise. The global Text Settings
// screen only ever opens outside a reader, where the override is inactive (cleared on
// reader exit), so the accessors return the globals it edits -- behavior unchanged there.
// focusReadingEnabled has no per-book override field, so it stays a plain global read.
void relayout(PreviewLayout& layout, const GfxRenderer& renderer, int fontId, int textWidth, const char* text) {
  layout.lines.clear();

  BlockStyle style;
  style.alignment = toCssAlign(SETTINGS.getReaderParagraphAlignment());
  style.textAlignDefined = true;  // honor the user's choice; RTL auto-detected from text

  ParsedText parsed(SETTINGS.getReaderExtraParagraphSpacing() != 0, SETTINGS.getReaderHyphenationEnabled() != 0,
                    SETTINGS.focusReadingEnabled != 0, style);

  // Feed one space-separated word at a time; addWord handles NFC/CJK/RTL/focus splitting
  std::string word;
  for (const char* p = text;; p++) {
    if (*p == ' ' || *p == '\0') {
      if (!word.empty()) {
        parsed.addWord(word, EpdFontFamily::REGULAR);
        word.clear();
      }
      if (*p == '\0') break;
    } else {
      word.push_back(*p);
    }
  }

  parsed.layoutAndExtractLines(
      renderer, fontId, static_cast<uint16_t>(textWidth),
      [&layout](std::shared_ptr<TextBlock> line, uint32_t) { layout.lines.push_back(std::move(line)); });
}

// Cut the sample down to what the pane can physically show.
//
// renderPreview lays out and prewarms whatever it is handed, but the draw loop below stops at
// textBottomLimit. The reader passes a whole page: on CJK that was ~30 lines laid out and up to
// MAX_PAGE_GLYPHS unique glyphs prewarmed in order to display ~10 lines — on the same heap where
// entering Reader Options was rebooting the device.
//
// The bound is a deliberate overestimate: it assumes no glyph is narrower than half the line
// advance, which is about right for Latin and roughly 2x generous for square CJK. Cutting too
// little only wastes a little work; cutting too much would leave the pane visibly half-empty.
std::string clampToPane(const char* text, int visibleLines, int textWidth, int lineAdvance) {
  const int perLine = std::max(1, textWidth / std::max(1, lineAdvance / 2));
  const size_t maxChars = static_cast<size_t>(visibleLines) * static_cast<size_t>(perLine) + 16;

  size_t chars = 0, bytes = 0, lastSpace = 0;
  while (text[bytes] != '\0') {
    // Continuation bytes (10xxxxxx) are mid-codepoint, so `bytes` only ever stops on a
    // UTF-8 boundary — this is what keeps the cut from splitting a multi-byte character.
    if ((static_cast<unsigned char>(text[bytes]) & 0xC0) != 0x80) {
      if (chars == maxChars) break;
      if (text[bytes] == ' ') lastSpace = bytes;
      chars++;
    }
    bytes++;
  }
  if (text[bytes] == '\0') return std::string(text);  // already short enough

  // Prefer a word boundary, but only when one falls near the cut. CJK has no spaces at all,
  // and backing off to the first one would truncate the sample to nothing.
  if (lastSpace > bytes - bytes / 5) bytes = lastSpace;
  return std::string(text, bytes);
}

}  // namespace

void renderPreview(GfxRenderer& renderer, PreviewLayout& layout, int previewPadding, int labelGap, int top, int height,
                   const char* familyName, const char* sizeName, const char* sampleText, bool showLabel) {
  // The reader passes its current page text; everything else previews the pangram.
  const char* text = (sampleText && *sampleText) ? sampleText : I18N.get(StrId::STR_FONT_PREVIEW_TEXT);
  const int left = previewPadding;
  const int width = renderer.getScreenWidth() - (previewPadding * 2);
  if (width <= 0 || height <= 0) return;

  // Without the caption the sample text fills the whole pane (bar the bottom padding).
  const int labelH = renderer.getTextHeight(UI_10_FONT_ID);
  const int labelReserved = showLabel ? (labelH + labelGap + previewPadding) : previewPadding;

  if (showLabel) {
    char labelBuf[128];
    snprintf(labelBuf, sizeof(labelBuf), "%s \"%s, %s\"", tr(STR_PREVIEW), familyName, sizeName);
    const int labelY = top + height - previewPadding - labelH;
    renderer.drawText(UI_10_FONT_ID, left, labelY, labelBuf);
  }

  const int fontId = SETTINGS.getReaderFontId();
  if (fontId == 0) return;

  const int lineH = renderer.getTextHeight(fontId);
  if (lineH <= 0) return;

  const int textLeft = left + SETTINGS.getReaderScreenMargin();
  const int textWidth = width - 2 * SETTINGS.getReaderScreenMargin();
  if (textWidth <= 0) return;

  const float compression = SETTINGS.getReaderLineCompression();
  const int lineAdvance = std::max(1, renderer.getLineHeight(fontId, compression));
  const int paragraphGap = SETTINGS.getReaderExtraParagraphSpacing() ? lineAdvance / 2 : 0;

  // Re-lay-out (and re-prewarm glyphs) only when a layout-affecting setting or the
  // geometry changed; else reuse the cache. The prewarm inputs are (fontId, the sample
  // clamped to the pane, styleMask<-focusReading); the sample is fixed per activity and
  // the clamp derives from key fields, so a matching key means an identical prewarm call.
  // This relies on nothing else evicting the SD glyph cache while this activity is up —
  // true today: the only evictor is FontCacheManager::PrewarmScope, used solely by the
  // reader/dictionary activities.
  const PreviewKey key{.fontId = fontId,
                       .fontPointSize = SETTINGS.getReaderFontSize(),
                       .screenMargin = SETTINGS.getReaderScreenMargin(),
                       .textWidth = textWidth,
                       .lineCompression = compression,
                       .alignment = SETTINGS.getReaderParagraphAlignment(),
                       .extraParagraphSpacing = SETTINGS.getReaderExtraParagraphSpacing() != 0,
                       .focusReading = SETTINGS.focusReadingEnabled != 0,
                       .hyphenation = SETTINGS.getReaderHyphenationEnabled() != 0};
  if (key != layout.key) {
    // Both the prewarm and the layout see the clamped sample, so neither pays for text the
    // draw loop below would discard. The temporary is a few hundred bytes and is freed on
    // exit from this block — far less than the page-sized layout it avoids building.
    const int visibleLines = std::max(1, (height - labelReserved) / lineAdvance);
    const std::string sample = clampToPane(text, visibleLines, textWidth, lineAdvance);
    if (auto* fcm = renderer.getFontCacheManager()) {
      fcm->prewarmCache(fontId, sample.c_str(), SETTINGS.focusReadingEnabled ? 0x03 : 0x01);
    }
    relayout(layout, renderer, fontId, textWidth, sample.c_str());
    layout.key = key;
  }

  // Draw the sample twice so the paragraph gap is visible
  int y = top + previewPadding;
  const int textBottomLimit = top + height - labelReserved;
  for (int paragraph = 0; paragraph < 2; paragraph++) {
    for (const auto& line : layout.lines) {
      if (y + lineH > textBottomLimit) return;
      line->render(renderer, fontId, textLeft, y);
      y += lineAdvance;
    }
    y += paragraphGap;
  }
}

}  // namespace textsettings
