#pragma once
#include <cstdint>
#include <string>

#include "../Activity.h"
#include "util/ButtonNavigator.h"
#include "util/DictionaryLookupController.h"
#include "util/FlashcardDeck.h"

// Browse the per-book flashcard deck (FlashcardDeck) as a paged list, modeled on
// LookedUpWordsActivity: the deck is paged from SD a window at a time (never
// materialized), so RAM is bounded regardless of deck size. Each row shows a box
// glyph + the word; Confirm opens an in-activity Detail phase (full card face +
// Leitner status + a re-lookup button reusing DictionaryLookupController);
// long-press Confirm deletes the card.
//
// The list window loads word + box/dueDay ONLY (wordsOnly), so each row costs one
// string allocation, not three -- the excerpt/chapter are read on demand for the
// single resident Detail card. Detail is a Phase, not a second Activity, so the
// one DictionaryLookupController member is shared by both list-lookup and
// detail-lookup (one fewer heap object on the fragmented reader heap).
class FlashcardListActivity final : public Activity {
 public:
  explicit FlashcardListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookCachePath)
      : Activity("FlashcardList", renderer, mappedInput),
        cachePath(std::move(bookCachePath)),
        controller(renderer, mappedInput, *this, cachePath) {}

  void onEnter() override;
  void onExit() override;
  void onResume() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Phase { List, Detail };

  // Clears any session dictionary override (set by long-press Confirm on the definition
  // screen) when this activity is destroyed, so a switch cannot outlive the screen that
  // hosted it. RAII rather than a call in onExit(): activities are heap-allocated and
  // deleted on exit, so the destructor always runs.
  Dictionary::SessionOverrideScope dictOverrideScope_;

  std::string cachePath;
  // Same windowed-paging discipline as LookedUpWordsActivity: only the on-screen
  // page lives in RAM. WINDOW_CAP is the real max rows/page (not over-sized), and
  // each Entry holds only `word` (wordsOnly load) to keep per-row heap minimal.
  int totalCount = 0;
  static constexpr int WINDOW_CAP = 16;  // >= max rows per page; bounded RAM
  FlashcardDeck::Entry window[WINDOW_CAP];
  int windowStart = -1;  // newest-first index of window[0]; -1 = invalid
  int windowLen = 0;
  int selectedIndex = 0;
  bool deleteConfirmMode = false;
  bool confirmReleaseConsumed = false;
  int pagesUntilFullRefresh = 0;  // landscape ghost-scrub cadence (0 = scrub next render)

  Phase phase = Phase::List;
  FlashcardDeck::Entry detail;  // the one resident full card (word+excerpt+chapter)
  uint32_t today = 0;           // days-since-2000 (local), 0 if clock unavailable
  bool clockOk = false;

  DictionaryLookupController controller;
  ButtonNavigator buttonNavigator;

  bool skipLoopDelay() override { return controller.skipLoopDelay(); }

  // Convert UI index (0 = most recent) to 0-based file index (0 = oldest).
  int fileIndexOf(int uiIndex) const { return totalCount - 1 - uiIndex; }

  void displayList();                                // push with the list refresh policy (FAST + landscape scrub)
  void refreshCount();                               // refresh totalCount from SD; invalidate cached window
  const FlashcardDeck::Entry* entryAt(int uiIndex);  // word-only paged fetch

  void openDetail();  // load the selected card full -> phase = Detail
  void renderList();
  void renderDetail();

  // Single-char status glyph for the row (mastered/suspended/new/due/box digit).
  static const char* glyphFor(const FlashcardDeck::Entry& e, uint32_t today);
};
