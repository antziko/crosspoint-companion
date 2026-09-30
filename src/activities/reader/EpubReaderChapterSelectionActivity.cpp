#include "EpubReaderChapterSelectionActivity.h"

#include <Arduino.h>  // millis()
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <SdDebugLog.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <string>

#include "MappedInputManager.h"
#include "components/UIScale.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

EpubReaderChapterSelectionActivity::EpubReaderChapterSelectionActivity(GfxRenderer& renderer,
                                                                       MappedInputManager& mappedInput,
                                                                       const std::shared_ptr<Epub>& epub,
                                                                       const int currentSpineIndex)
    : UiListActivity("EpubReaderChapterSelection", renderer, mappedInput),
      epub(epub),
      currentSpineIndex(currentSpineIndex) {}

void EpubReaderChapterSelectionActivity::onEnter() {
  UiListActivity::onEnter();

  // The reader underneath pins its page-render glyph arenas and its book font's kern/ligature
  // tables while this overlay is up; none of it draws a chapter title. releaseCache() frees
  // both (clearCache() keeps the kern tables, and keeps the arenas above 40KB free), and the
  // next page render's PrewarmScope rebuilds them at ordinary page-turn cost. The heap this
  // returns is what lets the list's own rows' fallback glyphs stay resident: over an open
  // book the list otherwise ran at ~20KB free, where the mini and overflow caches together
  // held fewer glyphs than one screen of Han titles, and every step re-read ~36 from SD.
  // Safe without a RenderLock here: onEnter runs after the manager has swapped
  // currentActivity, so the render task can only be painting this screen, which has not
  // drawn anything yet.
  if (auto* fcm = renderer.getFontCacheManager()) {
    const uint32_t freeBefore = esp_get_free_heap_size();
    fcm->releaseCache();
    SdDebugLog::log("CHS", "release free=%u->%u largest=%u", static_cast<unsigned>(freeBefore),
                    static_cast<unsigned>(esp_get_free_heap_size()),
                    static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
  }

  if (!epub) {
    return;
  }

  // Start with the current chapter at the top of the viewport; the first screen build pulls
  // the viewport to it (ListNav follow-on-build) and materializes the row window there
  // (refreshTocWindow in buildScreen).
  int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
  if (tocIndex == -1) {
    tocIndex = 0;
  }
  nav.selected = tocIndex;
}

void EpubReaderChapterSelectionActivity::onExit() {
  // Hand the heap back before the reader repaints. The rows' fallback glyphs sized their
  // cache to the room onEnter's release made (~7KB), and the reader's next page render has to
  // rebuild its own arenas and kern tables on top of whatever is still resident; left in
  // place, that page render dipped to ~9KB free. Runs under the manager's RenderLock.
  if (auto* fcm = renderer.getFontCacheManager()) {
    const uint32_t freeBefore = esp_get_free_heap_size();
    fcm->releaseCache();
    SdDebugLog::log("CHS", "exit release free=%u->%u largest=%u", static_cast<unsigned>(freeBefore),
                    static_cast<unsigned>(esp_get_free_heap_size()),
                    static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
  }
  UiListActivity::onExit();
}

// Materialize the ListItem/label window starting at `start` (clamped). TOC entries are SD LUT
// reads (getTocItem), so this runs only when the viewport leaves the current window. Finishes
// with a batch prewarm of the visible rows' CJK fallback glyphs -- one bounded SD pass per list
// page; repaints inside the window stay RAM-only.
void EpubReaderChapterSelectionActivity::refreshTocWindow(const int start) {
  const int total = listCount();
  int clamped = start;
  if (clamped > total - TOC_WINDOW) clamped = total - TOC_WINDOW;
  if (clamped < 0) clamped = 0;
  if (clamped == windowStart) return;

  windowCount = total - clamped < TOC_WINDOW ? total - clamped : TOC_WINDOW;
  for (int i = 0; i < windowCount; i++) {
    const auto tocItem = epub->getTocItem(clamped + i);
    std::string indent(tocItem.level > 0 ? (tocItem.level - 1) * 2 : 0, ' ');
    windowLabels[i] = indent + tocItem.title;
    fui::ListItem item;
    item.label = windowLabels[i].c_str();
    item.actionValue = static_cast<int16_t>(clamped + i);
    windowItems[i] = item;
  }
  windowStart = clamped;

  // Prewarm only the rows on screen, not the whole window. Over an open book the fallback's
  // mini is capped near 4KB; a full 24-row window of Han titles ran just over it, so the trim
  // dropped a few glyphs that every repaint then re-read from SD (~40 reads, ~110ms per step).
  // visibleRows is the fixed-height estimate, which is never fewer than the rows list() draws.
  // The window is pinned at the end of the TOC, so the viewport can start inside it.
  const int prewarmFirst = std::clamp(nav.top - windowStart, 0, windowCount);
  const int prewarmCount = std::min(windowCount - prewarmFirst, std::max(nav.visibleRows, 1));
  struct PrewarmCtx {
    const std::string* labels;
    int count;
  } prewarmCtx{windowLabels + prewarmFirst, prewarmCount};
  // Drop the previous window's glyph cache before sizing the new one. The mini's budget is
  // derived from free heap, so while the old arena is still allocated it counts against the
  // new screen: a list moving to new titles got 4KB where the first screen got 7KB, and a
  // screen of Han titles (~7.5KB) no longer fit. Freeing first keeps every floor unchanged and
  // lowers the peak, since the two arenas never coexist. Runs inside the screen build, under
  // the render lock and before any row draws, so no glyph pointer is held across it.
  if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseCache();
  const unsigned long tPrewarm = millis();
  renderer.prewarmFallbackText(
      uiScaleSpec().bodyFontId,
      [](const void* ctx, uint32_t i) -> const char* {
        const auto* c = static_cast<const PrewarmCtx*>(ctx);
        return i < static_cast<uint32_t>(c->count) ? c->labels[i].c_str() : nullptr;
      },
      &prewarmCtx, static_cast<uint32_t>(prewarmCount));
  SdDebugLog::log("CHS", "window start=%d count=%d prewarmed=%d prewarm=%lums free=%u largest=%u", windowStart,
                  windowCount, prewarmCount, millis() - tPrewarm, static_cast<unsigned>(esp_get_free_heap_size()),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
}

void EpubReaderChapterSelectionActivity::activateIndex(const int index) {
  if (index < 0 || index >= listCount()) {
    return;
  }
  // The activated row leaves this screen (finish); a lingering flash would gray
  // an unrelated element on the next render.
  app.clearTapFlash();
  nav.selected = index;
  const auto tocItem = epub->getTocItem(index);
  if (tocItem.spineIndex == -1) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
  } else {
    setResult(ChapterResult{tocItem.spineIndex, tocItem.anchor});
    finish();
  }
}

bool EpubReaderChapterSelectionActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return true;
  }

  if (!epub) {
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateIndex(nav.selected);
    return true;
  }

  return false;
}

void EpubReaderChapterSelectionActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  // Content: the safe area minus the header band drawChrome paints the title in.
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                                      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                                      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)),
                                      static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (!epub) {
    return;
  }
  if (listCount() == 0) {
    screen.centeredText(tr(STR_NO_CHAPTERS), screen.theme().smallText);
    return;
  }

  fui::ListProps props;
  props.count = static_cast<uint16_t>(listCount());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  // Same row font as Settings and the reader menu this list is opened from; a chapter title
  // that still overruns wraps onto a second line (list() grows only the rows that need it).
  // maxLines also marks the style explicitly set — an all-default smallText fails
  // textStyleUnset and list() would substitute bodyText back.
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  // Materialize the row window for the FINAL viewport -- syncListViewport has just applied
  // follow/clamping to nav.top -- and hand list() the window with its absolute base index.
  refreshTocWindow(nav.top);
  props.items = windowItems;
  props.itemsWindowFirst = static_cast<uint16_t>(windowStart);
  screen.list(props);
}

void EpubReaderChapterSelectionActivity::drawChrome() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  GUI.drawHeader(renderer, Rect{safe.x, safe.y + metrics.topPadding, safe.width, metrics.headerHeight},
                 tr(STR_SELECT_CHAPTER));
}
