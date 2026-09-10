#include "QuoteViewerActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "../../BookmarkStore.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// UI_12 (built-in compressed font, always resident — immune to the SD-font
// prewarm wipe that affects the reader page fonts). Roomier than UI_10 for reading.
constexpr int kBodyFontId = UI_12_FONT_ID;
constexpr int kHeaderFontId = UI_12_FONT_ID;
constexpr int kWrapLineCap = 200;  // bounds wrappedText alloc; 512-char preview is far fewer
}  // namespace

QuoteViewerActivity::QuoteViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                         int initialBookmarkIndex)
    : Activity("QuoteViewer", renderer, mappedInput) {
  const auto& bms = BOOKMARKS.getBookmarks();
  quoteIndices_.reserve(bms.size());
  for (size_t i = 0; i < bms.size(); i++) {
    if (bms[i].isQuote()) quoteIndices_.push_back(i);
  }
  // Position on the quote whose absolute index was passed in (fall back to first).
  for (size_t pos = 0; pos < quoteIndices_.size(); pos++) {
    if (quoteIndices_[pos] == static_cast<size_t>(initialBookmarkIndex)) {
      currentPos_ = static_cast<int>(pos);
      break;
    }
  }
}

void QuoteViewerActivity::onEnter() {
  Activity::onEnter();
  actionBar_.layout(renderer, mappedInput.hasTouch(), UI_10_FONT_ID, 3);
  if (quoteIndices_.empty()) {
    // Defensive: caller should only open this on a quote row.
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }
  loadCurrent();
  requestUpdate();
}

void QuoteViewerActivity::loadCurrent() {
  wrappedLines_.clear();
  pageOffset_ = 0;
  previewText_.clear();

  if (currentPos_ < 0 || currentPos_ >= static_cast<int>(quoteIndices_.size())) return;
  const size_t absIdx = quoteIndices_[currentPos_];
  const auto& bms = BOOKMARKS.getBookmarks();
  if (absIdx >= bms.size()) return;

  // Full text from the .qtext sidecar; fall back to the resident snippet teaser for a
  // synced-in quote whose full preview never reached this device.
  if (!BOOKMARKS.readPreviewAt(absIdx, previewText_) || previewText_.empty()) {
    previewText_ = bms[absIdx].snippet[0] != '\0' ? std::string(bms[absIdx].snippet) : std::string();
  }
  const char* text = !previewText_.empty() ? previewText_.c_str() : tr(STR_BOOKMARK_INSTRUCTIONS);

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int sidePad = metrics.contentSidePadding;
  const int contentWidth = renderer.getScreenWidth() - sidePad * 2;
  wrappedLines_ = renderer.wrappedText(kBodyFontId, text, contentWidth, kWrapLineCap);

  lineHeight_ = renderer.getLineHeight(kBodyFontId);
  const int topReserve = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int bottomReserve = metrics.buttonHintsHeight + metrics.verticalSpacing;
  const int bodyHeight = renderer.getScreenHeight() - topReserve - bottomReserve;
  linesPerPage_ = (lineHeight_ > 0) ? std::max(1, bodyHeight / lineHeight_) : 1;
}

void QuoteViewerActivity::loop() {
  if (quoteIndices_.empty()) return;
  using B = MappedInputManager::Button;

  if (mappedInput.wasReleased(B::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  int tapX = 0;
  int tapY = 0;
  const int tappedButton = mappedInput.wasScreenTapped(tapX, tapY) ? actionBar_.hitAt(tapX, tapY) : -1;
  if (tappedButton == 0 || tappedButton == 2) {
    const int count = static_cast<int>(quoteIndices_.size());
    currentPos_ = (currentPos_ + (tappedButton == 0 ? -1 : 1) + count) % count;
    loadCurrent();
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(B::Confirm) || tappedButton == 1) {
    const auto& bms = BOOKMARKS.getBookmarks();
    const size_t absIdx = quoteIndices_[currentPos_];
    if (absIdx < bms.size()) {
      setResult(
          ActivityResult{BookmarkResult{bms[absIdx].spineIndex, bms[absIdx].progress, bms[absIdx].paragraphIndex}});
    }
    finish();
    return;
  }

  const int total = static_cast<int>(quoteIndices_.size());
  if (mappedInput.wasReleased(B::Up)) {
    currentPos_ = (currentPos_ - 1 + total) % total;
    loadCurrent();
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(B::Down)) {
    currentPos_ = (currentPos_ + 1) % total;
    loadCurrent();
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(B::Left)) {
    if (pageOffset_ > 0) {
      pageOffset_ = std::max(0, pageOffset_ - linesPerPage_);
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasReleased(B::Right)) {
    const int maxOffset = std::max(0, static_cast<int>(wrappedLines_.size()) - linesPerPage_);
    if (pageOffset_ < maxOffset) {
      pageOffset_ = std::min(maxOffset, pageOffset_ + linesPerPage_);
      requestUpdate();
    }
    return;
  }
}

void QuoteViewerActivity::render(RenderLock&&) {
  renderer.clearScreen();
  if (quoteIndices_.empty()) {
    renderer.displayBuffer();
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int sidePad = metrics.contentSidePadding;
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto& bms = BOOKMARKS.getBookmarks();
  const size_t absIdx = quoteIndices_[currentPos_];
  const struct Bookmark& bm = bms[absIdx];

  // Header: chapter title (left, bold) + "N / M" quote counter (right). Both are drawn here
  // rather than passed to drawHeader, which only gets the empty title so it paints the band.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "");
  // With the top bar off the band is one title line tall and holds no battery, so the labels
  // centre in it and reclaim the reserve the battery group would have needed.
  const bool compactBand = UITheme::isTopBarHidden();
  constexpr int kBatteryReserve = 90;
  const int batteryReserve = compactBand ? 0 : kBatteryReserve;
  const int headerY = compactBand
                          ? metrics.topPadding + (metrics.headerHeight - renderer.getLineHeight(kHeaderFontId)) / 2
                          : metrics.topPadding + (metrics.headerHeight > 60 ? metrics.batteryBarHeight + 3 : 14);
  const char* chapter = bm.chapterTitle[0] != '\0' ? bm.chapterTitle : tr(STR_BOOKMARKS);

  char counter[24];
  std::snprintf(counter, sizeof(counter), "%d / %d", currentPos_ + 1, static_cast<int>(quoteIndices_.size()));
  const int counterW = renderer.getTextWidth(kHeaderFontId, counter, EpdFontFamily::REGULAR);
  constexpr int kCounterGap = 8;
  const int chapterBudget = std::max(0, pageWidth - sidePad * 2 - batteryReserve - counterW - kCounterGap);
  const std::string chapterTrunc = renderer.truncatedText(kHeaderFontId, chapter, chapterBudget);
  renderer.drawText(kHeaderFontId, sidePad, headerY, chapterTrunc.c_str(), true, EpdFontFamily::BOLD);
  renderer.drawText(kHeaderFontId, pageWidth - sidePad - batteryReserve - counterW, headerY, counter, true,
                    EpdFontFamily::REGULAR);

  // Body: wrapped preview, paginated by pageOffset_.
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int bodyBottomLimit = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int linesAvailable = static_cast<int>(wrappedLines_.size()) - pageOffset_;
  const int linesToDraw = std::min(linesPerPage_, std::max(0, linesAvailable));
  for (int i = 0; i < linesToDraw; i++) {
    if (y + lineHeight_ > bodyBottomLimit) break;
    renderer.drawText(kBodyFontId, sidePad, y, wrappedLines_[pageOffset_ + i].c_str(), true, EpdFontFamily::REGULAR);
    y += lineHeight_;
  }

  const bool hasMoreUp = pageOffset_ > 0;
  const bool hasMoreDown = pageOffset_ + linesPerPage_ < static_cast<int>(wrappedLines_.size());
  const char* leftHint = hasMoreUp ? tr(STR_DIR_UP) : "";
  const char* rightHint = hasMoreDown ? tr(STR_DIR_DOWN) : "";
  if (actionBar_.active()) {
    const char* barLabels[] = {tr(STR_PREVIOUS), tr(STR_OPEN), tr(STR_NEXT)};
    actionBar_.draw(renderer, UI_10_FONT_ID, barLabels, /*primaryIndex=*/1);
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN), leftHint, rightHint);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
