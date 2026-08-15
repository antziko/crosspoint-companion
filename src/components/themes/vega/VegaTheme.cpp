#include "VegaTheme.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "RecentBooksStore.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/EpubReaderUtils.h"
#include "activities/reader/ReadingTimeHistory.h"
#include "components/UITheme.h"
#include "components/icons/book.h"
#include "components/icons/bookmark.h"
#include "components/icons/bookmarkReturn.h"
#include "components/icons/chart.h"
#include "components/icons/cover.h"
#include "components/icons/folder.h"
#include "components/icons/highlight.h"
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
// Vertical breathing room between the book name and the chapter title in the
// hero top block, so the two read as distinct lines rather than one run-on.
constexpr int kHeroChapterGap = 8;
constexpr int kLineGap = 4;
constexpr int kNextThumbGap = 6;
constexpr int kProgressBarHeight = 12;
constexpr int kSelectionOutlineW = 3;
// White breathing room between a cover/thumbnail edge and its selection outline.
// The page background behind the cover is white, so pushing the outline this far
// out leaves a visible gap ring -- without it the (black) outline blends into an
// all-black cover and the selection reads as invisible.
constexpr int kSelectionGap = 2;
// Total offset of the outline's outer edge from the cover edge (gap + stroke).
constexpr int kSelectionInset = kSelectionGap + kSelectionOutlineW;

// Plain English day/month abbreviations -- mirrors HalClock::formatDate()'s
// kDowNames/kMonthNames (HalClock.cpp:248) and the MONTH_ABBR convention
// already established in ReadingStatsActivity.cpp/BookStatsActivity.cpp:
// short calendar labels stay outside tr(STR_*) so they match the RTC-driven
// date strings drawn everywhere else in the UI.
constexpr const char* DOW_ABBR[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
constexpr const char* MONTH_ABBR[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

void formatLastRead(uint32_t dayIndex, uint8_t hour, uint8_t minute, char* buf, size_t len) {
  // dayIndex/hour/minute are stored as local-calendar values -- EpubReaderActivity's
  // onExit captures them via HalClock::getLocalDateTime(SETTINGS.clockUtcOffsetQ, ...),
  // which already applies the UTC-offset+rollover arithmetic (HalClock.cpp) before
  // persisting (see BookReadingStats::lastReadDayIndex). No further correction here --
  // re-applying the offset at display time would double-shift the stamp.
  uint16_t year;
  uint8_t month, day;
  readingHistoryDateFromDayIndex(dayIndex, year, month, day);
  const uint8_t dow = readingHistoryDayOfWeek(dayIndex);
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
  bool hasTodayDuration = false;
  char todayDurationText[24] = {};
  bool hasEstRemaining = false;
  char estRemainingText[24] = {};
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
    const float chapterProgress = static_cast<float>(progress.pageNumber + 1) / static_cast<float>(progress.pageCount);
    const float percent = epub.calculateProgress(progress.spineIndex, chapterProgress) * 100.0f;
    details.progressPercent = std::clamp(static_cast<int>(percent + 0.5f), 0, 100);
    details.hasProgress = true;
    const int tocIndex = epub.getTocIndexForSpineIndex(progress.spineIndex);
    if (tocIndex >= 0) {
      details.chapterTitle = epub.getTocItem(tocIndex).title;
    }
  }

  const BookReadingStats stats = BookReadingStats::load(epub.getCachePath());
  // Cross-device total (local counter + last-synced remote sum from KOReader stats sync).
  if (stats.displayTotalSeconds() > 0) {
    BookReadingStats::formatDuration(stats.displayTotalSeconds(), details.durationText, sizeof(details.durationText));
    details.hasDuration = true;
  }
  // Mirrors BookStatsActivity's "Est. left" estimate (BookStatsActivity.cpp:145-159):
  // cross-device total time projected across the remaining progress.
  if (details.hasProgress && details.hasDuration && details.progressPercent > 0 && details.progressPercent < 100) {
    const uint64_t remaining =
        (static_cast<uint64_t>(stats.displayTotalSeconds()) * static_cast<uint64_t>(100 - details.progressPercent)) /
        static_cast<uint64_t>(details.progressPercent);
    BookReadingStats::formatDuration(static_cast<uint32_t>(std::min<uint64_t>(remaining, UINT32_MAX)),
                                     details.estRemainingText, sizeof(details.estRemainingText));
    details.hasEstRemaining = true;
  }
  // "Last read" shows the most recent dated session across devices.
  const bool remoteNewer = stats.remoteLastReadDayIndex > stats.lastReadDayIndex;
  const uint32_t lastDay = remoteNewer ? stats.remoteLastReadDayIndex : stats.lastReadDayIndex;
  if (lastDay != 0) {
    formatLastRead(lastDay, remoteNewer ? stats.remoteLastReadHour : stats.lastReadHour,
                   remoteNewer ? stats.remoteLastReadMinute : stats.lastReadMinute, details.lastReadText,
                   sizeof(details.lastReadText));
    details.hasLastRead = true;
  }

  // Today's reading time. Always shown (defaults to 0) once the book has a
  // reading position, so the hero line stays put across days. The anchor-day
  // total is adopted only when the clock confirms that anchor day is the local
  // "today"; without a usable clock (e.g. X3 with no/unset RTC) we can't
  // attribute a day, so it stays 0 rather than hiding the line.
  if (details.hasProgress) {
    snprintf(details.todayDurationText, sizeof(details.todayDurationText), "0s");
    details.hasTodayDuration = true;
    if (halClock.isAvailable()) {
      uint8_t dow = 0, day = 0, month = 0, hour = 0, minute = 0;
      uint16_t year = 0;
      // Local calendar day, not raw getDate(): recordReadingSession buckets
      // heatmapAnchorDay by getLocalDateTime, so the "today" comparison must
      // use the same basis or the count vanishes whenever UTC and local dates
      // differ (e.g. local 00:00-08:00 at UTC+8).
      if (halClock.getLocalDateTime(SETTINGS.clockUtcOffsetQ, dow, day, month, year, hour, minute)) {
        const uint32_t todayIdx = readingHistoryDayIndex(year, month, day);
        auto history = makeUniqueNoThrow<ReadingTimeHistory>();
        if (history) {
          ReadingTimeHistory::load(epub.getCachePath() + "/book_time_history.bin", *history);
          if (history->heatmapAnchorDay == todayIdx && history->heatmapAnchorSeconds > 0) {
            BookReadingStats::formatDuration(history->heatmapAnchorSeconds, details.todayDurationText,
                                             sizeof(details.todayDurationText));
          }
        }
      }
    }
  }

  return details;
}

HeroDetails cachedHeroDetails;

// hasCover result for each "next 3" tile, baked alongside the cover bitmaps so
// the per-frame redraw knows whether the title was drawn inside the placeholder
// (coverless) -- in which case the title below the tile is suppressed.
bool cachedNextHasCover[3] = {false, false, false};

// Shared cover-tile drawing for the hero card and the "next 3" row. Reuses the
// single cached thumbnail (UITheme::getCoverThumbPath at the theme's configured
// homeCoverHeight -- the only resolution HomeActivity::loadRecentCovers ever
// generates) and crops it into whatever tile size is requested (no scaling, see
// crop comment below), so no extra per-size thumbnail generation is needed.
// Mirrors Lyra3CoversTheme::drawRecentBookCover's load-or-placeholder pattern
// (Lyra3CoversTheme.cpp:42-81).
// A tile with a cover shows just the photo; the frame is drawn only for the
// empty placeholder (no photo). When a title is supplied, the placeholder wraps
// and centres it inside the frame (UTF-8-safe ellipsis on overflow) so a
// coverless book is identifiable from the tile itself; the generic cover icon is
// the fallback when no title is given.
// Returns true if a real cover bitmap was drawn (false = placeholder).
bool drawCoverTile(const GfxRenderer& renderer, const std::string& coverBmpPath, int sourceHeight, int tileX, int tileY,
                   int tileW, int tileH, const std::string& title = "") {
  // White-fill the tile first: this redraw happens over a restored cover-buffer
  // snapshot that may hold the previous pass's placeholder icon, and drawBitmap
  // composites dark-only (white pixels never overwrite), so without the clear
  // the icon ghosts through light areas of the cover.
  renderer.fillRect(tileX, tileY, tileW, tileH, false);
  bool hasCover = false;
  if (!coverBmpPath.empty()) {
    const std::string coverThumbPath = UITheme::getCoverThumbPath(coverBmpPath, sourceHeight);
    HalFile file;
    if (Storage.openFileForRead("HOME", coverThumbPath, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        const float coverWidth = static_cast<float>(bitmap.getWidth());
        const float coverHeight = static_cast<float>(bitmap.getHeight());
        // Crop (never scale) the thumbnail to the tile on both axes so
        // drawBitmap's fitScale stays at 1.0: the hero tile matches the
        // thumbnail's generation height (no vertical crop), the "next 3"
        // tiles are shorter and trim symmetric top/bottom slivers instead of
        // downscaling (which darkens the pre-dithered 1-bit bitmap). A
        // negative crop (tile larger than cover on that axis) is clamped to
        // no-crop by drawBitmap.
        const float cropX = 1.0f - static_cast<float>(tileW) / coverWidth;
        const float cropY = 1.0f - static_cast<float>(tileH) / coverHeight;
        // A cover narrower than the tile (cropX < 0, so no horizontal crop)
        // would sit flush-left. Center it within the tile.
        int drawX = tileX;
        if (bitmap.getWidth() < tileW) {
          drawX = tileX + (tileW - bitmap.getWidth()) / 2;
        }
        renderer.drawBitmap(bitmap, drawX, tileY, tileW, tileH, cropX, cropY);
        hasCover = true;
      }
      file.close();
    }
  }
  if (!hasCover) {
    renderer.drawRect(tileX, tileY, tileW, tileH, true);
    if (!title.empty()) {
      // Wrap the title into the tile (minus a small inset) and centre the block
      // vertically. wrappedText UTF-8-safely ellipsizes any overflow.
      constexpr int kInset = 4;
      const int innerW = tileW - 2 * kInset;
      const int lineH = renderer.getLineHeight(SMALL_FONT_ID);
      const int maxLines = std::max(1, (tileH - 2 * kInset) / lineH);
      const auto lines = renderer.wrappedText(SMALL_FONT_ID, title.c_str(), innerW, maxLines);
      int lineY = tileY + (tileH - static_cast<int>(lines.size()) * lineH) / 2;
      for (const auto& line : lines) {
        const int lineW = renderer.getTextWidth(SMALL_FONT_ID, line.c_str());
        renderer.drawText(SMALL_FONT_ID, tileX + (tileW - lineW) / 2, lineY, line.c_str(), true);
        lineY += lineH;
      }
    } else {
      renderer.drawIcon(CoverIcon, tileX + (tileW - 32) / 2, tileY + (tileH / 3 - 32) / 2, 32);
    }
  }
  return hasCover;
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
  // Row thumbnails are shorter than the hero: same cached thumbnail (generated
  // at coverH == homeCoverHeight), drawn with a symmetric vertical crop down to
  // kNextRowCoverHeight by drawCoverTile. Cropping keeps drawBitmap's fitScale
  // at 1.0 -- any scale < 1.0 nearest-neighbour-collapses the pre-dithered
  // 1-bit cover bitmap and visibly darkens it (OR-only-dark compositing biases
  // toward black). nextThumbW fills its slot (minus a thin gap so adjacent
  // covers don't touch); the crop ratios keep fitScale within ~0.999 of 1.0 --
  // no visible collapse.
  const int nextThumbH = VegaMetrics::kNextRowCoverHeight;
  const int nextThumbW = nextTileW - kNextThumbGap;
  const int nextCount = std::min(static_cast<int>(recentBooks.size()) - 1, 3);

  // Hero text block. Static for a given hero book -- nothing in it depends on selectorIndex --
  // so it belongs in the snapshot alongside the cover bitmaps, not in the per-frame path. It
  // used to be redrawn every frame, which on a CJK title is not the cheap operation the old
  // comment claimed: each wrappedText/drawText pulls Han glyphs through the SD font, and on this
  // screen the glyph ring is pinned at its 2048-byte floor (device log: home sits at ~23KB free,
  // under both SdCardFont floors), so most glyphs were re-read from SD at ~27ms each on every
  // press. A lambda rather than a member function: it needs a dozen geometry locals, and called
  // directly (never stored in a std::function) it costs nothing.
  auto drawHeroText = [&]() {
    const int titleLineH = renderer.getLineHeight(UI_10_FONT_ID);
    const int textLineH = renderer.getLineHeight(SMALL_FONT_ID);
    int textY = coverY;

    const HeroDetails& details = cachedHeroDetails;

    // Bottom block height: progress-section + today + lastRead.
    // Chapter moves to the top block so is excluded here.
    int detailBlockH = 0;
    if (details.hasProgress) {
      detailBlockH += textLineH + kLineGap;           // "xx% - duration" label
      detailBlockH += kProgressBarHeight + kLineGap;  // bar
      if (details.hasTodayDuration) {
        detailBlockH += textLineH + kLineGap;  // "Today: X" line
      }
    } else if (details.hasDuration) {
      detailBlockH += textLineH + kLineGap;  // duration alone
    }
    if (details.hasLastRead) {
      detailBlockH += textLineH;  // last element, no trailing gap
    }

    // Top block: title lines + gap + chapter. Title and chapter share the space
    // above the bottom-anchored detail block. The chapter wraps fully (no 1-line
    // ellipsis) into whatever height remains after the book name, separated by a
    // readable gap. One chapter line is reserved up front so a long book name can't
    // crowd the chapter out entirely.
    const int topBlockH = coverH - detailBlockH;
    const bool hasChapter = !details.chapterTitle.empty();
    const int chapterGap = hasChapter ? kHeroChapterGap : 0;
    const int minChapterH = hasChapter ? textLineH : 0;
    const int availableForTitle = topBlockH - chapterGap - minChapterH;
    const int dynamicTitleMaxLines = std::max(1, availableForTitle / titleLineH);
    const int titleMaxLines = std::min(kHeroTitleMaxLines, dynamicTitleMaxLines);

    // Draw book name (top-aligned)
    const std::string& heroTitle = hero.title.empty() ? hero.path : hero.title;
    const auto titleLines = renderer.wrappedText(UI_10_FONT_ID, heroTitle.c_str(), textW, titleMaxLines);
    for (const auto& line : titleLines) {
      renderer.drawText(UI_10_FONT_ID, textX, textY, line.c_str(), true, EpdFontFamily::BOLD);
      textY += titleLineH;
    }

    // Gap + chapter (top-aligned, wraps fully into the remaining top-block height)
    if (hasChapter) {
      textY += chapterGap;
      const int remainingH = (coverY + topBlockH) - textY;
      const int chapterMaxLines = std::max(1, remainingH / textLineH);
      const auto chapterLines =
          renderer.wrappedText(SMALL_FONT_ID, details.chapterTitle.c_str(), textW, chapterMaxLines);
      for (const auto& line : chapterLines) {
        renderer.drawText(SMALL_FONT_ID, textX, textY, line.c_str(), true);
        textY += textLineH;
      }
    }

    // Bottom block anchored to cover bottom edge
    textY = coverY + coverH - detailBlockH;

    // "xx% - duration" rides one line above the bar, aligned over the fill edge.
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

      if (details.hasTodayDuration) {
        char todayLine[40];
        snprintf(todayLine, sizeof(todayLine), "%s: %s", tr(STR_STATS_TODAY), details.todayDurationText);
        renderer.drawText(SMALL_FONT_ID, textX, textY, todayLine, true);
        if (details.hasEstRemaining) {
          const int estLineW = renderer.getTextWidth(SMALL_FONT_ID, details.estRemainingText);
          renderer.drawText(SMALL_FONT_ID, textX + textW - estLineW, textY, details.estRemainingText, true);
        }
        textY += textLineH + kLineGap;
      }
    } else if (details.hasDuration) {
      renderer.drawText(SMALL_FONT_ID, textX, textY, details.durationText, true);
      textY += textLineH + kLineGap;
    }

    if (details.hasLastRead) {
      renderer.drawText(SMALL_FONT_ID, textX, textY, details.lastReadText, true);
    }
  };

  // "Next 3" tile captions. Static per hero book for the same reason as the hero text, so it is
  // snapshotted with it; only the tile's selection outline below tracks selectorIndex.
  auto drawNextTitles = [&]() {
    for (int i = 0; i < nextCount; i++) {
      // Coverless tiles render the title inside the placeholder, so skip the
      // duplicate title below the tile.
      if (!cachedNextHasCover[i]) continue;
      const int slotX = rect.x + padding + i * nextTileW;
      const std::string& title = recentBooks[i + 1].title.empty() ? recentBooks[i + 1].path : recentBooks[i + 1].title;
      const auto titleLines = renderer.wrappedText(SMALL_FONT_ID, title.c_str(), nextTileW - 4, 2);
      int lineY = nextRowY + nextThumbH + kLineGap;
      for (const auto& line : titleLines) {
        const int lineW = renderer.getTextWidth(SMALL_FONT_ID, line.c_str());
        renderer.drawText(SMALL_FONT_ID, slotX + (nextTileW - lineW) / 2, lineY, line.c_str(), true);
        lineY += nextLineH;
      }
    }
  };

  // Expensive work (SD bitmap reads, epub/progress/stats I/O for the hero card text) happens once
  // per cover-bitmap generation, then gets snapshotted -- identical lifecycle to
  // Lyra3CoversTheme's per-tile bitmap loads. The static text is drawn BEFORE storeCoverBuffer()
  // so the snapshot carries it too; HomeActivity::restoreCoverBuffer() then blits both back on
  // every later frame and nothing here has to redraw them.
  if (!coverRendered) {
    drawCoverTile(renderer, hero.coverBmpPath, coverH, coverX, coverY, coverW, coverH);
    cachedHeroDetails = loadHeroDetails(hero);

    for (int i = 0; i < nextCount; i++) {
      const int slotX = rect.x + padding + i * nextTileW;
      const int thumbX = slotX + (nextTileW - nextThumbW) / 2;
      const RecentBook& next = recentBooks[i + 1];
      cachedNextHasCover[i] = drawCoverTile(renderer, next.coverBmpPath, coverH, thumbX, nextRowY, nextThumbW,
                                            nextThumbH, next.title.empty() ? next.path : next.title);
    }

    drawHeroText();
    drawNextTitles();

    coverBufferStored = storeCoverBuffer();
    coverRendered = coverBufferStored;
  } else if (!bufferRestored) {
    // The snapshot could not be taken (OOM) or could not be blitted back this frame, so the
    // framebuffer holds neither. Redraw the text, exactly as every frame used to. The cover
    // bitmaps stay missing here because they sit behind !coverRendered -- pre-existing.
    drawHeroText();
    drawNextTitles();
  }

  // The only per-frame work: selection outlines, which must track selectorIndex.
  if (selectorIndex == 0) {
    renderer.drawRoundedRect(coverX - kSelectionInset, coverY - kSelectionInset, coverW + 2 * kSelectionInset,
                             coverH + 2 * kSelectionInset, kSelectionOutlineW, kCornerRadius, true);
  }
  for (int i = 0; i < nextCount; i++) {
    if (selectorIndex != i + 1) continue;
    const int slotX = rect.x + padding + i * nextTileW;
    const int thumbX = slotX + (nextTileW - nextThumbW) / 2;
    renderer.drawRoundedRect(thumbX - kSelectionInset, nextRowY - kSelectionInset, nextThumbW + 2 * kSelectionInset,
                             nextThumbH + 2 * kSelectionInset, kSelectionOutlineW, kCornerRadius, true);
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
    case UIIcon::BookmarkRibbon:
      return BookmarkIcon;
    case UIIcon::BookmarkReturn:
      return BookmarkReturnIcon;
    case UIIcon::Highlight:
      return HighlightIcon;
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
        renderer.drawIcon(bmp, iconX, iconY, kMenuIconSize);
      }
    }
  }

  renderer.fillRect(0, labelY, renderer.getScreenWidth(), labelLineH, false);
  if (selectedIndex >= 0 && selectedIndex < buttonCount && buttonLabel != nullptr) {
    const std::string labelStr = buttonLabel(selectedIndex);
    const auto centeredLabel =
        renderer.truncatedText(kMenuLabelFontId, labelStr.c_str(), renderer.getScreenWidth() - 40);
    const int labelW = renderer.getTextWidth(kMenuLabelFontId, centeredLabel.c_str());
    renderer.drawText(kMenuLabelFontId, (renderer.getScreenWidth() - labelW) / 2, labelY + 2, centeredLabel.c_str(),
                      true);
  }
}
