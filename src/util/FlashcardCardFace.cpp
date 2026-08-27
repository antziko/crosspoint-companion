#include "FlashcardCardFace.h"

#include <GfxRenderer.h>
#include <Utf8.h>

#include <cctype>
#include <cstring>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/FlashcardDeck.h"

namespace {

// True when `text` carries a codepoint in a CJK script block -- one no built-in font subset
// can draw. Same predicate the renderer's own UI fallback map uses (resolveTextFontId).
bool containsCjk(const char* text) {
  if (!text) return false;
  const auto* cursor = reinterpret_cast<const unsigned char*>(text);
  uint32_t cp;
  while ((cp = utf8NextCodepoint(&cursor))) {
    if (utf8IsCjkCodepoint(cp)) return true;
  }
  return false;
}

// The font to draw `text` with, given the card's body font. Returns `bodyFont` unchanged
// unless the text is CJK and bodyFont is a built-in family: those ship no Han glyphs, and
// EpdFont::getGlyph then substitutes U+FFFD silently (EpdFont.cpp:224) -- the replacement
// boxes this fixes. getDefinitionFontId() already follows the book's SD family under
// "Same as book"; this covers its other branch, where the user picked NotoSerif/NotoSans as
// the dictionary font explicitly and the book's own family is the only source of Han glyphs.
//
// Look-up only -- no allocation, no residency call -- the same contract getDefinitionFontId()
// keeps, because this runs on the render path. In that explicit-family case
// ensureDefinitionFontResident() has deliberately loaded nothing extra
// (DictionaryActivityUtils.h:36), so the resolver hands back the nearest resident size, which
// is the reader's: correct glyphs at the book's size rather than the dictionary's, for zero
// extra heap. Under "Same as book" the dictionary size is already resident and this is a no-op.
int cjkCapableFontId(const GfxRenderer& renderer, const int bodyFont, const char* text) {
  if (!containsCjk(text) || renderer.isSdCardFont(bodyFont)) return bodyFont;
  const auto& settings = CrossPointSettings::getInstance();
  if (!settings.sdFontIdResolver) return bodyFont;
  const int id = settings.sdFontIdResolver(settings.sdFontResolverCtx, settings.getReaderSdFontFamilyName(),
                                           settings.getDefinitionPointSize());
  return id != 0 ? id : bodyFont;
}

// What drawHeadword actually put on screen, so the caller can place the xN badge against the
// font and scale used rather than assuming the 18pt serif.
struct HeadwordLayout {
  int width;       // drawn width of the headword
  int lineHeight;  // line height of the drawn headword, including any upscale
  int top;         // y of its line box
};

// Draw the card's headword centered in the header band [bandTop, bandTop + bandHeight).
//
// Latin keeps the 18pt bold serif. A CJK word cannot use it: it ships no Han glyphs, and only
// the three UI font ids carry a registered CJK fallback (SdCardFontSystem.cpp:35), so
// resolveTextFontId leaves the id alone and every character came out as a replacement box.
// It is drawn with the definition font instead, blitted per character at 2x through
// drawGlyphScaled -- the same upscale the inline gloss box uses for its enlarged character
// columns (DictionaryWordSelectActivity.cpp:1207-1222). Loading the family at a third point
// size is the alternative, and it pins another resident .cpfont on the reader's heap.
HeadwordLayout drawHeadword(GfxRenderer& renderer, const int bodyFont, const int bandTop, const int bandHeight,
                            const int pageWidth, const std::string& word) {
  if (!containsCjk(word.c_str())) {
    renderer.drawCenteredText(NOTOSERIF_18_FONT_ID, bandTop, word.c_str(), true, EpdFontFamily::BOLD);
    return {renderer.getTextWidth(NOTOSERIF_18_FONT_ID, word.c_str(), EpdFontFamily::BOLD),
            renderer.getLineHeight(NOTOSERIF_18_FONT_ID), bandTop};
  }

  const int fontId = cjkCapableFontId(renderer, bodyFont, word.c_str());
  const int lineHeight = renderer.getLineHeight(fontId);
  // Upscale only while the enlarged line still fits the band. The plain path is also the
  // better one when it does not: it keeps the kerning and bidi handling drawText does and the
  // per-glyph loop does not.
  if (lineHeight * 2 > bandHeight) {
    renderer.drawCenteredText(fontId, bandTop, word.c_str(), true, EpdFontFamily::BOLD);
    return {renderer.getTextWidth(fontId, word.c_str(), EpdFontFamily::BOLD), lineHeight, bandTop};
  }

  constexpr int kScale = 2;
  // Advance width, not ink width: it is what drawGlyphScaled accumulates glyph by glyph. Han
  // advances are uniform and unkerned -- the same assumption the gloss box's cell layout makes.
  const int total = renderer.getTextAdvanceWidth(fontId, word.c_str()) * kScale;
  const int top = bandTop + (bandHeight - renderer.getFontAscenderSize(fontId) * kScale) / 2;
  int x = (pageWidth - total) / 2;
  const auto* cursor = reinterpret_cast<const unsigned char*>(word.c_str());
  uint32_t cp;
  while ((cp = utf8NextCodepoint(&cursor))) {
    x += renderer.drawGlyphScaled(fontId, cp, x, top, kScale, true);
  }
  return {total, lineHeight * kScale, top};
}

// Word-wrap `text` centered within [contentTop, contentBottom); returns the y
// after the last line. Bounded by the excerpt cap, no heap beyond one line. When
// `highlightWord` is non-null, each case-insensitive occurrence is underlined;
// when `maskHighlight` is also true the occurrence is white-boxed first (the
// cloze blank), leaving just the underline.
int drawWrappedCentered(GfxRenderer& renderer, int fontId, int contentTop, int contentBottom, int pageWidth,
                        const char* text, const char* highlightWord, bool maskHighlight) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = metrics.listRowHeight;
  const int maxWidth = pageWidth - 2 * metrics.contentSidePadding;
  const int hlLen = highlightWord ? static_cast<int>(strlen(highlightWord)) : 0;

  // Greedy word wrap into a fixed line buffer (excerpt is capped, so bounded).
  char line[FlashcardDeck::EXCERPT_MAX + 1];
  int lineLen = 0;
  int y = contentTop;

  // Underline each case-insensitive occurrence of highlightWord in the just-drawn,
  // centered line. Both span endpoints are measured as cumulative prefixes from the
  // line start (null-terminating in place, no extra buffer) so they match drawText's
  // own left-to-right glyph layout (differential rounding + kerning); measuring the
  // matched word in isolation would drop the boundary kern and drift by ~a char.
  auto underline = [&](int top) {
    if (hlLen == 0) return;
    // Pen origin of the centered line = drawCenteredText's x = (W - inkWidth)/2.
    const int startX = (pageWidth - renderer.getTextWidth(fontId, line)) / 2;
    for (int i = 0; i + hlLen <= lineLen;) {
      // Never start a match on a UTF-8 continuation byte: this scan is byte-wise, so on CJK it
      // could otherwise align mid-sequence and underline a span straddling two characters.
      if ((static_cast<unsigned char>(line[i]) & 0xC0) == 0x80) {
        i++;
        continue;
      }
      bool match = true;
      for (int k = 0; k < hlLen; k++)
        if (std::tolower(static_cast<unsigned char>(line[i + k])) !=
            std::tolower(static_cast<unsigned char>(highlightWord[k]))) {
          match = false;
          break;
        }
      if (!match) {
        i++;
        continue;
      }
      // Pen origin of the match = advance cursor of the prefix (NOT ink width, which
      // loses side bearings/spaces). Then place the underline at the word's actual
      // ink extents from that origin, so it spans exactly the visible glyphs.
      const char saveStart = line[i];
      line[i] = '\0';
      const int penX = startX + renderer.getTextAdvanceWidth(fontId, line);
      line[i] = saveStart;
      const char saveEnd = line[i + hlLen];
      line[i + hlLen] = '\0';
      int inkMinX = 0, inkMaxX = 0;
      renderer.getTextInkBounds(fontId, line + i, &inkMinX, &inkMaxX);
      line[i + hlLen] = saveEnd;
      const int ulX = penX + inkMinX;     // visible left edge of the word
      const int ulW = inkMaxX - inkMinX;  // ink width of the word
      // Cloze front: white-box the word glyphs (small pad), leaving just the underline.
      if (maskHighlight) renderer.fillRect(ulX - 2, top, ulW + 4, lineHeight, false);
      renderer.fillRect(ulX, top + lineHeight - 3, ulW, 2, true);
      i += hlLen;
    }
  };

  auto flush = [&]() {
    if (lineLen == 0) return;
    line[lineLen] = '\0';
    if (y + lineHeight <= contentBottom) {
      renderer.drawCenteredText(fontId, y, line);
      underline(y);
    }
    y += lineHeight;
    lineLen = 0;
  };

  const char* p = text;
  bool joinWithSpace = false;  // the source had a word gap before the next token
  while (*p) {
    if (*p == ' ') {  // whitespace run: remember it as a word gap, then drop it
      while (*p == ' ') p++;
      joinWithSpace = true;
      continue;
    }

    // One token: up to the next space, or to the next CJK break opportunity. Breaking only on
    // ' ' made a whole Chinese sentence a single token -- one centered line far wider than the
    // screen, since CJK is written without spaces. Breaking between two codepoints when either
    // is CJK-breakable is the rule layout itself applies (ParsedText.cpp:150, mirrored by
    // utf8NeedsSpaceBetween); Latin text carries no such codepoint, so it tokenizes exactly as
    // before.
    const char* wordStart = p;
    const auto* cursor = reinterpret_cast<const unsigned char*>(p);
    uint32_t cp;
    while ((cp = utf8NextCodepoint(&cursor))) {
      p = reinterpret_cast<const char*>(cursor);
      if (*p == '\0' || *p == ' ') break;
      const auto* peek = cursor;
      if (utf8IsCjkBreakable(cp) || utf8IsCjkBreakable(utf8NextCodepoint(&peek))) break;
    }
    const int wordLen = static_cast<int>(p - wordStart);

    // Candidate line = current + (space) + word. The space goes back in only where the source
    // had one: a CJK break is not a word gap, and re-inserting one there would space the
    // characters out.
    char cand[FlashcardDeck::EXCERPT_MAX + 1];
    int candLen = lineLen;
    memcpy(cand, line, lineLen);
    if (candLen > 0 && joinWithSpace && candLen < FlashcardDeck::EXCERPT_MAX) cand[candLen++] = ' ';
    // Drop the tail rather than clamp the copy to the byte budget: a clamp can cut a UTF-8
    // sequence in half, and half a sequence draws as a broken glyph.
    if (candLen + wordLen > FlashcardDeck::EXCERPT_MAX) break;
    memcpy(cand + candLen, wordStart, wordLen);
    candLen += wordLen;
    cand[candLen] = '\0';

    if (lineLen > 0 && renderer.getTextWidth(fontId, cand) > maxWidth) {
      flush();
      memcpy(line, wordStart, wordLen);
      lineLen = wordLen;
    } else {
      memcpy(line, cand, candLen);
      lineLen = candLen;
    }
    joinWithSpace = false;
  }
  flush();
  return y;
}

// Draw the card's chapter title (if any) as a small footer just above the button
// hints. No-op when the chapter is empty.
void drawChapterFooter(GfxRenderer& renderer, int contentBottom, const std::string& chapter) {
  if (chapter.empty()) return;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int y = contentBottom - metrics.listRowHeight;

  // The chapter field may carry a trailing in-chapter page token "X/Y". When present the
  // title goes flush-left and the token flush-right, otherwise the title is centered.
  const char* ch = chapter.c_str();
  const int len = static_cast<int>(chapter.size());
  int sp = -1;
  const bool hasPage = FlashcardDeck::parseChapterPage(ch, len, &sp, nullptr, nullptr);

  // Rendered to match the reader's status-bar chapter title: SMALL_FONT_ID,
  // ellipsis-truncated, then dimmed to a grey checkerboard stipple so the footer
  // recedes from the card body (see BaseTheme::drawStatusBar).
  const int margin = metrics.contentSidePadding;
  const int sw = renderer.getScreenWidth();
  const int lineH = renderer.getLineHeight(SMALL_FONT_ID);

  if (!hasPage) {  // legacy / no page token: centered title, full-width budget
    const std::string trunc = renderer.truncatedText(SMALL_FONT_ID, ch, sw - 2 * margin);
    const int w = renderer.getTextWidth(SMALL_FONT_ID, trunc.c_str());
    renderer.drawCenteredText(SMALL_FONT_ID, y, trunc.c_str(), true);
    renderer.dimRegionCheckerboard((sw - w) / 2, y, w, lineH);
    return;
  }

  // title = bytes [0, sp); page = bytes (sp, len). title may be empty (book with
  // no TOC chapter, where only " X/Y" was stored).
  char titleBuf[FlashcardDeck::CHAPTER_MAX + 1];
  const int titleLen = sp;  // bytes before the space
  memcpy(titleBuf, ch, static_cast<size_t>(titleLen));
  titleBuf[titleLen] = '\0';
  const char* pageBuf = ch + sp + 1;  // null-terminated tail of chapter

  // Chapter title flush-left, page token flush-right at the screen margin. The
  // title is truncated to the gap before the page token so the two never overlap
  // (book-style footer). Both use SMALL_FONT_ID so they share a baseline.
  constexpr int kGap = 10;  // min clearance between title and page token
  const int wPage = renderer.getTextWidth(SMALL_FONT_ID, pageBuf);

  if (titleLen > 0) {
    const int titleMax = sw - 2 * margin - wPage - kGap;
    if (titleMax > 0) {
      const std::string trunc = renderer.truncatedText(SMALL_FONT_ID, titleBuf, titleMax);
      const int w = renderer.getTextWidth(SMALL_FONT_ID, trunc.c_str());
      renderer.drawText(SMALL_FONT_ID, margin, y, trunc.c_str(), true);
      renderer.dimRegionCheckerboard(margin, y, w, lineH);
    }
  }
  renderer.drawText(SMALL_FONT_ID, sw - margin - wPage, y, pageBuf, true);
}

}  // namespace

namespace FlashcardCardFace {

void render(GfxRenderer& renderer, int contentTop, int contentBottom, int pageWidth, const std::string& word,
            const std::string& excerpt, const std::string& chapter, bool showWord, uint32_t lookupCount) {
  const int defFont = CrossPointSettings::getInstance().getDefinitionFontId();
  const int bodyFont = cjkCapableFontId(renderer, defFont, excerpt.c_str());
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Bold word header — drawn only when the answer is shown. Cloze front omits it
  // (the row stays blank) so the excerpt below keeps the same position either way.
  if (showWord) {
    const int wordY = contentTop + metrics.listRowHeight;
    // The header owns the two rows between its own row and the excerpt below it.
    const int band = metrics.listRowHeight * 2;
    const HeadwordLayout head = drawHeadword(renderer, defFont, wordY, band, pageWidth, word);
    // Small "xN" lookup-count badge just right of the centered word, only when > 1.
    // Small font + bottom-aligned so it reads as a subtle annotation, not a second word.
    if (lookupCount > 1) {
      char cbuf[12];
      snprintf(cbuf, sizeof(cbuf), "x%lu", static_cast<unsigned long>(lookupCount));
      const int gap = renderer.getTextWidth(NOTOSERIF_18_FONT_ID, "  ", EpdFontFamily::BOLD);  // ~2 word-spaces
      const int x = (pageWidth + head.width) / 2 + gap;  // right edge of the centered word + the gap
      const int dy = head.lineHeight - renderer.getLineHeight(SMALL_FONT_ID);
      renderer.drawText(SMALL_FONT_ID, x, head.top + dy, cbuf, true);
    }
  }

  if (!excerpt.empty()) {
    // Mark the word as the page printed it, which is not always the headword: a "Did you mean?"
    // lookup files the card under the suggestion ("pontificate") while the excerpt keeps the
    // printed form ("pontifications"). Falls back to the headword when nothing is recovered.
    char surfaceBuf[64];
    const char* highlight = word.c_str();
    int surfaceLen = 0;
    const char* surface = FlashcardDeck::findSurfaceForm(word.c_str(), static_cast<int>(word.size()), excerpt.c_str(),
                                                         static_cast<int>(excerpt.size()), &surfaceLen);
    if (surface && surfaceLen > 0 && surfaceLen < static_cast<int>(sizeof(surfaceBuf))) {
      memcpy(surfaceBuf, surface, static_cast<size_t>(surfaceLen));
      surfaceBuf[surfaceLen] = '\0';
      highlight = surfaceBuf;
    }

    // Excerpt with the word underlined in context; masked (white-boxed) when hidden.
    drawWrappedCentered(renderer, bodyFont, contentTop + metrics.listRowHeight * 3, contentBottom, pageWidth,
                        excerpt.c_str(), highlight, /*maskHighlight=*/!showWord);
  } else if (!showWord) {
    // No excerpt to blank into: fall back to a centered "____" placeholder.
    renderer.drawCenteredText(bodyFont, contentTop + metrics.listRowHeight * 3, "____");
  }

  drawChapterFooter(renderer, contentBottom, chapter);
}

}  // namespace FlashcardCardFace
