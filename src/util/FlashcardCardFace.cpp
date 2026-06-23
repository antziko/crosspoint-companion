#include "FlashcardCardFace.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <cctype>
#include <cstring>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/FlashcardDeck.h"

namespace {

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
  while (*p) {
    const char* wordStart = p;
    while (*p && *p != ' ') p++;
    const int wordLen = static_cast<int>(p - wordStart);
    while (*p == ' ') p++;  // skip spaces

    // Candidate line = current + (space) + word.
    char cand[FlashcardDeck::EXCERPT_MAX + 1];
    int candLen = lineLen;
    memcpy(cand, line, lineLen);
    if (candLen > 0 && candLen < FlashcardDeck::EXCERPT_MAX) cand[candLen++] = ' ';
    const int copy = std::min(wordLen, FlashcardDeck::EXCERPT_MAX - candLen);
    memcpy(cand + candLen, wordStart, copy);
    candLen += copy;
    cand[candLen] = '\0';

    if (lineLen > 0 && renderer.getTextWidth(fontId, cand) > maxWidth) {
      flush();
      const int c2 = std::min(wordLen, FlashcardDeck::EXCERPT_MAX);
      memcpy(line, wordStart, c2);
      lineLen = c2;
    } else {
      memcpy(line, cand, candLen);
      lineLen = candLen;
    }
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

  // The chapter field may carry a trailing in-chapter page token "X/Y" (appended
  // at enroll in EpubReaderActivity::openWordSelect). Render the title at UI_10
  // italic and that page token one size smaller (SMALL_FONT_ID), centered as one
  // group. Detect the token as the last space-delimited run of digits-and-'/'.
  const char* ch = chapter.c_str();
  const int len = static_cast<int>(chapter.size());
  int sp = -1;
  for (int i = len - 1; i >= 0; --i) {
    if (ch[i] == ' ') {
      sp = i;
      break;
    }
  }
  bool hasPage = false;
  if (sp >= 0 && sp + 1 < len) {
    bool slash = false, digitsOnly = true;
    for (int i = sp + 1; i < len; ++i) {
      if (ch[i] == '/')
        slash = true;
      else if (ch[i] < '0' || ch[i] > '9') {
        digitsOnly = false;
        break;
      }
    }
    hasPage = slash && digitsOnly;
  }

  if (!hasPage) {  // legacy / no page token: original single-font centered footer
    renderer.drawCenteredText(UI_10_FONT_ID, y, ch, true, EpdFontFamily::ITALIC);
    return;
  }

  // title = bytes [0, sp); page = bytes (sp, len). title may be empty (book with
  // no TOC chapter, where only " X/Y" was stored).
  char titleBuf[FlashcardDeck::CHAPTER_MAX + 1];
  const int titleLen = sp;  // bytes before the space
  memcpy(titleBuf, ch, static_cast<size_t>(titleLen));
  titleBuf[titleLen] = '\0';
  const char* pageBuf = ch + sp + 1;  // null-terminated tail of chapter

  const int wTitle = titleLen > 0 ? renderer.getTextWidth(UI_10_FONT_ID, titleBuf, EpdFontFamily::ITALIC) : 0;
  const int sepW = titleLen > 0 ? renderer.getSpaceWidth(UI_10_FONT_ID, EpdFontFamily::ITALIC) : 0;
  const int wPage = renderer.getTextWidth(SMALL_FONT_ID, pageBuf);
  const int x0 = (renderer.getScreenWidth() - (wTitle + sepW + wPage)) / 2;
  // Bottom-align the shorter page font to the title baseline-ish.
  const int dy = renderer.getLineHeight(UI_10_FONT_ID) - renderer.getLineHeight(SMALL_FONT_ID);

  if (titleLen > 0) renderer.drawText(UI_10_FONT_ID, x0, y, titleBuf, true, EpdFontFamily::ITALIC);
  renderer.drawText(SMALL_FONT_ID, x0 + wTitle + sepW, y + dy, pageBuf, true);
}

}  // namespace

namespace FlashcardCardFace {

void render(GfxRenderer& renderer, int contentTop, int contentBottom, int pageWidth, const std::string& word,
            const std::string& excerpt, const std::string& chapter, bool showWord) {
  const int bodyFont = CrossPointSettings::getInstance().getDefinitionFontId();
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Bold word header — drawn only when the answer is shown. Cloze front omits it
  // (the row stays blank) so the excerpt below keeps the same position either way.
  if (showWord) {
    renderer.drawCenteredText(NOTOSERIF_18_FONT_ID, contentTop + metrics.listRowHeight, word.c_str(), true,
                              EpdFontFamily::BOLD);
  }

  if (!excerpt.empty()) {
    // Excerpt with the word underlined in context; masked (white-boxed) when hidden.
    drawWrappedCentered(renderer, bodyFont, contentTop + metrics.listRowHeight * 3, contentBottom, pageWidth,
                        excerpt.c_str(), word.c_str(), /*maskHighlight=*/!showWord);
  } else if (!showWord) {
    // No excerpt to blank into: fall back to a centered "____" placeholder.
    renderer.drawCenteredText(bodyFont, contentTop + metrics.listRowHeight * 3, "____");
  }

  drawChapterFooter(renderer, contentBottom, chapter);
}

}  // namespace FlashcardCardFace
