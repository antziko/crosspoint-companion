#include "DictionaryWordSelectActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Utf8.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstring>

#include "CrossPointSettings.h"
#include "DictionaryDefinitionActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/Dictionary.h"
#include "util/DictionaryActivityUtils.h"
#include "util/FlashcardDeck.h"

namespace {

// A display token ends a sentence if its last byte is ASCII '.', '!' or '?'.
// Multibyte UTF-8 characters end with a continuation byte (>= 0x80), so a raw
// last-byte test never false-matches them.
bool endsSentence(const char* s) {
  if (!s || !*s) return false;
  const size_t n = strlen(s);
  const char c = s[n - 1];
  return c == '.' || c == '!' || c == '?';
}

// Soft-hyphen U+00AD encoded as 2 UTF-8 bytes. Layout (ParsedText.cpp:19)
// strips these before measurement, so we mirror that here — otherwise
// derived word widths include the soft-hyphen glyph's advance and the
// highlight rectangle overruns into the inter-word gap.
constexpr char SOFT_HYPHEN_UTF8[] = "\xC2\xAD";
constexpr size_t SOFT_HYPHEN_BYTES = 2;

int16_t measureWordAdvanceX(const GfxRenderer& renderer, int fontId, const std::string& word,
                            EpdFontFamily::Style style) {
  if (word.find(SOFT_HYPHEN_UTF8) == std::string::npos) {
    return static_cast<int16_t>(renderer.getTextAdvanceX(fontId, word.c_str(), style));
  }
  std::string sanitized = word;
  size_t pos = 0;
  while ((pos = sanitized.find(SOFT_HYPHEN_UTF8, pos)) != std::string::npos) {
    sanitized.erase(pos, SOFT_HYPHEN_BYTES);
  }
  return static_cast<int16_t>(renderer.getTextAdvanceX(fontId, sanitized.c_str(), style));
}

// Single-style prewarm/advance-table bitmask: bit 0 = REGULAR, 1 = BOLD,
// 2 = ITALIC, 3 = BOLD_ITALIC. The `& 0x03` is defensive — Style enum
// is two bits, but UNDERLINE etc. live in higher bits if ever OR'd in.
constexpr uint8_t styleToBitMask(EpdFontFamily::Style style) {
  return static_cast<uint8_t>(1u << (static_cast<uint8_t>(style) & 0x03));
}

}  // namespace

void DictionaryWordSelectActivity::onEnter() {
  Activity::onEnter();
  std::vector<WordSelectNavigator::WordInfo> words;
  std::vector<WordSelectNavigator::Row> rows;
  std::string textPool;
  textPool.reserve(512);
  extractWords(words, rows, textPool);
  mergeHyphenatedWords(words, rows, textPool);
  // Only consume the initial Confirm release if Confirm is still held at onEnter — i.e.
  // we were opened mid hold-to-lookup. Other entry paths (e.g. reader menu → Lookup) have
  // already released Confirm by the time we open, so consuming would swallow the user's
  // first deliberate tap and force them to press twice.
  const bool consumeInitialConfirm = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  navigator.load(std::move(words), std::move(rows), std::move(textPool), consumeInitialConfirm, initialMarker_);
  // Opened via the reader's hold-Back gesture? Back is still held — swallow its release once.
  consumeInitialBackRelease_ = mappedInput.isPressed(MappedInputManager::Button::Back);
  requestUpdate();
}

void DictionaryWordSelectActivity::onExit() {
  controller.onExit();
  Activity::onExit();
}

void DictionaryWordSelectActivity::prewarmHighlightGlyphs(int currIdx) {
  const auto* w = navigator.getWordAt(currIdx);
  if (!w) return;
  auto* fcm = renderer.getFontCacheManager();
  if (!fcm) return;
  fcm->prewarmCache(SETTINGS.getReaderFontId(), navigator.getDisplay(*w), styleToBitMask(w->style));
}

void DictionaryWordSelectActivity::prebuildAdvanceTable() {
  // Concatenate every word on the page and OR the style flags. ~2KB transient
  // string; freed on return. Matches FontCacheManager::PrewarmScope's
  // scanText_ allocation pattern.
  std::string pageText;
  pageText.reserve(2048);
  uint8_t pageStyleMask = 0;
  for (const auto& element : page->elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(element.get());
    const auto& block = line->getBlock();
    if (!block) continue;
    const uint16_t n = block->wordCount();
    for (uint16_t i = 0; i < n; i++) {
      pageText.append(block->wordText(i), block->wordTextLen(i));
      pageText.push_back(' ');
      pageStyleMask |= styleToBitMask(block->wordStyle(i));
    }
  }
  if (pageStyleMask == 0) pageStyleMask = styleToBitMask(EpdFontFamily::REGULAR);
  // The advance table persists across clearCache() (SdCardFont.h:201) so
  // this only pays the SD cost on the first entry; subsequent ones
  // amortize.
  renderer.ensureSdCardFontReady(SETTINGS.getReaderFontId(), pageText.c_str(), pageStyleMask);
}

void DictionaryWordSelectActivity::extractWords(std::vector<WordSelectNavigator::WordInfo>& words,
                                                std::vector<WordSelectNavigator::Row>& rows, std::string& textPool) {
  words.clear();
  words.reserve(64);
  rows.clear();
  rows.reserve(16);

  // Populate the SD font's advance table once so every getTextAdvanceX call
  // below takes the fast in-RAM path.
  prebuildAdvanceTable();

  // Fallback used by blocks where we can't derive a per-line gap
  // (single-word blocks, degenerate first-word measurements).
  const int16_t naturalSpaceWidth =
      static_cast<int16_t>(renderer.getTextAdvanceX(SETTINGS.getReaderFontId(), " ", EpdFontFamily::REGULAR));

  for (const auto& element : page->elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(element.get());
    const auto& block = line->getBlock();
    if (!block) continue;

    // Flat per-word storage (TextBlock stores words back-to-back in a single
    // NUL-terminated arena; wordText(i) is a stable const char*, wordTextLen(i)
    // its byte length excluding the NUL).
    const uint16_t blockWordCount = block->wordCount();

    // Per-line gap = xPos[1] - xPos[0] - firstWordWidth. Justified blocks
    // stretch the gap (ParsedText.cpp:514-553 adds justifyExtra), so a
    // global space-width can't be reused — we measure per-block.
    int16_t lineGapWidth = naturalSpaceWidth;
    if (blockWordCount >= 2 && block->wordTextLen(0) > 0) {
      const EpdFontFamily::Style firstStyle = block->wordStyle(0);
      const std::string firstWord(block->wordText(0), block->wordTextLen(0));
      const int16_t firstWidth = measureWordAdvanceX(renderer, SETTINGS.getReaderFontId(), firstWord, firstStyle);
      const int16_t derivedGap = static_cast<int16_t>(block->wordXpos(1) - block->wordXpos(0) - firstWidth);
      // When wordList[1] is a continuation (attached punctuation etc., ParsedText.cpp:537-544)
      // the layout inserts no inter-word gap, so derivedGap collapses to the kerning offset
      // (~1-3 px). Real gaps are always >= getSpaceAdvance(...), so a half-space threshold
      // cleanly separates a real gap from a continuation kerning without needing Block to
      // expose continuesVec. Without the threshold, an undersized lineGapWidth propagates as
      // a per-word width overestimate (~4-6 px) — the highlight rectangle bleeds past the
      // word into the inter-word space.
      if (derivedGap > naturalSpaceWidth / 2) lineGapWidth = derivedGap;
    }

    // Ruby-annotated lines shift their base text down by half an ascender (see
    // TextBlock::getRubyShift). Move the tap targets in lockstep so the selection
    // boxes stay aligned with the rendered word positions.
    const int rubyShift = block->getRubyShift(renderer.getFontAscenderSize(SETTINGS.getReaderFontId()));

    for (uint16_t wIdx = 0; wIdx < blockWordCount; wIdx++) {
      int16_t screenX = line->xPos + block->wordXpos(wIdx) + marginLeft;
      int16_t screenY = line->yPos + marginTop + rubyShift;
      const std::string wordText(block->wordText(wIdx), block->wordTextLen(wIdx));
      const EpdFontFamily::Style wordStyle = block->wordStyle(wIdx);

      // Skip tokens with no alphanumeric characters (bullets, punctuation, etc.)
      if (!std::any_of(wordText.begin(), wordText.end(), [](unsigned char c) { return std::isalnum(c); })) {
        continue;
      }

      // Split on en-dash (U+2013: E2 80 93) and em-dash (U+2014: E2 80 94)
      std::vector<size_t> splitStarts;
      splitStarts.reserve(4);
      size_t partStart = 0;
      for (size_t i = 0; i < wordText.size();) {
        if (i + 2 < wordText.size() && static_cast<uint8_t>(wordText[i]) == 0xE2 &&
            static_cast<uint8_t>(wordText[i + 1]) == 0x80 &&
            (static_cast<uint8_t>(wordText[i + 2]) == 0x93 || static_cast<uint8_t>(wordText[i + 2]) == 0x94)) {
          if (i > partStart) splitStarts.push_back(partStart);
          i += 3;
          partStart = i;
        } else {
          i++;
        }
      }
      if (partStart < wordText.size()) splitStarts.push_back(partStart);

      if (splitStarts.size() <= 1 && partStart == 0) {
        // width = (xPos[i+1] - xPos[i]) - lineGapWidth, which is the layout's
        // xpos diff with the trailing inter-word gap removed. Punctuation
        // tokens skipped above kept their xpos entries as boundary markers,
        // so this works regardless of what the next token is.
        // Last word per block has no next xpos; fall back to direct
        // measurement. Clamp to 1 to guard pathological cases (continuation
        // negative kerning, short words where the entire xpos diff is the
        // gap).
        int16_t wordWidth;
        if (wIdx + 1 < blockWordCount) {
          const int16_t raw = static_cast<int16_t>(block->wordXpos(wIdx + 1) - block->wordXpos(wIdx));
          wordWidth = std::max(static_cast<int16_t>(1), static_cast<int16_t>(raw - lineGapWidth));
        } else {
          wordWidth = measureWordAdvanceX(renderer, SETTINGS.getReaderFontId(), wordText, wordStyle);
        }
        {
          uint16_t off = WordSelectNavigator::poolAppend(textPool, wordText.c_str(), wordText.size());
          WordSelectNavigator::WordInfo wi;
          wi.textOffset = off;
          wi.textLen = static_cast<uint16_t>(wordText.size());
          wi.lookupOffset = off;
          wi.lookupLen = wi.textLen;
          wi.screenX = screenX;
          wi.screenY = screenY;
          wi.width = wordWidth;
          wi.style = wordStyle;
          wi.fontId = SETTINGS.getReaderFontId();
          words.push_back(wi);
        }
      } else {
        for (size_t si = 0; si < splitStarts.size(); si++) {
          size_t start = splitStarts[si];
          size_t end = (si + 1 < splitStarts.size()) ? splitStarts[si + 1] : wordText.size();
          size_t textEnd = end;
          while (textEnd > start && textEnd <= wordText.size()) {
            if (textEnd >= 3 && static_cast<uint8_t>(wordText[textEnd - 3]) == 0xE2 &&
                static_cast<uint8_t>(wordText[textEnd - 2]) == 0x80 &&
                (static_cast<uint8_t>(wordText[textEnd - 1]) == 0x93 ||
                 static_cast<uint8_t>(wordText[textEnd - 1]) == 0x94)) {
              textEnd -= 3;
            } else {
              break;
            }
          }
          std::string part = wordText.substr(start, textEnd - start);
          if (part.empty()) continue;

          std::string prefix = wordText.substr(0, start);
          // Dash-split words are rare (~0-2 per page); per-part measurement
          // is fine here. Soft-hyphen stripping matches the rest of
          // extractWords and matches layout's preprocessor.
          int16_t offsetX =
              prefix.empty() ? 0 : measureWordAdvanceX(renderer, SETTINGS.getReaderFontId(), prefix, wordStyle);
          int16_t partWidth = measureWordAdvanceX(renderer, SETTINGS.getReaderFontId(), part, wordStyle);
          {
            uint16_t off = WordSelectNavigator::poolAppend(textPool, part.c_str(), part.size());
            WordSelectNavigator::WordInfo wi;
            wi.textOffset = off;
            wi.textLen = static_cast<uint16_t>(part.size());
            wi.lookupOffset = off;
            wi.lookupLen = wi.textLen;
            wi.screenX = static_cast<int16_t>(screenX + offsetX);
            wi.screenY = screenY;
            wi.width = partWidth;
            wi.style = wordStyle;
            wi.fontId = SETTINGS.getReaderFontId();
            words.push_back(wi);
          }
        }
      }
    }
  }

  WordSelectNavigator::organizeIntoRows(words, rows);
}

void DictionaryWordSelectActivity::mergeHyphenatedWords(std::vector<WordSelectNavigator::WordInfo>& words,
                                                        std::vector<WordSelectNavigator::Row>& rows,
                                                        std::string& textPool) {
  WordSelectNavigator::mergeHyphenatedPairs(words, rows, textPool);

  // Cross-page hyphenation: update lookup text when the last word on this page
  // ends with a hyphen and its continuation begins the next page.
  if (!nextPageFirstWord.empty() && !rows.empty()) {
    int lastWordIdx = rows.back().wordIndices.back();
    const char* lastWord = textPool.data() + words[lastWordIdx].textOffset;
    uint16_t lastLen = words[lastWordIdx].textLen;
    if (lastLen > 0 && utf8EndsWithHyphen(lastWord, lastLen) && lastWord[0] != '-') {
      std::string firstPart(lastWord, lastLen);
      utf8RemoveTrailingHyphen(firstPart);
      std::string merged = firstPart + nextPageFirstWord;
      uint16_t off = WordSelectNavigator::poolAppend(textPool, merged.c_str(), merged.size());
      words[lastWordIdx].lookupOffset = off;
      words[lastWordIdx].lookupLen = static_cast<uint16_t>(merged.size());
    }
  }

  rows.erase(
      std::remove_if(rows.begin(), rows.end(), [](const WordSelectNavigator::Row& r) { return r.wordIndices.empty(); }),
      rows.end());
}

// Page-local sentence the current selection sits in, for the flashcard front
// face. Walks outward from the selected word (or the anchor..cursor span for a
// phrase) to the nearest sentence-ending token or page edge, bounded by word
// count, then joins via the existing buildPhrase primitive. Page-clipped
// sentences are accepted (the navigator only holds the current page). Returns
// "" if there is no selection.
std::string DictionaryWordSelectActivity::buildLookupExcerpt() const {
  const int sel = navigator.getCurrentFlatIndex();
  if (sel < 0) return "";

  // Phrase lookups span anchor..cursor; single lookups are just the cursor word.
  const int anchor = navigator.getAnchorFlatIndex();
  int lo = sel, hi = sel;
  if (anchor >= 0) {
    lo = std::min(anchor, sel);
    hi = std::max(anchor, sel);
  }

  static constexpr int MAX_EXCERPT_WORDS = 40;  // bounds the joined string length
  // Extend left until the previous token ends a sentence (or page start).
  while (lo > 0 && (hi - lo + 1) < MAX_EXCERPT_WORDS) {
    const auto* prev = navigator.getWordAt(lo - 1);
    if (!prev || endsSentence(navigator.getDisplay(*prev))) break;
    lo--;
  }
  // Extend right until the current token ends a sentence (or page end).
  while ((hi - lo + 1) < MAX_EXCERPT_WORDS) {
    const auto* cur = navigator.getWordAt(hi);
    if (!cur || endsSentence(navigator.getDisplay(*cur))) break;
    if (!navigator.getWordAt(hi + 1)) break;  // page edge
    hi++;
  }

  std::string excerpt = navigator.buildPhrase(lo, hi);
  // Trim to the deck's cap on a word boundary where possible (the deck also
  // hard-caps, but this avoids storing a mid-word fragment).
  if (static_cast<int>(excerpt.size()) > FlashcardDeck::EXCERPT_MAX) {
    excerpt.resize(FlashcardDeck::EXCERPT_MAX);
    const size_t sp = excerpt.find_last_of(' ');
    if (sp != std::string::npos && sp > 0) excerpt.resize(sp);
  }
  return excerpt;
}

void DictionaryWordSelectActivity::loop() {
  if (controller.isActive()) {
    switch (controller.handleInput()) {
      case DictionaryLookupController::LookupEvent::FoundDefinition: {
        // Auto-enroll the looked-up word as a flashcard. This is the sole site
        // with live page context for the excerpt; the navigator still holds the
        // page words + current selection here (it is reset on activity exit).
        if (!cachePath.empty()) {
          FlashcardDeck::enroll(cachePath, controller.getLookupWord(), buildLookupExcerpt(), chapterTitle_);
        }
        startActivityForResult(std::make_unique<DictionaryDefinitionActivity>(
                                   renderer, mappedInput, controller.getFoundWord(), controller.getFoundLocation(),
                                   true, cachePath, controller.getRecordHistory(), controller.getLookupWord(),
                                   DictionaryLookupController::toHistStatus(controller.getFoundStatus())),
                               [this](const ActivityResult& result) {
                                 if (!result.isCancelled) {
                                   setResult(ActivityResult{});
                                   finish();
                                 } else {
                                   forceFullRepaintOnNextRender();
                                   requestUpdate();
                                 }
                               });
        break;
      }
      case DictionaryLookupController::LookupEvent::NotFoundDismissedBack:
        forceFullRepaintOnNextRender();
        requestUpdate();
        break;
      case DictionaryLookupController::LookupEvent::NotFoundDismissedDone:
        setResult(ActivityResult{});
        finish();
        break;
      case DictionaryLookupController::LookupEvent::Cancelled:
        forceFullRepaintOnNextRender();
        requestUpdate();
        break;
      default:
        break;
    }
    return;
  }

  // Swallow the Back release that ended the launching hold-Back gesture (see onEnter), so it
  // doesn't fall through to the cancel handlers below on the first tick.
  if (consumeInitialBackRelease_) {
    const bool released = mappedInput.wasReleased(MappedInputManager::Button::Back);
    if (!mappedInput.isPressed(MappedInputManager::Button::Back)) {
      consumeInitialBackRelease_ = false;
      if (released) return;
    }
  }

  if (navigator.isEmpty()) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      DictUtils::cancelAndFinish(*this);
    }
    return;
  }

  if (navigator.handleNavigation(mappedInput, renderer)) {
    requestUpdate();
  }

  // Check Back early when not in multi-select mode. This allows exit even when
  // confirmReleaseConsumed is stuck true (menu-triggered entry has no Confirm release).
  if (!navigator.isMultiSelecting() && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    DictUtils::cancelAndFinish(*this);
    return;
  }

  // HighlightRange mode: reuse the same single-word + long-press-range selection gesture,
  // but emit a quote result instead of a dictionary lookup.
  if (mode_ == Mode::HighlightRange) {
    handleHighlightInput();
    return;
  }

  if (controller.handleMultiSelect(navigator)) return;

  if (navigator.isMultiSelecting()) return;

  if (controller.handleConfirmLookup(navigator)) return;

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    DictUtils::cancelAndFinish(*this);
    return;
  }
}

const char* DictionaryWordSelectActivity::confirmHintLabel() const {
  return mode_ == Mode::HighlightRange ? tr(STR_ADD_HIGHLIGHT) : tr(STR_LOOKUP_SHORT);
}

void DictionaryWordSelectActivity::handleHighlightInput() {
  std::string phrase;
  const auto act = navigator.handleMultiSelectInput(mappedInput, phrase);
  if (act != WordSelectNavigator::MultiSelectAction::None) {
    if (act == WordSelectNavigator::MultiSelectAction::PhraseReady) {
      emitQuoteResult(navigator.getAnchorFlatIndex(), navigator.getCurrentFlatIndex(), std::move(phrase));
    } else {
      // Entered/Exited multi-select, or a consumed long-press carryover — just repaint
      // so the highlight reflects the new selection state.
      requestUpdate();
    }
    return;
  }

  // A plain Confirm tap (no long-press, not in multi-select) saves a single-word quote.
  if (!navigator.isMultiSelecting() && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const auto* sel = navigator.getSelected();
    if (sel) {
      const int idx = navigator.getCurrentFlatIndex();
      emitQuoteResult(idx, idx, navigator.getDisplay(*sel));
    }
  }
}

void DictionaryWordSelectActivity::emitQuoteResult(int fromFlatIdx, int toFlatIdx, std::string previewText) {
  if (fromFlatIdx < 0 || toFlatIdx < 0) {
    DictUtils::cancelAndFinish(*this);  // selection went stale; treat as cancel
    return;
  }
  HighlightRangeResult result;
  result.startWordIndex = std::min(fromFlatIdx, toFlatIdx);
  result.endWordIndex = std::max(fromFlatIdx, toFlatIdx);
  result.previewText = std::move(previewText);
  setResult(ActivityResult{std::move(result)});
  finish();
}

void DictionaryWordSelectActivity::render(RenderLock&&) {
  const int lineHeight = renderer.getLineHeight(SETTINGS.getReaderFontId());
  const int currIdx = navigator.getCurrentFlatIndex();

  // Differential fast path. Only valid when:
  //   - we set it up on the previous frame (RenderMode::Differential),
  //   - the controller has nothing pending to draw,
  //   - we have a current selection.
  if (nextRenderMode_ == RenderMode::Differential && !controller.isActive() && currIdx >= 0) {
    prewarmHighlightGlyphs(currIdx);
    auto dirty = navigator.renderHighlightDifferential(renderer, lineHeight, prevHighlightIdx_, currIdx);
    if (dirty.has_value()) {
      // Push full panel — the SDK's windowed-refresh path produces alternating black→white
      // transition failures on consecutive fast partial refreshes, so it's intentionally not
      // wired up here. The savings come from skipping page->render, which dominates the
      // pre-optimization cost; the full push at the end is a hardware floor (~444ms).
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      prevHighlightIdx_ = currIdx;
      return;
    }
    // Fall through to full repaint.
  }

  // Skip-initial-render fast path. Fires at most once per activity instance,
  // when the caller signalled the framebuffer already contains the page at
  // our margins (currently only EpubReaderActivity's hold-to-lookup path).
  // Conditions:
  //   - flag still set (one-shot),
  //   - controller has nothing to draw (an active controller would mean we
  //     re-entered render() after a sub-activity returned without the
  //     framebuffer being reset by forceFullRepaintOnNextRender()),
  //   - we have a current selection (currIdx >= 0); otherwise there is
  //     nothing to overlay and we fall through to a normal repaint.
  // We consume the flag unconditionally on first entry so any later
  // full-repaint goes through the normal clearScreen + page->render path.
  if (framebufferContainsPage_) {
    framebufferContainsPage_ = false;
    if (!controller.isActive() && currIdx >= 0) {
      // Clear the bottom strip the caller reserved (status bar OR auto-turn
      // label). Match the menu→lookup path, which wipes via clearScreen() +
      // page->render(); we skipped both, so clear that one region instead.
      if (reservedBottomHeight_ > 0) {
        int bezelTop, bezelRight, bezelBottom, bezelLeft;
        renderer.getOrientedViewableTRBL(&bezelTop, &bezelRight, &bezelBottom, &bezelLeft);
        const int clearY = renderer.getScreenHeight() - bezelBottom - reservedBottomHeight_;
        const int clearW = renderer.getScreenWidth() - bezelLeft - bezelRight;
        renderer.clearRect(bezelLeft, clearY, clearW, reservedBottomHeight_);
      }

      prewarmHighlightGlyphs(currIdx);

      auto setup = navigator.renderHighlightDifferential(renderer, lineHeight, /*prevWordIdx=*/-1, currIdx);
      bool snapshotPrimed = setup.has_value();
      if (!snapshotPrimed) {
        // Hyphenated wrap or oversize capture. The framebuffer still holds
        // the page, but we cannot prime the snapshot for the differential
        // path. Draw the multi-word highlight (which overwrites pixels under
        // each highlight rect) and force the next render to do a full
        // repaint so the renderer state is consistent. The user just pays
        // for one regular page render on the next cursor move instead of
        // on entry.
        navigator.renderHighlight(renderer, lineHeight);
      }
      const auto labels = mappedInput.mapLabels("", confirmHintLabel(), "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      prevHighlightIdx_ = currIdx;
      nextRenderMode_ = snapshotPrimed ? RenderMode::Differential : RenderMode::FullPage;
      return;
    }
    // Flag was set but conditions weren't met (controller active or no
    // current selection). Fall through to the normal full-repaint path.
  }

  // Full repaint path.
  renderer.clearScreen();
  if (controller.render()) {
    // Controller drew an overlay; framebuffer state is unknown.
    nextRenderMode_ = RenderMode::FullPage;
    prevHighlightIdx_ = -1;
    return;
  }

  // Font prewarm: scan pass accumulates text, then prewarm, then real render.
  // Without this, every cold codepoint cold-misses the 8-slot SD glyph
  // overflow ring and the page render serializes ~100+ individual SD reads.
  // Same pattern as EpubReaderActivity::renderContents().
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  page->render(renderer, SETTINGS.getReaderFontId(), marginLeft, marginTop);  // scan pass
  scope.endScanAndPrewarm();
  page->render(renderer, SETTINGS.getReaderFontId(), marginLeft, marginTop);

  // Set up snapshot AND draw the highlight via the differential entry point with
  // prevWordIdx = -1 (no previous highlight to wipe). This both draws the highlight
  // for this frame and primes snapshot_ so the next frame can run the fast path.
  // If the navigator declines (multi-select, hyphenated, oversize), fall back to
  // the multi-word renderHighlight and stay on the full path next frame.
  //
  // The -1 literal is load-bearing: renderHighlightDifferential uses prevWordIdx
  // < 0 as the signal "framebuffer was just redrawn from scratch, discard any
  // stale snapshot rather than restoring it on top of fresh pixels." This is the
  // only path that disturbs the framebuffer outside the differential cycle, so
  // it's also the only call site that must pass -1.
  bool snapshotPrimed = false;
  if (currIdx >= 0) {
    auto setup = navigator.renderHighlightDifferential(renderer, lineHeight, /*prevWordIdx=*/-1, currIdx);
    snapshotPrimed = setup.has_value();
  }
  if (!snapshotPrimed) {
    navigator.renderHighlight(renderer, lineHeight);
  }

  const auto labels = mappedInput.mapLabels("", confirmHintLabel(), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);

  prevHighlightIdx_ = currIdx;
  nextRenderMode_ = snapshotPrimed ? RenderMode::Differential : RenderMode::FullPage;
}
