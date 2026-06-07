#include "VegaTheme.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/EpubReaderUtils.h"
#include "activities/reader/ReadingTimeHistory.h"
#include "components/UITheme.h"
#include "components/icons/book.h"
#include "components/icons/chart.h"
#include "components/icons/cover.h"
#include "components/icons/folder.h"
#include "components/icons/library.h"
#include "components/icons/recent.h"
#include "components/icons/settings2.h"
#include "components/icons/transfer.h"
#include "fontIds.h"

namespace {

constexpr int kCornerRadius = 6;
constexpr int kHeroTextGap = 14;
// Hero title shows the book's full name -- no ellipsis. Real titles essentially
// never wrap past this many lines at the hero column width, so this is a safety
// ceiling (against pathological/junk metadata), not a practical truncation point.
constexpr int kHeroTitleMaxLines = 6;
constexpr int kLineGap = 4;
constexpr int kNextThumbGap = 6;
constexpr int kProgressBarHeight = 12;
constexpr int kSelectionOutlineW = 3;

// Plain English day/month abbreviations -- mirrors HalClock::formatDate()'s
// kDowNames/kMonthNames (HalClock.cpp:248) and the MONTH_ABBR convention
// already established in ReadingStatsActivity.cpp/BookStatsActivity.cpp:
// short calendar labels stay outside tr(STR_*) so they match the RTC-driven
// date strings drawn everywhere else in the UI.
constexpr const char* DOW_ABBR[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
constexpr const char* MONTH_ABBR[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

void formatLastRead(uint32_t dayIndex, uint8_t hour, uint8_t minute, char* buf, size_t len) {
  // dayIndex/hour/minute are stored as raw RTC reads (EpubReaderActivity.cpp's
  // onExit -- halClock.getDate()/getTime(), no offset applied), same convention
  // as the status bar's clock. Apply SETTINGS.clockUtcOffsetQ here at display
  // time -- identical offset+rollover arithmetic to HalClock::formatTime/
  // formatDate (HalClock.cpp:108-115, 222-243) -- so "Last read on ..." shows
  // the same local time as the rest of the UI, and stays correct if the user
  // changes their UTC offset later (raw value on disk is untouched).
  // Clamp against corrupted persisted values, same guard HalClock::formatTime/
  // formatDate apply (HalClock.cpp:109) -- keeps the result in [-12:00,+14:00]
  // and keeps the single +-1440 rollover correction below valid.
  uint8_t offsetQ = SETTINGS.clockUtcOffsetQ;
  if (offsetQ > 104) offsetQ = 104;
  const int offsetMins = (static_cast<int>(offsetQ) - 48) * 15;
  int totalMins = static_cast<int>(hour) * 60 + static_cast<int>(minute) + offsetMins;
  int dayShift = 0;
  if (totalMins < 0) {
    totalMins += 1440;
    dayShift = -1;
  } else if (totalMins >= 1440) {
    totalMins -= 1440;
    dayShift = 1;
  }
  // dayIndex is sentinel-gated >= 1 by the caller (0 = "never recorded"), so
  // dayIndex - 1 can't underflow.
  const uint32_t adjustedDayIndex = static_cast<uint32_t>(static_cast<int>(dayIndex) + dayShift);
  hour = static_cast<uint8_t>(totalMins / 60);
  minute = static_cast<uint8_t>(totalMins % 60);

  uint16_t year;
  uint8_t month, day;
  readingHistoryDateFromDayIndex(adjustedDayIndex, year, month, day);
  const uint8_t dow = readingHistoryDayOfWeek(adjustedDayIndex);
  const char* dowStr = (dow >= 1 && dow <= 7) ? DOW_ABBR[dow - 1] : "???";
  const char* monStr = (month >= 1 && month <= 12) ? MONTH_ABBR[month - 1] : "?";
  char dateTime[40];
  snprintf(dateTime, sizeof(dateTime), "%s, %u %s %02u:%02u", dowStr, static_cast<unsigned>(day), monStr,
           static_cast<unsigned>(hour), static_cast<unsigned>(minute));
  snprintf(buf, len, tr(STR_HOME_LAST_READ_FORMAT), dateTime);
}

// Hero-book details: derived from real I/O (open epub, read progress.bin/stats.bin),
// too costly to redo every render. Cached here and only recomputed inside the
// !coverRendered block below -- exactly the cycle HomeActivity already drives for
// the cover bitmap (it clears coverRendered whenever recentBooks[0] changes).
// EPUB-only for progress%/chapter (XTC/TXT have no portable calculateProgress/TOC);
// "Last read" is format-agnostic in BookReadingStats but in practice is only ever
// populated by EPUB sessions (EpubReaderActivity is the sole writer).
struct HeroDetails {
  bool hasProgress = false;
  int progressPercent = 0;
  std::string chapterTitle;
  bool hasDuration = false;
  char durationText[24] = {};
  bool hasLastRead = false;
  char lastReadText[64] = {};
};

HeroDetails loadHeroDetails(const RecentBook& book) {
  HeroDetails details;
  if (!FsHelpers::hasEpubExtension(book.path)) {
    return details;
  }
  Epub epub(book.path, "/.crosspoint");
  if (!epub.load(false, true)) {
    return details;
  }

  EpubReaderUtils::Progress progress;
  if (EpubReaderUtils::loadProgress(epub, progress, "VEGA") && progress.hasPageCount && progress.pageCount > 0) {
    const float chapterProgress =
        static_cast<float>(progress.pageNumber + 1) / static_cast<float>(progress.pageCount);
    const float percent = epub.calculateProgress(progress.spineIndex, chapterProgress) * 100.0f;
    details.progressPercent = std::clamp(static_cast<int>(percent + 0.5f), 0, 100);
    details.hasProgress = true;
    const int tocIndex = epub.getTocIndexForSpineIndex(progress.spineIndex);
    if (tocIndex >= 0) {
      details.chapterTitle = epub.getTocItem(tocIndex).title;
    }
  }

  const BookReadingStats stats = BookReadingStats::load(epub.getCachePath());
  if (stats.totalReadingSeconds > 0) {
    BookReadingStats::formatDuration(stats.totalReadingSeconds, details.durationText, sizeof(details.durationText));
    details.hasDuration = true;
  }
  if (stats.lastReadDayIndex != 0) {
    formatLastRead(stats.lastReadDayIndex, stats.lastReadHour, stats.lastReadMinute, details.lastReadText,
                   sizeof(details.lastReadText));
    details.hasLastRead = true;
  }
  return details;
}

HeroDetails cachedHeroDetails;

// Shared cover-tile drawing for the hero card and the "next 3" row. Reuses the
// single cached thumbnail (UITheme::getCoverThumbPath at the theme's configured
// homeCoverHeight -- the only resolution HomeActivity::loadRecentCovers ever
// generates) and lets GfxRenderer::drawBitmap scale-to-fit + crop into whatever
// tile size is requested, so no extra per-size thumbnail generation is needed.
// Mirrors Lyra3CoversTheme::drawRecentBookCover's load-or-placeholder pattern
// (Lyra3CoversTheme.cpp:42-81).
void drawCoverTile(const GfxRenderer& renderer, const std::string& coverBmpPath, int sourceHeight, int tileX,
                   int tileY, int tileW, int tileH) {
  bool hasCover = false;
  if (!coverBmpPath.empty()) {
    const std::string coverThumbPath = UITheme::getCoverThumbPath(coverBmpPath, sourceHeight);
    HalFile file;
    if (Storage.openFileForRead("HOME", coverThumbPath, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        const float coverWidth = static_cast<float>(bitmap.getWidth());
        const float coverHeight = static_cast<float>(bitmap.getHeight());
        const float ratio = coverWidth / coverHeight;
        const float tileRatio = static_cast<float>(tileW) / static_cast<float>(tileH);
        const float cropX = 1.0f - (tileRatio / ratio);
        renderer.drawBitmap(bitmap, tileX, tileY, tileW, tileH, cropX);
        hasCover = true;
      }
      file.close();
    }
  }
  renderer.drawRect(tileX, tileY, tileW, tileH, true);
  if (!hasCover) {
    renderer.fillRect(tileX, tileY + tileH / 3, tileW, 2 * tileH / 3, true);
    renderer.drawIcon(CoverIcon, tileX + (tileW - 32) / 2, tileY + (tileH / 3 - 32) / 2, 32, 32);
  }
}

}  // namespace

void VegaTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                    const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                    bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  if (recentBooks.empty()) {
    drawEmptyRecents(renderer, rect);
    return;
  }

  const int padding = VegaMetrics::kHeroPadding;
  const int coverH = VegaMetrics::values.homeCoverHeight;
  const int coverW = coverH * 2 / 3;
  const int heroAreaH = coverH + 2 * padding;
  const int coverX = rect.x + padding;
  const int coverY = rect.y + padding;
  const int textX = coverX + coverW + kHeroTextGap;
  const int textW = rect.x + rect.width - padding - textX;

  const RecentBook& hero = recentBooks[0];

  // "Next 3" row geometry -- shared by the one-time cover-bitmap pass below and
  // the per-frame title/highlight redraw, so the two halves can't drift apart.
  const int nextRowY = rect.y + heroAreaH + VegaMetrics::kSectionGap;
  const int nextTileW = (rect.width - 2 * padding) / 3;
  const int nextLineH = renderer.getLineHeight(SMALL_FONT_ID);
  // Draw at *native* coverH (== homeCoverHeight, the cached-thumbnail's
  // generation height) so GfxRenderer::drawBitmap's fitScale lands at 1.0 --
  // any scale < 1.0 nearest-neighbour-collapses the pre-dithered 1-bit cover
  // bitmap and visibly darkens it (OR-only-dark compositing biases toward
  // black). nextThumbW fills its slot (minus a thin gap so adjacent covers
  // don't touch); the resulting crop ratio keeps fitScale within ~0.999 of
  // 1.0 -- no visible collapse.
  const int nextThumbH = coverH;
  const int nextThumbW = nextTileW - kNextThumbGap;
  const int nextCount = std::min(static_cast<int>(recentBooks.size()) - 1, 3);

  // Expensive work (SD bitmap reads, epub/progress/stats I/O for the hero card
  // text) happens once per cover-bitmap generation, then gets snapshotted --
  // identical lifecycle to Lyra3CoversTheme's per-tile bitmap loads.
  if (!coverRendered) {
    drawCoverTile(renderer, hero.coverBmpPath, coverH, coverX, coverY, coverW, coverH);
    cachedHeroDetails = loadHeroDetails(hero);

    for (int i = 0; i < nextCount; i++) {
      const int slotX = rect.x + padding + i * nextTileW;
      const int thumbX = slotX + (nextTileW - nextThumbW) / 2;
      drawCoverTile(renderer, recentBooks[i + 1].coverBmpPath, coverH, thumbX, nextRowY, nextThumbW, nextThumbH);
    }

    coverBufferStored = storeCoverBuffer();
    coverRendered = coverBufferStored;
  }

  // Cheap per-frame redraw: hero text block (cached, no I/O) + "next 3" titles
  // and selection highlight, which must track selectorIndex every render.
  const bool heroSelected = (selectorIndex == 0);
  if (heroSelected) {
    renderer.drawRoundedRect(coverX - kSelectionOutlineW, coverY - kSelectionOutlineW, coverW + 2 * kSelectionOutlineW,
                             coverH + 2 * kSelectionOutlineW, kSelectionOutlineW, kCornerRadius, true);
  }

  const int titleLineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int textLineH = renderer.getLineHeight(SMALL_FONT_ID);
  int textY = coverY;

  const HeroDetails& details = cachedHeroDetails;

  // Sum the exact heights the detail block below will occupy (mirrors the
  // sequential textY += additions in the draw calls further down -- if those
  // ever change, this must change with them) so the title's line budget can be
  // sized to guarantee title + gap + details fits within coverH. Without this,
  // a long title could push progress/bar/duration/chapter/last-read below the
  // hero cover's bottom edge into the "Next 3" row.
  int detailBlockH = 0;
  if (details.hasProgress) {
    detailBlockH += textLineH + kLineGap;            // "xx% - duration" label (combined, tracks bar fill)
    detailBlockH += kProgressBarHeight + kLineGap;   // bar
  } else if (details.hasDuration) {
    detailBlockH += textLineH + kLineGap;            // duration alone, left-aligned
  }
  if (!details.chapterTitle.empty()) {
    detailBlockH += textLineH + kLineGap;            // wrappedText(..., 1) -- always 1 line
  }
  if (details.hasLastRead) {
    detailBlockH += textLineH;                       // last element, no trailing gap
  }

  // Largest line count that still leaves room for the detail block within
  // coverH, capped by kHeroTitleMaxLines (the "don't ramble forever" ceiling).
  const int availableForTitle = coverH - kLineGap - detailBlockH;
  const int dynamicTitleMaxLines = std::max(1, availableForTitle / titleLineH);
  const int titleMaxLines = std::min(kHeroTitleMaxLines, dynamicTitleMaxLines);

  const std::string& heroTitle = hero.title.empty() ? hero.path : hero.title;
  const auto titleLines = renderer.wrappedText(UI_10_FONT_ID, heroTitle.c_str(), textW, titleMaxLines);
  for (const auto& line : titleLines) {
    renderer.drawText(UI_10_FONT_ID, textX, textY, line.c_str(), true, EpdFontFamily::BOLD);
    textY += titleLineH;
  }

  // Detail block (progress/duration/chapter/last-read) bottom-aligns to the
  // hero cover's bottom edge instead of trailing the title -- titleMaxLines
  // above already guarantees title + kLineGap + detailBlockH <= coverH, so
  // this can't overlap the title even at max line count.
  textY = coverY + coverH - detailBlockH;

  // "xx% - duration" rides one line above the bar, right-aligned over the
  // bar's current fill edge (fillEdgeX) -- like a tooltip following a slider
  // thumb -- instead of sitting at the left margin. fillEdgeX is computed
  // once, shared by the label and the fill draw, so they can't drift apart.
  // Clamped so the label stays inside [textX, textX+textW] near 0%/100%.
  const int barInnerX = textX + 2;
  const int barInnerW = textW - 4;
  const int fillW = details.hasProgress ? barInnerW * details.progressPercent / 100 : 0;
  const int fillEdgeX = barInnerX + fillW;

  auto drawTrackingLabel = [&](const char* text) {
    const int labelW = renderer.getTextWidth(SMALL_FONT_ID, text);
    const int labelX = std::clamp(fillEdgeX - labelW, textX, textX + textW - labelW);
    renderer.drawText(SMALL_FONT_ID, labelX, textY, text, true);
  };

  if (details.hasProgress) {
    char label[40];
    if (details.hasDuration) {
      snprintf(label, sizeof(label), "%d%% - %s", details.progressPercent, details.durationText);
    } else {
      snprintf(label, sizeof(label), "%d%%", details.progressPercent);
    }
    drawTrackingLabel(label);
    textY += textLineH + kLineGap;

    renderer.drawRect(textX, textY, textW, kProgressBarHeight, true);
    if (fillW > 0) {
      renderer.fillRect(barInnerX, textY + 2, fillW, kProgressBarHeight - 4, true);
    }
    textY += kProgressBarHeight + kLineGap;
  } else if (details.hasDuration) {
    renderer.drawText(SMALL_FONT_ID, textX, textY, details.durationText, true);
    textY += textLineH + kLineGap;
  }

  if (!details.chapterTitle.empty()) {
    const auto chapterLines = renderer.wrappedText(SMALL_FONT_ID, details.chapterTitle.c_str(), textW, 1);
    for (const auto& line : chapterLines) {
      renderer.drawText(SMALL_FONT_ID, textX, textY, line.c_str(), true);
      textY += textLineH;
    }
    textY += kLineGap;
  }

  if (details.hasLastRead) {
    renderer.drawText(SMALL_FONT_ID, textX, textY, details.lastReadText, true);
  }

  // "Next 3" row -- titles + selection highlight redrawn every frame so the
  // highlighted tile always tracks selectorIndex (covers are baked into the
  // snapshot above; mirrors Lyra3CoversTheme.cpp:90-128's split).
  for (int i = 0; i < nextCount; i++) {
    const int slotX = rect.x + padding + i * nextTileW;
    const int thumbX = slotX + (nextTileW - nextThumbW) / 2;
    const bool selected = (selectorIndex == i + 1);
    if (selected) {
      renderer.drawRoundedRect(thumbX - kSelectionOutlineW, nextRowY - kSelectionOutlineW,
                               nextThumbW + 2 * kSelectionOutlineW, nextThumbH + 2 * kSelectionOutlineW,
                               kSelectionOutlineW, kCornerRadius, true);
    }
    const std::string& title = recentBooks[i + 1].title.empty() ? recentBooks[i + 1].path : recentBooks[i + 1].title;
    const auto titleLines = renderer.wrappedText(SMALL_FONT_ID, title.c_str(), nextTileW - 4, 2);
    int lineY = nextRowY + nextThumbH + kLineGap;
    for (const auto& line : titleLines) {
      const int lineW = renderer.getTextWidth(SMALL_FONT_ID, line.c_str());
      renderer.drawText(SMALL_FONT_ID, slotX + (nextTileW - lineW) / 2, lineY, line.c_str(), true);
      lineY += nextLineH;
    }
  }
}

// ---------------------------------------------------------------------------
// Horizontal icon-only menu row, anchored to the bottom of the screen --
// ported from CrossInk's LyraCarouselTheme::drawButtonMenu
// (uxjulia/CrossInk/.../LyraCarouselTheme.cpp:455). The passed-in rect is
// retained by the BaseTheme interface but ignored here, same as that reference.
// mcrosson's GfxRenderer has no drawIconInverted, so the selected icon is
// marked with a filled highlight behind it instead (LyraTheme::drawButtonMenu's
// own selection treatment, LyraTheme.cpp:531-533) rather than an inverted glyph.
// ---------------------------------------------------------------------------
namespace {
constexpr int kMenuLabelFontId = SMALL_FONT_ID;
constexpr int kMenuIconSize = 32;
constexpr int kMenuIconPad = 14;
constexpr int kMenuHighlightPad = 7;
constexpr int kMenuLabelTopGap = 3;

const uint8_t* vegaMenuIcon(UIIcon icon) {
  switch (icon) {
    case UIIcon::Folder:
      return FolderIcon;
    case UIIcon::Book:
      return BookIcon;
    case UIIcon::Recent:
      return RecentIcon;
    case UIIcon::Settings:
      return Settings2Icon;
    case UIIcon::Transfer:
      return TransferIcon;
    case UIIcon::Library:
      return LibraryIcon;
    case UIIcon::Chart:
      return ChartIcon;
    default:
      return nullptr;
  }
}
}  // namespace

void VegaTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                               const std::function<std::string(int index)>& buttonLabel,
                               const std::function<UIIcon(int index)>& rowIcon) const {
  if (buttonCount <= 0) return;
  (void)rect;

  const int tileH = kMenuIconPad + kMenuIconSize + kMenuIconPad;
  const int labelLineH = renderer.getLineHeight(kMenuLabelFontId);
  const int rowY =
      renderer.getScreenHeight() - VegaMetrics::values.buttonHintsHeight - tileH - kMenuLabelTopGap - labelLineH;
  const int labelY = rowY - kMenuLabelTopGap - labelLineH;
  const int tileW = renderer.getScreenWidth() / buttonCount;

  for (int i = 0; i < buttonCount; i++) {
    const int tileX = i * tileW;
    const int iconX = tileX + (tileW - kMenuIconSize) / 2;
    const int iconY = rowY + kMenuIconPad;
    const bool selected = (selectedIndex == i);
    if (selected) {
      const int highlightSize = kMenuIconSize + 2 * kMenuHighlightPad;
      const int highlightY = rowY + (tileH - highlightSize) / 2;
      renderer.fillRoundedRect(iconX - kMenuHighlightPad, highlightY, highlightSize, highlightSize, kCornerRadius,
                               Color::LightGray);
    }
    if (rowIcon != nullptr) {
      const uint8_t* bmp = vegaMenuIcon(rowIcon(i));
      if (bmp != nullptr) {
        renderer.drawIcon(bmp, iconX, iconY, kMenuIconSize, kMenuIconSize);
      }
    }
  }

  renderer.fillRect(0, labelY, renderer.getScreenWidth(), labelLineH, false);
  if (selectedIndex >= 0 && selectedIndex < buttonCount && buttonLabel != nullptr) {
    const std::string labelStr = buttonLabel(selectedIndex);
    const auto centeredLabel = renderer.truncatedText(kMenuLabelFontId, labelStr.c_str(), renderer.getScreenWidth() - 40);
    const int labelW = renderer.getTextWidth(kMenuLabelFontId, centeredLabel.c_str());
    renderer.drawText(kMenuLabelFontId, (renderer.getScreenWidth() - labelW) / 2, labelY + 2, centeredLabel.c_str(),
                      true);
  }
}
