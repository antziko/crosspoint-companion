#include "ReadingStatsActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "BookReadingStats.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"

namespace {
constexpr int SUMMARY_LINES = 1;
constexpr char STATS_CACHE_BASE_DIR[] = "/.crosspoint";

// Books-tab row height: matches the timeline list row metric (line + padding).
int bookRowHeight(GfxRenderer& renderer) { return renderer.getLineHeight(UI_10_FONT_ID) + 10; }

// Extracts the display title from a cache label filename of the form
// "<dirName>--<title>-by-<author>.txt" (see BookMetadataCache). Returns the raw
// dir name when no matching label exists — rare, since the label is written when
// book.bin is first built. Author is dropped at the last "-by-" so titles that
// happen to contain "-by-" still keep everything up to the author.
std::string titleFromLabel(const std::string& dirName, const std::vector<std::string>& labelNames) {
  const std::string prefix = dirName + "--";
  for (const auto& ln : labelNames) {
    if (ln.size() > prefix.size() && ln.compare(0, prefix.size(), prefix) == 0) {
      std::string t = ln.substr(prefix.size());
      if (t.size() >= 4 && t.compare(t.size() - 4, 4, ".txt") == 0) t.erase(t.size() - 4);
      const size_t by = t.rfind("-by-");
      if (by != std::string::npos && by > 0) t.erase(by);
      return t.empty() ? dirName : t;
    }
  }
  return dirName;
}
}  // namespace

ReadingStatsActivity::ReadingStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("ReadingStats", renderer, mappedInput) {}

void ReadingStatsActivity::onEnter() {
  Activity::onEnter();

  // One of the few non-reader screens that follows SETTINGS.displayOrientation
  // (the hold-to-rotate gesture is handled in loop(), see resolveSideNavAction).
  ReaderUtils::applyOrientation(renderer, SETTINGS.displayOrientation);

  stats = makeUniqueNoThrow<GlobalReadingStats>();
  if (stats) {
    GlobalReadingStats::load(*stats);
    // Fold the synced remote snapshot onto the local history for cross-device
    // timeline + heatmap. Built once here, reused every frame.
    displayHist = makeUniqueNoThrow<ReadingTimeHistory>();
    if (displayHist) {
      stats->displayHistory(*displayHist);
      timeline.build(*displayHist);
    } else {
      timeline.build(stats->history);  // OOM fallback: local-only view
    }
    timeline.scrollToSelection(renderer, contentRect());
  }

  // Per-book breakdown for the Books tab (one directory scan + a stats.bin read
  // per book — done once here, never per frame).
  buildBookBreakdown();

  requestUpdate();
}

void ReadingStatsActivity::buildBookBreakdown() {
  bookRows.clear();
  booksSumSeconds = 0;
  booksScrollOffset = 0;
  booksSelectedIndex = 0;

  // Single pass over /.crosspoint collecting book-cache dir names and their
  // "<dir>--<title>-by-<author>.txt" label siblings. Snapshot names with the
  // listing handle open, then close it before opening any stats.bin — SdFat must
  // not open a file while a directory scan is active (mirrors
  // BookCacheUtils::scanOrphanCaches).
  std::vector<std::string> dirNames;
  std::vector<std::string> labelNames;
  {
    HalFile dir = Storage.open(STATS_CACHE_BASE_DIR);
    if (!dir || !dir.isDirectory()) return;
    while (true) {
      HalFile f = dir.openNextFile();
      if (!f) break;
      char name[160];
      if (f.getName(name, sizeof(name)) == 0) continue;
      if (f.isDirectory()) {
        if (isBookCacheDirectoryName(name)) dirNames.emplace_back(name);
      } else if (strstr(name, "--") != nullptr) {
        labelNames.emplace_back(name);  // candidate cache label (joined by prefix below)
      }
    }
  }

  bookRows.reserve(dirNames.size());
  for (const auto& dirName : dirNames) {
    const std::string cachePath = std::string(STATS_CACHE_BASE_DIR) + "/" + dirName;
    const uint32_t secs = BookReadingStats::load(cachePath).displayTotalSeconds();
    if (secs == 0) continue;  // only books actually read (this device or another)
    bookRows.push_back({titleFromLabel(dirName, labelNames), secs, dirName});
    // uint32 sum: even 4 billion seconds is ~136 years — no realistic overflow.
    booksSumSeconds += secs;
  }

  std::sort(bookRows.begin(), bookRows.end(),
            [](const BookStatRow& a, const BookStatRow& b) { return a.allDevicesSeconds > b.allDevicesSeconds; });
}

int ReadingStatsActivity::booksVisibleRows(const Rect& content) const {
  const int rowH = bookRowHeight(renderer);
  if (rowH <= 0) return 0;
  // Reserve one row at the bottom for the pinned "sum of books" subtotal.
  return std::max(0, (content.height - rowH) / rowH);
}

int ReadingStatsActivity::booksMaxScroll(const Rect& content) const {
  return std::max(0, static_cast<int>(bookRows.size()) - booksVisibleRows(content));
}

void ReadingStatsActivity::scrollSelectedBookIntoView(const Rect& content) {
  const int visible = booksVisibleRows(content);
  if (visible <= 0) return;
  if (booksSelectedIndex < booksScrollOffset) {
    booksScrollOffset = booksSelectedIndex;
  } else if (booksSelectedIndex >= booksScrollOffset + visible) {
    booksScrollOffset = booksSelectedIndex - visible + 1;
  }
}

void ReadingStatsActivity::promptDeleteBook() {
  if (booksSelectedIndex < 0 || booksSelectedIndex >= static_cast<int>(bookRows.size())) return;
  // Copy by value: the row is erased inside the handler, so it can't be referenced then.
  const std::string dirName = bookRows[booksSelectedIndex].dirName;
  const std::string title = bookRows[booksSelectedIndex].title;
  // Resolve now, not in the handler: content_id.bin lives inside the dir about to be
  // deleted. Empty on a legacy cache with no fingerprint — the cache still goes, only the
  // path-keyed sidecars are then unreachable.
  const std::string bookPath = recordedBookPathForCache(dirName);

  auto handler = [this, dirName, bookPath](const ActivityResult& res) {
    if (res.isCancelled) return;
    if (!removeBookCache(dirName)) return;  // removeBookCache already logged; keep the row
    // Leave the book as if never opened: bookmarks, recents entry, resume pointer.
    if (!bookPath.empty()) forgetBookSidecars(bookPath);

    // Drop the row locally (no full rescan): subtract its time, erase, then clamp the
    // selection and scroll window to the shrunken list.
    auto it =
        std::find_if(bookRows.begin(), bookRows.end(), [&](const BookStatRow& r) { return r.dirName == dirName; });
    if (it != bookRows.end()) {
      booksSumSeconds -= it->allDevicesSeconds;
      bookRows.erase(it);
    }
    if (booksSelectedIndex >= static_cast<int>(bookRows.size())) {
      booksSelectedIndex = std::max(0, static_cast<int>(bookRows.size()) - 1);
    }
    const int maxScroll = booksMaxScroll(contentRect());
    if (booksScrollOffset > maxScroll) booksScrollOffset = maxScroll;
    scrollSelectedBookIntoView(contentRect());
    requestUpdate(true);
  };

  // Heading states the effect (all reading data, not just the render cache); body is the title.
  startActivityForResultNoThrow<ConfirmationActivity>(std::move(handler), renderer, mappedInput,
                                                      tr(STR_STATS_FORGET_BOOK), title);
}

void ReadingStatsActivity::openSelectedBook() {
  if (booksSelectedIndex < 0 || booksSelectedIndex >= static_cast<int>(bookRows.size())) return;
  const std::string bookPath = recordedBookPathForCache(bookRows[booksSelectedIndex].dirName);

  // No fingerprint (legacy cache) or the file has since been deleted/moved off the card.
  // Toast rather than navigate: goToReader() replaces this screen, and a failed load only
  // finishes back to the home screen with nothing said.
  if (bookPath.empty() || !Storage.exists(bookPath.c_str())) {
    LOG_ERR("ReadingStats", "Cannot open %s: no book path", bookRows[booksSelectedIndex].dirName.c_str());
    GUI.drawPopup(renderer, tr(STR_STATS_BOOK_MISSING));
    delay(900);
    renderer.forceCleanRefreshNextPaint();  // stop the popup box ghosting under the list
    requestUpdate();
    return;
  }

  onSelectBook(bookPath);
}

void ReadingStatsActivity::onExit() {
  Activity::onExit();

  // Reset orientation back to portrait for the rest of the UI.
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
}

Rect ReadingStatsActivity::tabBarRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Second summary line only when a stats sync has brought in time from another
  // device (must match the render() condition or tabs and content drift apart).
  const int summaryLines = (stats && stats->remoteOtherSeconds > 0) ? SUMMARY_LINES + 1 : SUMMARY_LINES;
  const int summaryHeight = summaryLines * (renderer.getLineHeight(SMALL_FONT_ID) + 2);
  const int y =
      metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + summaryHeight + metrics.verticalSpacing;
  return Rect{0, y, renderer.getScreenWidth(), metrics.tabBarHeight};
}

Rect ReadingStatsActivity::contentRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect tabs = tabBarRect();
  const int top = tabs.y + tabs.height + metrics.verticalSpacing;
  const int height = renderer.getScreenHeight() - top - metrics.buttonHintsHeight - metrics.verticalSpacing;
  return Rect{0, top, renderer.getScreenWidth(), std::max(0, height)};
}

std::vector<TabInfo> ReadingStatsActivity::buildTabs() const {
  return {{tr(STR_STATS_TIMELINE), selectedTab == Tab::Timeline},
          {tr(STR_STATS_HEATMAP), selectedTab == Tab::Heatmap},
          {tr(STR_STATS_BOOKS), selectedTab == Tab::Books}};
}

void ReadingStatsActivity::selectTab(const int index) {
  if (index < 0 || index >= TAB_COUNT) return;
  const Tab next = TAB_ORDER[index];
  if (next == selectedTab) return;
  selectedTab = next;
  timeline.resetFocus();   // leaving Timeline: drop any year/month cursor
  booksScrollOffset = 0;   // re-entering Books starts at the top
  booksSelectedIndex = 0;  // ...with the first row selected
  requestUpdate();
}

bool ReadingStatsActivity::handleTouch() {
  int tx = 0;
  int ty = 0;

  // Tab bar: a tap picks a tab outright instead of cycling towards it.
  if (mappedInput.wasScreenTapped(tx, ty)) {
    int tab = 0;
    if (GUI.tabIndexFromPoint(renderer, tabBarRect(), buildTabs(), tx, ty, tab)) {
      selectTab(tab);
      return true;
    }
  }

  const Rect content = contentRect();

  if (selectedTab == Tab::Books) {
    if (bookRows.empty()) return false;
    const int rowH = bookRowHeight(renderer);
    const int shown = std::min(booksVisibleRows(content), static_cast<int>(bookRows.size()) - booksScrollOffset);

    // Hold on a row prompts to clear it, the touch counterpart of the Confirm
    // hold. wasScreenLongPress swallows the rest of the contact, so the finger
    // lift cannot also answer the prompt it opens.
    if (mappedInput.wasScreenLongPress(tx, ty)) {
      const int row = (rowH > 0 && ty >= content.y) ? (ty - content.y) / rowH : -1;
      if (row >= 0 && row < shown) {
        booksSelectedIndex = booksScrollOffset + row;
        promptDeleteBook();
        return true;
      }
    }

    int row = 0;
    switch (mappedInput.rowTouch(row, content.y, rowH, shown)) {
      case MappedInputManager::RowTouch::Down:
        booksSelectedIndex = booksScrollOffset + row;
        requestUpdate();
        return true;
      case MappedInputManager::RowTouch::Tap:
        booksSelectedIndex = booksScrollOffset + row;
        openSelectedBook();
        return true;
      case MappedInputManager::RowTouch::None:
        break;
    }
    return false;
  }

  // Timeline: a tap on a year/month cell drills the focus into that section and
  // selects the cell, the touch counterpart of Down-then-Left/Right.
  if (selectedTab == Tab::Timeline && mappedInput.wasScreenTapped(tx, ty)) {
    const ReadingTimeHistory* h = displayHist ? displayHist.get() : (stats ? &stats->history : nullptr);
    if (h && timeline.selectAtPoint(*h, renderer, content, tx, ty)) {
      timeline.scrollToSelection(renderer, content);
      requestUpdate();
      return true;
    }
  }

  return false;
}

void ReadingStatsActivity::loop() {
  // After a long-press delete has fired, swallow input until Confirm is physically
  // released so the release doesn't re-arm the gesture (cf. RecentBooksActivity).
  if (booksLongPressFired) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) booksLongPressFired = false;
    return;
  }

  const ReadingTimeHistory* h = displayHist ? displayHist.get() : (stats ? &stats->history : nullptr);

  // Left/Right: at the tab bar (focus None) cycle Timeline/Heatmap/Books; inside a
  // focused Timeline section move the year/month selection instead.
  auto cycleTab = [&](int delta) {
    int idx = 0;
    for (int i = 0; i < TAB_COUNT; ++i)
      if (TAB_ORDER[i] == selectedTab) idx = i;
    selectTab((idx + delta + TAB_COUNT) % TAB_COUNT);
  };

  if (mappedInput.hasTouch() && handleTouch()) return;

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    if (timeline.focus() == StatsTimelineView::Focus::None) {
      cycleTab(-1);
    } else if (h && timeline.selectPrev(*h)) {
      timeline.scrollToSelection(renderer, contentRect());
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    if (timeline.focus() == StatsTimelineView::Focus::None) {
      cycleTab(1);
    } else if (h && timeline.selectNext(*h)) {
      timeline.scrollToSelection(renderer, contentRect());
      requestUpdate();
    }
    return;
  }

  // Physical side Up/Down: move the focus down/up the levels (tab -> Yearly ->
  // Monthly) on the Timeline tab -- holding them is reserved for the
  // display-orientation-cycle gesture (both tabs).
  switch (ReaderUtils::resolveSideNavAction(mappedInput, MappedInputManager::Button::Down)) {
    case ReaderUtils::SideNavAction::STEP:
      if (selectedTab == Tab::Timeline && h && timeline.focusIn()) {
        timeline.scrollToSelection(renderer, contentRect());
        requestUpdate();
      } else if (selectedTab == Tab::Books && booksSelectedIndex + 1 < static_cast<int>(bookRows.size())) {
        booksSelectedIndex++;
        scrollSelectedBookIntoView(contentRect());
        requestUpdate();
      }
      break;
    case ReaderUtils::SideNavAction::ROTATE:
      ReaderUtils::cycleDisplayOrientation(renderer, -1);
      requestUpdate();
      break;
    case ReaderUtils::SideNavAction::NONE:
      break;
  }
  switch (ReaderUtils::resolveSideNavAction(mappedInput, MappedInputManager::Button::Up)) {
    case ReaderUtils::SideNavAction::STEP:
      if (selectedTab == Tab::Timeline && timeline.focusOut()) {
        timeline.scrollToSelection(renderer, contentRect());
        requestUpdate();
      } else if (selectedTab == Tab::Books && booksSelectedIndex > 0) {
        booksSelectedIndex--;
        scrollSelectedBookIntoView(contentRect());
        requestUpdate();
      }
      break;
    case ReaderUtils::SideNavAction::ROTATE:
      ReaderUtils::cycleDisplayOrientation(renderer, 1);
      requestUpdate();
      break;
    case ReaderUtils::SideNavAction::NONE:
      break;
  }

  // Long-press Confirm on a selected book: prompt to delete its whole cache dir
  // (progress + rendered sections + stats). Hold-to-act, matching RecentBooksActivity.
  constexpr unsigned long LONG_PRESS_MS = 1000;
  if (selectedTab == Tab::Books && !bookRows.empty() && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= LONG_PRESS_MS) {
    booksLongPressFired = true;
    promptDeleteBook();
    return;
  }

  // Tap Confirm opens the selected book; the hold above deletes it. Ordering matters: the
  // long-press sets booksLongPressFired, whose guard at the top of loop() swallows the
  // release that follows so the book does not also open.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectedTab == Tab::Books && !bookRows.empty()) openSelectedBook();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }
}

void ReadingStatsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const int pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_READING_STATS));

  const int leftX = metrics.contentSidePadding;
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID) + 2;
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  // Total reading time (dated + undated merged — both X3 RTC and X4 SNTP now
  // date-stamp sessions, so the split is noise).
  const uint32_t totalSeconds = stats ? stats->totalReadingSeconds : 0;
  char totalBuf[32];
  BookReadingStats::formatDuration(totalSeconds, totalBuf, sizeof(totalBuf));
  std::string line1 = std::string(tr(STR_STATS_TIME_READING)) + ": " + totalBuf;
  renderer.drawText(SMALL_FONT_ID, leftX, y, line1.c_str());
  y += lineHeight;

  // Cross-device total (local + last-synced remote counters from KOReader stats
  // sync). Condition must match contentRect()'s summary-line count.
  if (stats && stats->remoteOtherSeconds > 0) {
    char allBuf[32];
    BookReadingStats::formatDuration(stats->displayTotalSeconds(), allBuf, sizeof(allBuf));
    char allLine[96];
    snprintf(allLine, sizeof(allLine), tr(STR_STATS_ALL_DEVICES_FORMAT), allBuf);
    renderer.drawText(SMALL_FONT_ID, leftX, y, allLine);
    y += lineHeight;
  }

  GUI.drawTabBar(renderer, tabBarRect(), buildTabs(), true);

  const Rect content = contentRect();
  if (selectedTab == Tab::Timeline) {
    timeline.renderList(renderer, content);
  } else if (selectedTab == Tab::Books) {
    renderBooksTab(content);
  } else if (displayHist) {
    StatsTimelineView::renderHeatmap(renderer, content, *displayHist);
  } else if (stats) {
    StatsTimelineView::renderHeatmap(renderer, content, stats->history);
  } else {
    StatsTimelineView::renderEmptyState(renderer, content);
  }

  // Confirm hint only on the Books tab: tap opens the book, hold clears its reading data.
  const char* confirmHint = (selectedTab == Tab::Books && !bookRows.empty()) ? tr(STR_OPEN) : "";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmHint, tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void ReadingStatsActivity::renderBooksTab(const Rect& content) const {
  if (bookRows.empty()) {
    StatsTimelineView::renderEmptyState(renderer, content);
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int rowH = bookRowHeight(renderer);
  const int leftX = content.x + metrics.contentSidePadding;
  const int rightEdge = content.x + content.width - metrics.contentSidePadding;

  // Bottom row is reserved for the pinned "sum of books" subtotal; the scrollable
  // list fills the space above it.
  const int footerY = content.y + content.height - rowH;

  int rowY = content.y;
  for (size_t i = static_cast<size_t>(booksScrollOffset); i < bookRows.size() && rowY + rowH <= footerY; ++i) {
    const auto& r = bookRows[i];
    // Selected row drawn inverted (black fill, white text) — long-press Confirm deletes
    // it. Matches the timeline's boxed-cursor cell.
    const bool selected = static_cast<int>(i) == booksSelectedIndex;
    if (selected) {
      renderer.fillRoundedRect(leftX - 4, rowY + 1, (rightEdge - leftX) + 8, rowH - 2, 6, Color::Black);
    }
    char durBuf[32];
    BookReadingStats::formatDuration(r.allDevicesSeconds, durBuf, sizeof(durBuf));
    const int durW = renderer.getTextWidth(UI_10_FONT_ID, durBuf);
    const int titleMaxW = std::max(0, (rightEdge - durW - 12) - leftX);
    const std::string title = renderer.truncatedText(UI_10_FONT_ID, r.title.c_str(), titleMaxW);
    renderer.drawText(UI_10_FONT_ID, leftX, rowY + 5, title.c_str(), !selected);
    renderer.drawText(UI_10_FONT_ID, rightEdge - durW, rowY + 5, durBuf, !selected);
    rowY += rowH;
  }

  // Divider + reconciliation subtotal, pinned at the bottom of the content area.
  renderer.drawLine(content.x + metrics.contentSidePadding, footerY, rightEdge, footerY);
  char sumBuf[32];
  BookReadingStats::formatDuration(booksSumSeconds, sumBuf, sizeof(sumBuf));
  char footer[96];
  snprintf(footer, sizeof(footer), tr(STR_STATS_SUM_OF_BOOKS_FORMAT), sumBuf);
  renderer.drawText(UI_10_FONT_ID, leftX, footerY + 5, footer, true, EpdFontFamily::BOLD);
}
