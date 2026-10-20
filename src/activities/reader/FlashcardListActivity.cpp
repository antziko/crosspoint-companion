#include "FlashcardListActivity.h"

#include <Epub.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <string_view>

#include "CrossPointSettings.h"
#include "DictionaryDefinitionActivity.h"
#include "MappedInputManager.h"
#include "ReadingTimeHistory.h"  // readingHistoryDayIndex
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/Dictionary.h"
#include "util/DictionaryActivityUtils.h"
#include "util/FlashcardCardFace.h"
#include "util/ListSwipeScroll.h"
#include "util/LookupMarks.h"

const char* FlashcardListActivity::glyphFor(const FlashcardDeck::Entry& e, uint32_t today) {
  if (FlashcardDeck::isMastered(e.box)) return "*";                            // graduated
  if (FlashcardDeck::isSuspended(e.box)) return "~";                           // set aside
  if (e.dueDay == 0) return "+";                                               // new (never scheduled)
  if (today != 0 && FlashcardDeck::isDue(e.box, e.dueDay, today)) return "!";  // due now
  // Scheduled in a Leitner box: show the box digit (0..5).
  static char buf[2] = {0, 0};
  buf[0] = static_cast<char>('0' + (e.box <= 5 ? e.box : 0));
  return buf;
}

void FlashcardListActivity::onEnter() {
  Activity::onEnter();

  // The detail card face renders through getDefinitionFontId() (FlashcardCardFace.cpp:198),
  // so the dictionary's size needs the same font residency the definition viewer arranges.
  DictUtils::ensureDefinitionFontResident(renderer);

  // Resolve today in the user's local calendar day (same convention as reading
  // stats / FlashcardReviewActivity); 0 when the RTC is unavailable, which makes
  // the due/box glyphs fall back to box digits.
  uint8_t dayOfWeek = 0, day = 0, month = 0, hour = 0, minute = 0;
  uint16_t year = 0;
  clockOk = halClock.isAvailable() && halClock.getLocalDateTime(dayOfWeek, day, month, year, hour, minute);
  today = clockOk ? readingHistoryDayIndex(year, month, day) : 0;
  tapBar = mappedInput.hasTouch();

  refreshCount();
  requestUpdate();
}

void FlashcardListActivity::refreshCount() {
  totalCount = FlashcardDeck::count(cachePath);
  windowStart = -1;  // invalidate cached page
  windowLen = 0;
}

const FlashcardDeck::Entry* FlashcardListActivity::entryAt(int uiIndex) {
  if (uiIndex < 0 || uiIndex >= totalCount) return nullptr;
  if (windowStart < 0 || uiIndex < windowStart || uiIndex >= windowStart + windowLen) {
    windowStart = uiIndex;  // drawList enters each page at its first index
    windowLen = FlashcardDeck::loadWindow(cachePath, windowStart, WINDOW_CAP, window, /*wordsOnly=*/true);
  }
  const int off = uiIndex - windowStart;
  if (off < 0 || off >= windowLen) return nullptr;
  return &window[off];
}

void FlashcardListActivity::openDetail() {
  if (selectedIndex < 0 || selectedIndex >= totalCount) return;
  // Load the selected card in full (excerpt + chapter) for the detail card face.
  if (FlashcardDeck::loadWindow(cachePath, selectedIndex, 1, &detail, /*wordsOnly=*/false) < 1) return;
  resolveDetailPosition();
  phase = Phase::Detail;
  requestUpdate();
}

void FlashcardListActivity::flipDetail() {
  // Same rule as the review screen's flip: look the word up in the dictionary the card was
  // saved from, silently falling back to the active one when it is not installed. The two
  // screens must agree — opening the same card from the list and from a review should not
  // produce different definitions. Safe here for the same reason: loop() is the UI task and
  // the controller has nothing in flight (Dictionary.h:94-99).
  // Re-read first, for the same reason the review screen does: `detail` is a resident copy
  // and the definition screen may have written a new dictionary onto this card since.
  uint32_t recorded = 0;
  if (FlashcardDeck::cardDict(cachePath, detail.word, recorded)) detail.dictHash = recorded;
  DictUtils::applyCardDict(detail.dictHash);
  controller.startLookup(detail.word);
}

void FlashcardListActivity::jumpToDetailPage() {
  if (detailSpine < 0) return;
  FlashcardJumpResult jump;
  jump.spineIndex = static_cast<uint16_t>(detailSpine);
  jump.progress = detailProgress;
  // The word as the page prints it ("donned" for a card filed under "don"), as the underline.
  int surfaceLen = 0;
  const char* surface =
      FlashcardDeck::findSurfaceForm(detail.word.c_str(), static_cast<int>(detail.word.size()), detail.excerpt.c_str(),
                                     static_cast<int>(detail.excerpt.size()), &surfaceLen);
  if (!surface || surfaceLen <= 0) {
    surface = detail.word.c_str();
    surfaceLen = static_cast<int>(detail.word.size());
  }
  LookupMarks::Mark mark;
  if (LookupMarks::markFor(surface, surfaceLen, mark)) {
    jump.hasWord = true;
    jump.wordHash = mark.wordHash;
    jump.headHash = mark.headHash;
    jump.byteLen = mark.byteLen;
    jump.word = detail.word;
  }
  setResult(ActivityResult{jump});
  finish();
}

void FlashcardListActivity::deleteSelected() {
  FlashcardDeck::removeAt(cachePath, fileIndexOf(selectedIndex));
  refreshCount();
  deleteConfirmMode = false;
  if (selectedIndex >= totalCount) {
    selectedIndex = std::max(0, totalCount - 1);
  }
  requestUpdate();
}

// The card records its chapter by TOC title plus a "page/pageCount" token from the layout it
// was saved under, so the jump lands on the first spine carrying that title at the same
// fraction of the chapter. The fraction aims at the middle of the card's page slice: its
// pageCount may have been a mid-build estimate, and the start of the slice then falls on
// the page before the word.
void FlashcardListActivity::resolveDetailPosition() {
  detailSpine = -1;
  if (!book) return;
  int titleLen = static_cast<int>(detail.chapter.size());
  int page = 0;
  int pageCount = 0;
  FlashcardDeck::parseChapterPage(detail.chapter.c_str(), static_cast<int>(detail.chapter.size()), &titleLen, &page,
                                  &pageCount);
  const bool hasPage = page >= 1 && pageCount >= 1;
  const std::string_view field(detail.chapter.data(), detail.chapter.size());
  const std::string_view title(detail.chapter.data(), static_cast<size_t>(titleLen));
  const int tocCount = book->getTocItemsCount();
  for (int i = 0; i < tocCount; i++) {
    const std::string toc = book->getTocItem(i).title;
    bool match = toc == title;
    // A card with no page saved: an older one, or one whose long title the chapter cap cut,
    // with or without a stub of the page token after it ("...Rules 9"). Matched by prefix.
    if (!match && !hasPage && toc.size() >= kMinTitlePrefix && field.size() >= kMinTitlePrefix) {
      match = (field.size() > toc.size() && field.substr(0, toc.size()) == toc && field[toc.size()] == ' ') ||
              (toc.size() > field.size() && std::string_view(toc).substr(0, field.size()) == field);
    }
    if (!match) continue;
    detailSpine = book->getSpineIndexForTocIndex(i);
    // No page saved: the chapter start, from where the reader searches the whole chapter.
    detailProgress =
        hasPage ? (static_cast<float>(std::min(page, pageCount)) - 0.5f) / static_cast<float>(pageCount) : 0.0f;
    return;
  }
  LOG_DBG("FCL", "'%s': chapter \"%s\" not in this book's TOC", detail.word.c_str(), detail.chapter.c_str());
}

void FlashcardListActivity::onExit() {
  controller.onExit();  // stops+joins the lookup task first: nothing may free fonts under it
  DictUtils::releaseDefinitionFont(renderer);
  Activity::onExit();
}

void FlashcardListActivity::onResume() {
  // A definition opened from the detail face releases the extra size on its way out, so the
  // card would draw at the reader's size again. Re-take it (no-op if still resident).
  DictUtils::ensureDefinitionFontResident(renderer);
}

void FlashcardListActivity::loop() {
  if (controller.isActive()) {
    switch (controller.handleInput()) {
      case DictionaryLookupController::LookupEvent::FoundDefinition: {
        // Nothrow: ~4.8 KB pushed straight after the lookup's glyph prewarm. See the note at
        // the matching site in DictionaryWordSelectActivity — a bare new aborts the device.
        auto definition = makeUniqueNoThrow<DictionaryDefinitionActivity>(
            renderer, mappedInput, controller.getFoundWord(), controller.getFoundLocation(), true, cachePath,
            controller.getRecordHistory(), controller.getLookupWord(),
            DictionaryLookupController::toHistStatus(controller.getFoundStatus()));
        if (!definition) {
          LOG_ERR("FCL", "OOM: DictionaryDefinitionActivity");
          requestUpdate();
          break;
        }
        startActivityForResult(std::move(definition), [this](const ActivityResult& result) {
          refreshCount();
          if (!result.isCancelled) {
            setResult(ActivityResult{});
            finish();
          } else {
            requestUpdate();  // back to the detail view
          }
        });
        break;
      }
      case DictionaryLookupController::LookupEvent::NotFoundDismissedBack:
        refreshCount();
        requestUpdate();
        break;
      case DictionaryLookupController::LookupEvent::NotFoundDismissedDone:
        setResult(ActivityResult{});
        finish();
        break;
      case DictionaryLookupController::LookupEvent::Cancelled:
        requestUpdate();
        break;
      default:
        break;
    }
    return;
  }

  // The on-screen buttons first: on a touch-only board they are the only route to these actions.
  if (handleTapBar()) return;

  // ----- Detail phase: Confirm looks up the live definition, Back returns. -----
  if (phase == Phase::Detail) {
    int tapX = 0;
    int tapY = 0;
    // A tap anywhere on the card flips it, like Confirm.
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(tapX, tapY)) {
      flipDetail();
      return;
    }
    // Right on the front row; Down too, because the X4 Pro has only the side pair.
    if (detailSpine >= 0 && (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
                             mappedInput.wasReleased(MappedInputManager::Button::Down))) {
      jumpToDetailPage();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      phase = Phase::List;
      requestUpdate();
      return;
    }
    return;
  }

  // ----- List phase -----
  if (totalCount == 0) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      DictUtils::cancelAndFinish(*this);
    }
    return;
  }

  // Long press Confirm: enter delete-confirm mode (fire at threshold).
  if (!deleteConfirmMode && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= Dictionary::LONG_PRESS_MS) {
    deleteConfirmMode = true;
    confirmReleaseConsumed = true;
    requestUpdate();
    return;
  }

  if (deleteConfirmMode) {
    // Consume the Confirm release that follows the threshold-fire.
    if (confirmReleaseConsumed && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      confirmReleaseConsumed = false;
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      deleteSelected();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      deleteConfirmMode = false;
      requestUpdate();
      return;
    }
    return;
  }

  const int totalItems = totalCount;
  const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false, tapBarReserve());

  buttonNavigator.onNextRelease([this, totalItems] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, totalItems);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this, totalItems] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, totalItems);
    requestUpdate();
  });
  buttonNavigator.onNextContinuous([this, totalItems, pageItems] {
    selectedIndex = ButtonNavigator::nextPageIndex(selectedIndex, totalItems, pageItems);
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this, totalItems, pageItems] {
    selectedIndex = ButtonNavigator::previousPageIndex(selectedIndex, totalItems, pageItems);
    requestUpdate();
  });
  // A swipe pages like a held side key, wrapping at both ends.
  if (const int step = listSwipeStep(mappedInput)) {
    selectedIndex = step > 0 ? ButtonNavigator::nextPageIndex(selectedIndex, totalItems, pageItems)
                             : ButtonNavigator::previousPageIndex(selectedIndex, totalItems, pageItems);
    requestUpdate();
  }

  // A tap on a row selects and activates it in one go, like the FUI list screens.
  // A tap is never a hold, so it cannot reach the long-press branch above.
  int tapX = 0;
  int tapY = 0;
  const int tappedRow = mappedInput.wasScreenTapped(tapX, tapY) ? listTouch_.touchRow(renderer, tapX, tapY) : -1;
  if (tappedRow >= 0) selectedIndex = tappedRow;

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || tappedRow >= 0) {
    openDetail();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    DictUtils::cancelAndFinish(*this);
    return;
  }
}

void FlashcardListActivity::displayList() {
  // Same fast/periodic-clean refresh cadence as LookedUpWordsActivity: FAST in
  // portrait; FAST + one HALF scrub every N moves in landscape to clear the
  // differential-waveform ghosting that accumulates with repeated up/down.
  const auto o = renderer.getOrientation();
  const bool landscape =
      o == GfxRenderer::Orientation::LandscapeClockwise || o == GfxRenderer::Orientation::LandscapeCounterClockwise;
  HalDisplay::RefreshMode mode = HalDisplay::FAST_REFRESH;
  if (landscape && --pagesUntilFullRefresh <= 0) {
    mode = HalDisplay::HALF_REFRESH;
    pagesUntilFullRefresh = std::max(1, SETTINGS.getRefreshFrequency());
  }
  renderer.displayBuffer(mode);
}

void FlashcardListActivity::render(RenderLock&&) {
  if (phase == Phase::Detail) {
    renderDetail();
  } else {
    renderList();
  }
}

void FlashcardListActivity::renderList() {
  renderer.clearScreen();
  if (controller.render()) return;

  const auto& metrics = UITheme::getInstance().getMetrics();

  const int pageItems =
      std::max(1, UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false, tapBarReserve()));
  const int curPage = selectedIndex / pageItems + 1;
  const int totalPages = std::max(1, (totalCount + pageItems - 1) / pageItems);

  char titleBuf[64];
  snprintf(titleBuf, sizeof(titleBuf), tr(STR_LOOKUP_HIST_HEADER_FORMAT), tr(STR_FLASHCARDS_LIST),
           static_cast<int>(totalCount), curPage, totalPages);
  GUI.drawHeader(renderer, UITheme::getInstance().getSafeHeaderRect(renderer), titleBuf);

  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  const int contentTop = safe.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  if (totalCount == 0) {
    const int midY = contentTop + (safe.y + safe.height - contentTop) / 2;
    renderer.drawCenteredText(UI_10_FONT_ID, midY, tr(STR_FLASHCARDS_EMPTY));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    drawTapBar();
    displayList();
    return;
  }

  const int contentHeight = pageItems * metrics.listRowHeight;
  const uint32_t todayLocal = today;
  listTouch_.record(Rect{safe.x, contentTop, safe.width, contentHeight}, totalCount, selectedIndex);
  GUI.drawList(
      renderer, Rect{safe.x, contentTop, safe.width, contentHeight}, totalCount, selectedIndex,
      [this, todayLocal](int i) {
        const auto* e = entryAt(i);
        if (!e) return std::string();
        std::string row = std::string(glyphFor(*e, todayLocal)) + " " + e->word;
        if (e->count > 1) {
          char tag[12];
          snprintf(tag, sizeof(tag), "  x%lu", static_cast<unsigned long>(e->count));
          row += tag;
        }
        return row;
      },
      nullptr, nullptr, nullptr, false);

  if (deleteConfirmMode) {
    char buf[128];
    const auto* sel = entryAt(selectedIndex);
    snprintf(buf, sizeof(buf), "%s: %s?", tr(STR_DELETE), sel ? sel->word.c_str() : "");
    GUI.drawPopup(renderer, buf);
    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_DELETE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  drawTapBar();

  displayList();
}

void FlashcardListActivity::renderDetail() {
  renderer.clearScreen();
  if (controller.render()) return;

  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, UITheme::getInstance().getSafeHeaderRect(renderer), tr(STR_FLASHCARDS_LIST));

  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  const int contentTop = safe.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = safe.y + safe.height - tapBarReserve();

  // Shared card face: bold word + excerpt (word underlined in context) + chapter.
  FlashcardCardFace::render(renderer, contentTop, contentBottom, safe.x, safe.width, detail.word, detail.excerpt,
                            detail.chapter,
                            /*showWord=*/true, detail.count);

  // Leitner status line, in the gap between the word header and the excerpt.
  char statusBuf[64];
  if (FlashcardDeck::isMastered(detail.box)) {
    snprintf(statusBuf, sizeof(statusBuf), "%s", tr(STR_FLASHCARD_SUMMARY_MASTERED));
  } else if (FlashcardDeck::isSuspended(detail.box)) {
    snprintf(statusBuf, sizeof(statusBuf), "%s", tr(STR_FLASHCARD_SUMMARY_SUSPENDED));
  } else if (detail.box == 0 && detail.dueDay == 0) {
    snprintf(statusBuf, sizeof(statusBuf), "%s", tr(STR_FLASHCARD_STAT_NEW));
  } else if (clockOk && FlashcardDeck::isDue(detail.box, detail.dueDay, today)) {
    snprintf(statusBuf, sizeof(statusBuf), "%s", tr(STR_FLASHCARD_STAT_DUE_SHORT));
  } else if (clockOk && detail.dueDay > today) {
    snprintf(statusBuf, sizeof(statusBuf), tr(STR_FLASHCARD_LIST_SCHEDULED), detail.box,
             static_cast<int>(detail.dueDay - today));
  } else {
    snprintf(statusBuf, sizeof(statusBuf), tr(STR_FLASHCARD_LIST_BOX), detail.box);
  }
  // Box/due status sits at the very top of the content area, above the bold word
  // header (which the card face draws at contentTop + listRowHeight).
  renderer.drawCenteredText(UI_10_FONT_ID, contentTop, statusBuf, true, EpdFontFamily::ITALIC);

  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), tr(STR_FLASHCARD_FLIP), "", detailSpine >= 0 ? tr(STR_GO_TO_PAGE) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  drawTapBar();

  displayList();
}

int FlashcardListActivity::buildTapActions(Tap* actions, const char** labels) const {
  if (!tapBar) return 0;
  int count = 0;
  const auto add = [&](const Tap action, const char* label) {
    if (count >= MAX_TAP_ACTIONS) return;
    actions[count] = action;
    labels[count] = label;
    ++count;
  };
  // Hint-strip order. Select needs no button in the list: a tap on a row opens it.
  if (phase == Phase::Detail) {
    add(Tap::Back, tr(STR_BACK));
    add(Tap::Flip, tr(STR_FLASHCARD_FLIP));
    if (detailSpine >= 0) add(Tap::GoToPage, tr(STR_GO_TO_PAGE));
    // Long-press Confirm on the list deletes; a finger has no Confirm to hold.
    add(Tap::Delete, tr(STR_DELETE));
  } else if (deleteConfirmMode) {
    add(Tap::Cancel, tr(STR_CANCEL));
    add(Tap::ConfirmDelete, tr(STR_DELETE));
  } else {
    add(Tap::Back, tr(STR_BACK));
  }
  return count;
}

int FlashcardListActivity::tapBarReserve() const {
  if (!tapBar) return 0;
  return renderer.getLineHeight(UI_10_FONT_ID) + TAP_BUTTON_PADDING_Y * 2 +
         UITheme::getInstance().getMetrics().verticalSpacing;
}

Rect FlashcardListActivity::tapBarRect() const {
  if (!tapBar) return Rect{};
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int height = renderer.getLineHeight(UI_10_FONT_ID) + TAP_BUTTON_PADDING_Y * 2;
  const int inset = metrics.contentSidePadding;
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int y = safe.y + safe.height - metrics.verticalSpacing - height;
  return Rect{safe.x + inset, y, safe.width - inset * 2, height};
}

void FlashcardListActivity::drawTapBar() {
  Tap actions[MAX_TAP_ACTIONS];
  const char* labels[MAX_TAP_ACTIONS];
  const int count = buildTapActions(actions, labels);
  if (count <= 0) return;
  const Rect bar = tapBarRect();
  const int step = bar.width / count;
  const int width = step - TAP_BUTTON_GAP;
  const int radius = UITheme::getInstance().getMetrics().controlRadius;
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  for (int i = 0; i < count; ++i) {
    const int x = bar.x + step * i;
    renderer.fillRect(x, bar.y, width, bar.height, false);
    if (radius > 0) {
      renderer.drawRoundedRect(x, bar.y, width, bar.height, 1, radius, true);
    } else {
      renderer.drawRect(x, bar.y, width, bar.height);
    }
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, labels[i]);
    renderer.drawText(UI_10_FONT_ID, x + (width - textWidth) / 2, bar.y + (bar.height - lineHeight) / 2, labels[i],
                      true);
  }
}

bool FlashcardListActivity::handleTapBar() {
  Tap actions[MAX_TAP_ACTIONS];
  const char* labels[MAX_TAP_ACTIONS];
  const int count = buildTapActions(actions, labels);
  if (count <= 0) return false;
  const Rect bar = tapBarRect();
  const int step = bar.width / count;
  int column = 0;
  // colWidth is the drawn width, not the step: a tap in the gap between two buttons does nothing.
  if (mappedInput.colTouch(column, bar.x, step, count, bar.y, bar.y + bar.height, step - TAP_BUTTON_GAP) !=
      MappedInputManager::RowTouch::Tap) {
    return false;
  }
  if (column < 0 || column >= count) return false;
  runTapAction(actions[column]);
  return true;
}

void FlashcardListActivity::runTapAction(const Tap action) {
  switch (action) {
    case Tap::Back:
      if (phase == Phase::Detail) {
        phase = Phase::List;
        requestUpdate();
      } else {
        DictUtils::cancelAndFinish(*this);
      }
      break;
    case Tap::Flip:
      flipDetail();
      break;
    case Tap::GoToPage:
      jumpToDetailPage();
      break;
    case Tap::Delete:
      // The detail card is the selected row, so the list's confirm prompt deletes it.
      phase = Phase::List;
      deleteConfirmMode = true;
      confirmReleaseConsumed = false;
      requestUpdate();
      break;
    case Tap::Cancel:
      deleteConfirmMode = false;
      requestUpdate();
      break;
    case Tap::ConfirmDelete:
      deleteSelected();
      break;
  }
}
