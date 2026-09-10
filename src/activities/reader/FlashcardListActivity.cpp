#include "FlashcardListActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "DictionaryDefinitionActivity.h"
#include "MappedInputManager.h"
#include "ReadingTimeHistory.h"  // readingHistoryDayIndex
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/Dictionary.h"
#include "util/DictionaryActivityUtils.h"
#include "util/FlashcardCardFace.h"

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
  clockOk = halClock.isAvailable() &&
            halClock.getLocalDateTime(SETTINGS.clockUtcOffsetQ, dayOfWeek, day, month, year, hour, minute);
  today = clockOk ? readingHistoryDayIndex(year, month, day) : 0;

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
  phase = Phase::Detail;
  requestUpdate();
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

  // ----- Detail phase: Confirm looks up the live definition, Back returns. -----
  if (phase == Phase::Detail) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
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
      FlashcardDeck::removeAt(cachePath, fileIndexOf(selectedIndex));
      refreshCount();
      deleteConfirmMode = false;
      if (selectedIndex >= totalCount) {
        selectedIndex = std::max(0, totalCount - 1);
      }
      requestUpdate();
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
  const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false);

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

  // A tap on a row selects and activates it in one go, like the FUI list screens.
  // A tap is never a hold, so it cannot reach the long-press branch above.
  int tapX = 0;
  int tapY = 0;
  const int tappedRow = mappedInput.wasScreenTapped(tapX, tapY) ? listTouch_.indexAt(renderer, tapX, tapY) : -1;
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

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  const int pageItems = std::max(1, UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false));
  const int curPage = selectedIndex / pageItems + 1;
  const int totalPages = std::max(1, (totalCount + pageItems - 1) / pageItems);

  char titleBuf[64];
  snprintf(titleBuf, sizeof(titleBuf), tr(STR_LOOKUP_HIST_HEADER_FORMAT), tr(STR_FLASHCARDS_LIST),
           static_cast<int>(totalCount), curPage, totalPages);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, titleBuf);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  if (totalCount == 0) {
    const int midY = contentTop + (pageHeight - contentTop - metrics.buttonHintsHeight) / 2;
    renderer.drawCenteredText(UI_10_FONT_ID, midY, tr(STR_FLASHCARDS_EMPTY));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    displayList();
    return;
  }

  const int contentHeight = pageItems * metrics.listRowHeight;
  const uint32_t todayLocal = today;
  listTouch_.record(Rect{0, contentTop, pageWidth, contentHeight}, totalCount, selectedIndex);
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, totalCount, selectedIndex,
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

  displayList();
}

void FlashcardListActivity::renderDetail() {
  renderer.clearScreen();
  if (controller.render()) return;

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FLASHCARDS_LIST));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = pageHeight - metrics.buttonHintsHeight;

  // Shared card face: bold word + excerpt (word underlined in context) + chapter.
  FlashcardCardFace::render(renderer, contentTop, contentBottom, pageWidth, detail.word, detail.excerpt, detail.chapter,
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

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_FLASHCARD_FLIP), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  displayList();
}
