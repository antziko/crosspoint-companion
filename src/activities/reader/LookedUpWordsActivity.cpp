#include "LookedUpWordsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "DictionaryDefinitionActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/Dictionary.h"
#include "util/DictionaryActivityUtils.h"
#include "util/LookupHistory.h"

const char* LookedUpWordsActivity::glyphFor(LookupHistory::Status s) {
  switch (s) {
    case LookupHistory::Status::Direct:
      return "\xe2\x88\x9a";  // √ U+221A
    case LookupHistory::Status::Stem:
      return "~";
    case LookupHistory::Status::AltForm:
      return "~";
    case LookupHistory::Status::Suggestion:
      return "?";
    case LookupHistory::Status::NotFound:
      return "\xc3\x97";  // × U+00D7
    default:
      return "?";
  }
}

void LookedUpWordsActivity::onEnter() {
  Activity::onEnter();
  refreshCount();
  requestUpdate();
}

void LookedUpWordsActivity::refreshCount() {
  totalCount = LookupHistory::count(cachePath);
  windowStart = -1;  // invalidate cached page
  windowLen = 0;
}

const LookupHistory::Entry* LookedUpWordsActivity::entryAt(int uiIndex) {
  if (uiIndex < 0 || uiIndex >= totalCount) return nullptr;
  if (windowStart < 0 || uiIndex < windowStart || uiIndex >= windowStart + windowLen) {
    windowStart = uiIndex;  // drawList enters each page at its first index
    windowLen = LookupHistory::loadWindow(cachePath, windowStart, WINDOW_CAP, window);
  }
  const int off = uiIndex - windowStart;
  if (off < 0 || off >= windowLen) return nullptr;
  return &window[off];
}

void LookedUpWordsActivity::onExit() {
  controller.onExit();
  Activity::onExit();
}

void LookedUpWordsActivity::loop() {
  if (controller.isActive()) {
    switch (controller.handleInput()) {
      case DictionaryLookupController::LookupEvent::FoundDefinition: {
        startActivityForResult(std::make_unique<DictionaryDefinitionActivity>(
                                   renderer, mappedInput, controller.getFoundWord(), controller.getFoundLocation(),
                                   true, cachePath, controller.getRecordHistory(), controller.getLookupWord(),
                                   DictionaryLookupController::toHistStatus(controller.getFoundStatus())),
                               [this](const ActivityResult& result) {
                                 refreshCount();
                                 if (!result.isCancelled) {
                                   setResult(ActivityResult{});
                                   finish();
                                 } else {
                                   requestUpdate();
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
      LookupHistory::removeAt(cachePath, fileIndexOf(selectedIndex));
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

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (const auto* e = entryAt(selectedIndex)) controller.startLookup(e->word);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    DictUtils::cancelAndFinish(*this);
    return;
  }
}

void LookedUpWordsActivity::displayList() {
  // FAST_REFRESH is a differential (turbo) waveform that resolves cleanly only in
  // the panel's native portrait scan direction; in landscape repeated up/down
  // accumulates DC bias into progressive whitening. Rather than pay the slow
  // HALF_REFRESH on every move, keep FAST and scrub with one HALF every N moves
  // (N = the user's Refresh Frequency) — the same fast/periodic-clean cadence the
  // reader uses for page turns. Portrait stays pure FAST (no washout there).
  const auto o = renderer.getOrientation();
  const bool landscape =
      o == GfxRenderer::Orientation::LandscapeClockwise || o == GfxRenderer::Orientation::LandscapeCounterClockwise;
  // Entry render scrubs (counter starts at 0) for a clean baseline, then FAST until
  // the next periodic HALF. On X4, FAST re-syncs the reference plane every frame so
  // it stays clean regardless; the scrub matters for the X3 turbo path.
  HalDisplay::RefreshMode mode = HalDisplay::FAST_REFRESH;
  if (landscape && --pagesUntilFullRefresh <= 0) {
    mode = HalDisplay::HALF_REFRESH;
    pagesUntilFullRefresh = std::max(1, SETTINGS.getRefreshFrequency());
  }
  renderer.displayBuffer(mode);
}

void LookedUpWordsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  if (controller.render()) return;

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Same row count the nav uses (orientation-aware: drops the bottom button-hints
  // reservation in landscape). Drives both the page indicator and the list height,
  // so render and navigation always agree on rows-per-page.
  const int pageItems = std::max(1, UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false));
  const int curPage = selectedIndex / pageItems + 1;
  const int totalPages = std::max(1, (totalCount + pageItems - 1) / pageItems);

  char titleBuf[64];
  snprintf(titleBuf, sizeof(titleBuf), tr(STR_LOOKUP_HIST_HEADER_FORMAT), tr(STR_LOOKUP_HISTORY),
           static_cast<int>(totalCount), curPage, totalPages);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, titleBuf);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  if (totalCount == 0) {
    const int midY = contentTop + (pageHeight - contentTop - metrics.buttonHintsHeight) / 2;
    renderer.drawCenteredText(UI_10_FONT_ID, midY, tr(STR_LOOKUP_HISTORY_EMPTY));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    displayList();
    return;
  }

  // Size the list to exactly `pageItems` rows so drawList renders every row that
  // fits (the old manual height under-filled by ~1 row, worse in landscape where
  // the bottom button-hints band doesn't apply).
  const int contentHeight = pageItems * metrics.listRowHeight;

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, totalCount, selectedIndex,
      [this](int i) {
        const auto* e = entryAt(i);
        if (!e) return std::string();
        return std::string(glyphFor(e->status)) + " " + e->word;
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
