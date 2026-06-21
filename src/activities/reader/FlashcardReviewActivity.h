#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../Activity.h"
#include "CrossPointSettings.h"
#include "util/DictionaryLookupController.h"
#include "util/FlashcardDeck.h"

// Per-book spaced-repetition review loop over the dictionary-lookup flashcard
// deck (FlashcardDeck). Modeled on LookedUpWordsActivity: it owns a
// DictionaryLookupController and reuses DictionaryDefinitionActivity (with
// recordHistory=false) for the card BACK face, so the dict render-race and
// font-fragmentation hazards are handled by that proven path rather than
// re-implemented here.
//
// The session is a bounded vector of newest-first deck indices selected by
// FlashcardDeck::buildSession and shuffled here for presentation. Only one card
// (word + excerpt + schedule) is resident at a time; the deck is never
// materialized.
class FlashcardReviewActivity final : public Activity {
 public:
  explicit FlashcardReviewActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookCachePath)
      : Activity("FlashcardReview", renderer, mappedInput),
        cachePath(std::move(bookCachePath)),
        controller(renderer, mappedInput, *this, cachePath) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Overview = pre-session deck stats (box-ladder); Confirm starts the session.
  // Front = card context shown, word hidden (cloze) or shown (word+context).
  // AwaitingGrade = the definition (back face) has been viewed; pass/fail prompt.
  // Summary = session finished; show the tally.
  enum class Phase { Overview, Front, AwaitingGrade, Summary };

  static constexpr int SESSION_CAP = 30;  // bounded session; reserve()'d once

  std::string cachePath;
  DictionaryLookupController controller;

  std::vector<uint16_t> session;  // newest-first deck indices, shuffled
  size_t cursor = 0;
  uint32_t today = 0;
  bool clockOk = false;

  FlashcardDeck::Entry card;   // the one resident card
  FlashcardDeck::Stats stats;  // deck-wide stats for the overview screen
  Phase phase = Phase::Overview;
  uint8_t cardStyle = 0;  // FLASHCARD_STYLE_* chosen on the overview (Up/Down)

  // Session tally (rendered on the summary screen).
  int reviewed = 0;
  int correct = 0;
  int mastered = 0;

  int pagesUntilFullRefresh = 0;  // landscape ghost-scrub cadence (0 = scrub next render)

  bool skipLoopDelay() override { return controller.skipLoopDelay(); }

  void buildAndShuffleSession(FlashcardDeck::SessionScope scope);
  // Start a review session in `scope` (persists the choice as the new default,
  // then builds + shuffles). No-op if the chosen scope yields no cards.
  void startSession(FlashcardDeck::SessionScope scope);
  bool loadCurrentCard();  // fills `card` from session[cursor]; false if exhausted
  void gradeAndAdvance(bool correctRecall);
  void displayList();  // push with the list refresh policy (FAST + landscape scrub)

  // Render helpers.
  void renderOverview(int contentTop, int contentBottom, int pageWidth);
  void renderFront(int contentTop, int contentBottom, int pageWidth);
  void renderAwaitingGrade(int contentTop, int contentBottom, int pageWidth);
  void renderSummary(int contentTop, int contentBottom, int pageWidth);
  // Word-wrap `text` centered within [contentTop, contentBottom); returns the y
  // after the last line. Bounded by the excerpt cap, no heap beyond one line.
  int drawWrappedCentered(int fontId, int contentTop, int contentBottom, int pageWidth, const char* text);
  // Draw the card's chapter title (if any) as a small footer just above the
  // button hints. No-op when the card has no chapter.
  void drawChapterFooter(int contentBottom);
};
