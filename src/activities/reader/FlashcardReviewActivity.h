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
  // Revealed = cloze answer shown after a front grade; the grade is re-pickable
  //   (Left/Right), Confirm flips to the definition, the "prev" side button
  //   (PageBack) suspends and the "next" side button (PageForward) commits the
  //   grade + advances -- the "now that I see it, grade honestly" step, only
  //   reached from a cloze front, never word+context.
  // AwaitingGrade = the definition (back face) has been viewed; pass/fail prompt.
  // Summary = session finished; show the tally.
  enum class Phase { Overview, Front, Revealed, AwaitingGrade, Summary };

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
  uint8_t cardStyle = 0;        // FLASHCARD_STYLE_* chosen on the overview (Up/Down)
  bool pendingCorrect = false;  // grade picked on a cloze front, committed on Revealed-confirm
  // A suspended-review session (entered from the overview when cards are set
  // aside): cards are always shown word+context, Pass/Fail are disabled, and the
  // Up button unsuspends instead of suspending.
  bool suspendedMode = false;

  // Session tally (rendered on the summary screen).
  int reviewed = 0;
  int correct = 0;
  int mastered = 0;
  int suspended = 0;  // cards set aside (or restored, in suspendedMode) this session

  int pagesUntilFullRefresh = 0;  // landscape ghost-scrub cadence (0 = scrub next render)

  bool skipLoopDelay() override { return controller.skipLoopDelay(); }

  void buildAndShuffleSession(FlashcardDeck::SessionScope scope);
  // Start a review session in `scope` (persists the choice as the new default,
  // then builds + shuffles). No-op if the chosen scope yields no cards.
  void startSession(FlashcardDeck::SessionScope scope);
  bool loadCurrentCard();  // fills `card` from session[cursor]; false if exhausted
  void gradeAndAdvance(bool correctRecall);
  // Advance to the next session card (or the summary if exhausted). Shared by
  // grading and the suspend/unsuspend flows; performs no grading or re-queue.
  void advanceCard();
  // Browse the session by `delta` (-1 prev, +1 next), clamped to the session
  // bounds, without mutating any card. Used by the suspended-review pass so the
  // user can page through set-aside cards before deciding to resume one.
  void navigateCard(int delta);
  // Abandon the in-progress session and return to the deck overview (the flashcard
  // "home" page). Resets the session + tallies and recomputes deck stats. Used by
  // Back from a card face, so the user lands on the overview instead of the reader.
  void returnToOverview();
  // Prompt to suspend (active session) or unsuspend (suspendedMode) the resident
  // card via ConfirmationActivity; on confirm, mutate the deck and advance.
  void promptSuspendToggle();
  // Suspended-review only: confirm + permanently delete the current set-aside card.
  void promptDelete();
  void displayList();  // push with the list refresh policy (FAST + landscape scrub)

  // Render helpers.
  void renderOverview(int contentTop, int contentBottom, int pageWidth);
  void renderFront(int contentTop, int contentBottom, int pageWidth);
  void renderRevealed(int contentTop, int contentBottom, int pageWidth);
  void renderAwaitingGrade(int contentTop, int contentBottom, int pageWidth);
  void renderSummary(int contentTop, int contentBottom, int pageWidth);
  // The card face (bold word header + underlined-in-context excerpt + chapter
  // footer) is rendered by the shared FlashcardCardFace::render helper, reused by
  // the deck-browser detail view. This activity passes its resident `card` fields.
  // Draw the side-button suspend/unsuspend clue at the top of the content area --
  // the side buttons are not part of the front-button hints row, so they need an
  // explicit on-screen label. Text reflects suspendedMode. The suspend clue ("prev"
  // / PageBack button) and, when showNextHint is true (cloze reveal), the "Next"
  // clue ("next" / PageForward button) are placed at the physical position of the
  // side button that triggers them, folding in the Side Button Layout + CW swap (via
  // usesUpButton) and the per-device button geometry: X3 side-by-side along the top
  // (Up=top-left, Down=top-right); X4 stacked on the right (Up=top-right,
  // Down=bottom-right -- hence contentBottom is needed).
  void drawSuspendHint(int contentTop, int contentBottom, bool showNextHint = false);
};
