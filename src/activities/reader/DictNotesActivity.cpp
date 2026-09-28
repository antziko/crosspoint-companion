#include "DictNotesActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>

#include "CrossPointSettings.h"
#include "DictionaryDefinitionActivity.h"
#include "MappedInputManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/Dictionary.h"
#include "util/DictionaryActivityUtils.h"
#include "util/DictionaryRegistry.h"

namespace {
// Bounds the wrap in reader mode. A note is capped at DictNotes::TEXT_MAX bytes on disk, so it
// can never reach this; the cap is what keeps a corrupt line from allocating without limit.
constexpr int kWrapLineCap = 64;
constexpr int kBodyFontId = UI_12_FONT_ID;
}  // namespace

void DictNotesActivity::onEnter() {
  Activity::onEnter();
  loadDictionaries();

  if (dicts_.size() == 1) {
    // One dictionary has notes: a picker with a single row would be a press that asks nothing.
    // Back from the list then leaves the screen, so the way out retraces the way in.
    pickerSkipped_ = true;
    enterDictionary(0);
  } else {
    mode_ = Mode::Dictionaries;
    // Open on the caller's dictionary when it has notes -- from the reader that is the book's,
    // so the common case is still one press away.
    for (size_t i = 0; i < dicts_.size(); i++) {
      if (dicts_[i].hash == preferredHash_) dictIndex_ = static_cast<int>(i);
    }
  }
  requestUpdate();
}

void DictNotesActivity::onExit() {
  controller_.onExit();
  // The definition viewer no longer releases the definition-size font itself; the host that
  // outlives it does, so the reader underneath never carries it. Same pattern as the flashcard
  // screens and the history list.
  DictUtils::releaseDefinitionFont(renderer);
  Activity::onExit();
}

void DictNotesActivity::loadDictionaries() {
  dicts_.clear();

  // 24 summaries is 144 bytes; the names below are what actually costs, and there is one per
  // dictionary that HAS notes, not per dictionary installed.
  auto summaries = makeUniqueNoThrow<DictNotes::DictSummary[]>(DictNotes::MAX_DICTS);
  if (!summaries) {
    LOG_ERR("DNOTE", "OOM: dictionary summaries");
    return;
  }
  const int n = DictNotes::listDictionaries(summaries.get(), DictNotes::MAX_DICTS);
  if (n <= 0) return;
  if (!reserveNoThrow(dicts_, static_cast<size_t>(n))) {
    LOG_ERR("DNOTE", "OOM: %d dictionary rows", n);
    return;
  }

  for (int i = 0; i < n; i++) {
    DictRow row;
    row.hash = summaries[i].dictHash;
    row.count = summaries[i].count;
    const int regIdx = dictionaryRegistry.indexOfHash(row.hash);
    if (regIdx >= 0) {
      row.name = dictionaryRegistry.getEntries()[regIdx].name;
    } else {
      // Not installed here (deleted, renamed, or the card came from another device). Listed
      // anyway, named by its hash, so its notes stay readable and deletable rather than becoming
      // unreachable data on the card.
      char hex[16];
      snprintf(hex, sizeof(hex), "%08lx", static_cast<unsigned long>(row.hash));
      row.name = hex;
    }
    dicts_.push_back(std::move(row));
  }

  // By name, case-insensitively -- the order the dictionary registry itself sorts in, so the two
  // lists cannot disagree about where a dictionary sits.
  std::sort(dicts_.begin(), dicts_.end(),
            [](const DictRow& a, const DictRow& b) { return strcasecmp(a.name.c_str(), b.name.c_str()) < 0; });

  if (dictIndex_ >= static_cast<int>(dicts_.size())) dictIndex_ = static_cast<int>(dicts_.size()) - 1;
  if (dictIndex_ < 0) dictIndex_ = 0;
}

void DictNotesActivity::enterDictionary(const int index) {
  if (index < 0 || index >= static_cast<int>(dicts_.size())) return;
  dictIndex_ = index;
  dictHash_ = dicts_[index].hash;
  dictName_ = dicts_[index].name;
  selectedIndex = 0;
  mode_ = Mode::List;
  listTouch_.clear();  // the picker's rows must not take taps meant for the notes
  refreshCount();
  requestUpdate();
}

void DictNotesActivity::goBack() {
  switch (mode_) {
    case Mode::Note:
      mode_ = Mode::List;
      wrappedLines_.clear();  // the list does not need it, and this screen sits on the reader
      wrappedLines_.shrink_to_fit();
      listTouch_.clear();
      requestUpdate();
      return;
    case Mode::List:
      if (pickerSkipped_) {
        DictUtils::cancelAndFinish(*this);
        return;
      }
      // A delete may have emptied this dictionary, which drops it from the picker entirely.
      loadDictionaries();
      if (dicts_.empty()) {
        DictUtils::cancelAndFinish(*this);
        return;
      }
      mode_ = Mode::Dictionaries;
      listTouch_.clear();
      requestUpdate();
      return;
    case Mode::Dictionaries:
      DictUtils::cancelAndFinish(*this);
      return;
  }
}

void DictNotesActivity::refreshCount() {
  totalCount = DictNotes::count(dictHash_);
  windowStart = -1;  // invalidate cached page
  windowLen = 0;
  if (selectedIndex >= totalCount) selectedIndex = std::max(0, totalCount - 1);
}

const DictNotes::Note* DictNotesActivity::entryAt(const int uiIndex) {
  if (uiIndex < 0 || uiIndex >= totalCount) return nullptr;
  if (windowStart < 0 || uiIndex < windowStart || uiIndex >= windowStart + windowLen) {
    windowStart = uiIndex;  // drawList enters each page at its first index
    windowLen = DictNotes::loadWindow(dictHash_, windowStart, WINDOW_CAP, window);
    // Rows show a teaser; the reader re-reads the note it opens. Trimmed on a UTF-8 boundary so
    // a cut multi-byte character cannot reach the renderer, where it would draw as '?'.
    for (int i = 0; i < windowLen; i++) {
      std::string& text = window[i].text;
      if (text.size() <= TEASER_MAX) continue;
      size_t n = TEASER_MAX;
      while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80) n--;
      text.resize(n);
      text += "...";
    }
  }
  const int off = uiIndex - windowStart;
  if (off < 0 || off >= windowLen) return nullptr;
  return &window[off];
}

void DictNotesActivity::openSelected() {
  // Re-read rather than use the row: the window holds teasers.
  DictNotes::Note full;
  if (DictNotes::loadWindow(dictHash_, selectedIndex, 1, &full) != 1) return;

  // No dictionary to ask (uninstalled, renamed, or the card came from another device -- the
  // picker lists such a hash on purpose), or nothing to ask it: show the text instead.
  if (full.word.empty() || dictionaryRegistry.indexOfHash(dictHash_) < 0) {
    openNoteText();
    return;
  }

  // Point the lookup at the dictionary the note was taken from, so the definition that opens is
  // the one the note marks -- and so computeNoteSpans, which reads the ACTIVE dictionary, finds
  // it. Same helper and same ordering the flashcard screens use: UI task, no lookup in flight.
  DictUtils::applyCardDict(dictHash_);
  // Not recorded in history: re-reading a note is not a new lookup, and it must not push the
  // word back up the book's list.
  controller_.startLookup(full.word, false);
}

void DictNotesActivity::openNoteText() {
  DictNotes::Note full;
  if (DictNotes::loadWindow(dictHash_, selectedIndex, 1, &full) != 1) return;

  viewWord_ = full.word;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int sidePad = metrics.contentSidePadding;
  const int contentWidth = renderer.getScreenWidth() - sidePad * 2;
  wrappedLines_ = renderer.wrappedText(kBodyFontId, full.text.c_str(), contentWidth, kWrapLineCap);

  lineHeight_ = renderer.getLineHeight(kBodyFontId);
  const int topReserve = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int bottomReserve = metrics.buttonHintsHeight + metrics.verticalSpacing;
  const int bodyHeight = renderer.getScreenHeight() - topReserve - bottomReserve;
  linesPerPage_ = (lineHeight_ > 0) ? std::max(1, bodyHeight / lineHeight_) : 1;

  pageOffset_ = 0;
  mode_ = Mode::Note;
  listTouch_.clear();  // the rows underneath must not take taps meant for the reader
  requestUpdate();
}

void DictNotesActivity::promptDelete() {
  const DictNotes::Note* sel = entryAt(selectedIndex);
  if (!sel) return;
  // Copied by value: the callback runs a frame or more later, by which point the window may
  // have been paged out from under it.
  const int index = selectedIndex;
  const uint32_t hash = dictHash_;
  const std::string body = sel->text;
  startActivityForResultNoThrow<ConfirmationActivity>(
      [this, index, hash](const ActivityResult& res) {
        if (res.isCancelled) {
          requestUpdate();  // note untouched
          return;
        }
        DictNotes::removeAt(hash, index);
        refreshCount();
        // Out of notes: this dictionary is gone from the picker, so there is nothing to come
        // back to here.
        if (totalCount == 0) {
          goBack();
          return;
        }
        requestUpdate();
      },
      renderer, mappedInput, tr(STR_DELETE), body);
}

void DictNotesActivity::loop() {
  using B = MappedInputManager::Button;

  // --- A lookup started from a note ---
  // Ahead of every mode: while the controller owns the screen it owns the input too.
  if (controller_.isActive()) {
    switch (controller_.handleInput()) {
      case DictionaryLookupController::LookupEvent::FoundDefinition: {
        // Nothrow: ~4.8 KB pushed straight after the lookup's glyph prewarm, on top of this
        // screen and whatever opened it. A bare new aborts the device (CLAUDE.md).
        auto definition = makeUniqueNoThrow<DictionaryDefinitionActivity>(
            renderer, mappedInput, controller_.getFoundWord(), controller_.getFoundLocation(), true, cachePath_,
            controller_.getRecordHistory(), controller_.getLookupWord(),
            DictionaryLookupController::toHistStatus(controller_.getFoundStatus()));
        if (!definition) {
          LOG_ERR("DNOTE", "OOM: DictionaryDefinitionActivity");
          requestUpdate();
          break;
        }
        startActivityForResult(std::move(definition), [this](const ActivityResult& result) {
          // A note may have been added or deleted from inside the definition.
          refreshCount();
          if (!result.isCancelled) {
            // Long-press Back in the definition means "back to the book", not "back to here".
            setResult(ActivityResult{});
            finish();
          } else {
            requestUpdate();
          }
        });
        break;
      }
      case DictionaryLookupController::LookupEvent::NotFoundDismissedBack:
        // The dictionary no longer answers for this word (a re-prepared or edited dictionary).
        // The note is still the user's text, so fall back to showing it rather than dead-ending.
        openNoteText();
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

  // --- Note text, the fallback view ---
  if (mode_ == Mode::Note) {
    if (mappedInput.wasReleased(B::Back)) {
      goBack();
      return;
    }
    const int maxOffset = std::max(0, static_cast<int>(wrappedLines_.size()) - linesPerPage_);
    const auto scroll = [this, maxOffset](const int delta) {
      const int next = std::clamp(pageOffset_ + delta, 0, maxOffset);
      if (next == pageOffset_) return;
      pageOffset_ = next;
      requestUpdate();
    };
    if (mappedInput.wasReleased(B::Down) || mappedInput.wasReleased(B::PageForward)) {
      scroll(linesPerPage_);
      return;
    }
    if (mappedInput.wasReleased(B::Up) || mappedInput.wasReleased(B::PageBack)) {
      scroll(-linesPerPage_);
      return;
    }
    // Touch: tap the left third to go back a page, the rest forward -- the same thirds the
    // definition screen pages by, so the gesture carries over from where the note was taken.
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasScreenTapped(tx, ty)) {
      scroll(tx >= renderer.getScreenWidth() / 3 ? linesPerPage_ : -linesPerPage_);
      return;
    }
    return;
  }

  // --- Dictionary picker ---
  if (mode_ == Mode::Dictionaries) {
    if (dicts_.empty()) {
      if (mappedInput.wasReleased(B::Back)) DictUtils::cancelAndFinish(*this);
      return;
    }
    const int totalItems = static_cast<int>(dicts_.size());
    const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false);
    buttonNavigator.onNextRelease([this, totalItems] {
      dictIndex_ = ButtonNavigator::nextIndex(dictIndex_, totalItems);
      requestUpdate();
    });
    buttonNavigator.onPreviousRelease([this, totalItems] {
      dictIndex_ = ButtonNavigator::previousIndex(dictIndex_, totalItems);
      requestUpdate();
    });
    buttonNavigator.onNextContinuous([this, totalItems, pageItems] {
      dictIndex_ = ButtonNavigator::nextPageIndex(dictIndex_, totalItems, pageItems);
      requestUpdate();
    });
    buttonNavigator.onPreviousContinuous([this, totalItems, pageItems] {
      dictIndex_ = ButtonNavigator::previousPageIndex(dictIndex_, totalItems, pageItems);
      requestUpdate();
    });

    int tapX = 0;
    int tapY = 0;
    const int tappedRow = mappedInput.wasScreenTapped(tapX, tapY) ? listTouch_.touchRow(renderer, tapX, tapY) : -1;
    if (tappedRow >= 0) dictIndex_ = tappedRow;

    if (mappedInput.wasReleased(B::Confirm) || tappedRow >= 0) {
      enterDictionary(dictIndex_);
      return;
    }
    if (mappedInput.wasReleased(B::Back)) goBack();
    return;
  }

  // --- One dictionary's notes ---
  if (totalCount == 0) {
    if (mappedInput.wasReleased(B::Back)) goBack();
    return;
  }

  // Hold to delete. Two gestures for one action: Confirm on a board with buttons, a long press
  // on the row itself where there is no Confirm pin to hold (BoardConfig.h, XTEINK_X4_PRO).
  if (mappedInput.isPressed(B::Confirm) && mappedInput.getHeldTime() >= Dictionary::LONG_PRESS_MS) {
    promptDelete();
    return;
  }
  int hx = 0;
  int hy = 0;
  if (mappedInput.wasScreenLongPress(hx, hy)) {
    const int row = listTouch_.touchRow(renderer, hx, hy);
    if (row >= 0) {
      selectedIndex = row;
      promptDelete();
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

  // A tap on a row selects and opens it in one go, like the other hand-rolled list screens.
  // A tap is never a hold, so it cannot reach the long-press branch above.
  int tapX = 0;
  int tapY = 0;
  const int tappedRow = mappedInput.wasScreenTapped(tapX, tapY) ? listTouch_.touchRow(renderer, tapX, tapY) : -1;
  if (tappedRow >= 0) selectedIndex = tappedRow;

  if (mappedInput.wasReleased(B::Confirm) || tappedRow >= 0) {
    openSelected();
    return;
  }

  if (mappedInput.wasReleased(B::Back)) goBack();
}

void DictNotesActivity::displayList() {
  // Fast waveform, with a periodic HALF scrub in landscape where repeated FAST accumulates DC
  // bias into progressive whitening. Same cadence LookedUpWordsActivity uses, for the same
  // panel reason.
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

void DictNotesActivity::render(RenderLock&&) {
  renderer.clearScreen();
  // The lookup overlays push their own frame.
  if (controller_.render()) return;

  switch (mode_) {
    case Mode::Dictionaries:
      renderDictionaries();
      break;
    case Mode::List:
      renderList();
      break;
    case Mode::Note:
      renderNote();
      break;
  }
  displayList();
}

void DictNotesActivity::renderDictionaries() {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageItems = std::max(1, UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false));

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_DICT_NOTES));
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  if (dicts_.empty()) {
    listTouch_.clear();
    const int midY = contentTop + (pageHeight - contentTop - metrics.buttonHintsHeight) / 2;
    renderer.drawCenteredText(UI_10_FONT_ID, midY, tr(STR_DICT_NOTES_EMPTY));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    return;
  }

  const int count = static_cast<int>(dicts_.size());
  const int contentHeight = pageItems * metrics.listRowHeight;
  listTouch_.record(Rect{0, contentTop, pageWidth, contentHeight}, count, dictIndex_);
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, count, dictIndex_,
      [this](int i) { return i >= 0 && i < static_cast<int>(dicts_.size()) ? dicts_[i].name : std::string(); }, nullptr,
      nullptr,
      // How many notes each dictionary holds, right-aligned: the row shape a settings list uses
      // for its values.
      [this](int i) {
        return i >= 0 && i < static_cast<int>(dicts_.size()) ? std::to_string(dicts_[i].count) : std::string();
      },
      false);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void DictNotesActivity::renderList() {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Same row count the nav uses (orientation-aware), so render and navigation always agree on
  // rows per page.
  const int pageItems = std::max(1, UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false));
  static_assert(WINDOW_CAP >= 16, "the window must cover a full page of rows");
  const int curPage = selectedIndex / pageItems + 1;
  const int totalPages = std::max(1, (totalCount + pageItems - 1) / pageItems);

  char titleBuf[64];
  // The dictionary's own name, not "Dictionary Notes": the rows below are scoped to it, and
  // which dictionary they came from is the one thing the list cannot otherwise show.
  snprintf(titleBuf, sizeof(titleBuf), tr(STR_LOOKUP_HIST_HEADER_FORMAT),
           dictName_.empty() ? tr(STR_DICT_NOTES) : dictName_.c_str(), static_cast<int>(totalCount), curPage,
           totalPages);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, titleBuf);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  if (totalCount == 0) {
    listTouch_.clear();
    const int midY = contentTop + (pageHeight - contentTop - metrics.buttonHintsHeight) / 2;
    renderer.drawCenteredText(UI_10_FONT_ID, midY, tr(STR_DICT_NOTES_EMPTY));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    return;
  }

  const int contentHeight = pageItems * metrics.listRowHeight;
  listTouch_.record(Rect{0, contentTop, pageWidth, contentHeight}, totalCount, selectedIndex);
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, totalCount, selectedIndex,
      [this](int i) {
        const auto* e = entryAt(i);
        if (!e) return std::string();
        // The headword the note was taken from, then the text: a list of bare excerpts gives no
        // clue which definition each came from.
        return e->word + " - " + e->text;
      },
      nullptr, nullptr, nullptr, false);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void DictNotesActivity::renderNote() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int sidePad = metrics.contentSidePadding;

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, viewWord_.c_str());

  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int visible = std::min(linesPerPage_, static_cast<int>(wrappedLines_.size()) - pageOffset_);
  for (int i = 0; i < visible; i++) {
    renderer.drawText(kBodyFontId, sidePad, top + i * lineHeight_, wrappedLines_[pageOffset_ + i].c_str(), true,
                      EpdFontFamily::REGULAR);
  }

  const bool more = pageOffset_ + linesPerPage_ < static_cast<int>(wrappedLines_.size());
  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), "", pageOffset_ > 0 ? tr(STR_DIR_UP) : "", more ? tr(STR_DIR_DOWN) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
