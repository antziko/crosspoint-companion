#pragma once
#include <I18n.h>  // StrId, for the NotFound popup's message

#include <cstdint>
#include <memory>
#include <string>

#include "Dictionary.h"
#include "LookupHistory.h"
#include "WordSelectNavigator.h"

class Activity;
class DictLookupTask;
class GfxRenderer;
class MappedInputManager;

// Shared controller for dictionary lookup flow used by DictionaryWordSelectActivity
// and DictionaryDefinitionActivity.  Owns the background lookup task, the
// stems/alt-form fallback chain, and all overlay rendering (looking-up popup,
// alt-form prompt, not-found popup).  The calling activity delegates input and
// render to this class whenever isActive() returns true.
class DictionaryLookupController {
  friend class DictLookupTask;

 public:
  enum class LookupState { Idle, LookingUp, AltFormPrompt, NotFound };
  enum class LookupEvent { None, FoundDefinition, NotFoundDismissedBack, NotFoundDismissedDone, Cancelled };

  // How the word was ultimately resolved when FoundDefinition fires.
  enum class FoundStatus { Direct, Stem, AltForm, Suggestion };

  // Convert FoundStatus to LookupHistory::Status for history recording.
  static LookupHistory::Status toHistStatus(FoundStatus fs) {
    switch (fs) {
      case FoundStatus::Direct:
        return LookupHistory::Status::Direct;
      case FoundStatus::Stem:
        return LookupHistory::Status::Stem;
      case FoundStatus::AltForm:
        return LookupHistory::Status::AltForm;
      case FoundStatus::Suggestion:
        return LookupHistory::Status::Suggestion;
      default:
        return LookupHistory::Status::NotFound;
    }
  }

  DictionaryLookupController(GfxRenderer& renderer, MappedInputManager& mappedInput, Activity& owner,
                             std::string cachePath = "");
  ~DictionaryLookupController();

  // Start a lookup.  Transitions Idle → LookingUp, spawns background task.
  // If recordHistory is true (default), adds the word to lookup history on success.
  void startLookup(const std::string& word, bool recordHistory = true);

  // Like startLookup but marks the result as Suggestion (word came from fuzzy suggestions list).
  void startLookupAsSuggestion(const std::string& word);

  // Run `fn(ctx, cleanedWord)` at the top of every startLookup, before any dictionary is
  // resolved. Lets an owner point the lookup at a per-word dictionary — the reader's word
  // select uses it to honour the dictionary a word's flashcard records. Installed per owner
  // rather than done here because the definition screen shares this class and must NOT get
  // the behaviour: a card override would fight its own long-press dictionary switch, and on
  // a chained word it would repoint the ORIGINAL word's card.
  //
  // The cleaned word is the one the deck is keyed on, which is why the hook lives here and
  // not at the call sites: cleanWord() runs inside lookupOrPopup(), out of the owner's sight.
  //
  // Plain function pointer + ctx, never std::function: ~2-4 KB per signature plus a
  // heap-allocated closure (CLAUDE.md).
  void setPreLookupHook(void (*fn)(void*, const std::string&), void* ctx) {
    preLookupFn_ = fn;
    preLookupCtx_ = ctx;
  }

  // Called by the activity after the suggestions path has been exhausted.
  // Transitions to NotFound state.
  void setNotFound();

  // Must be called from the activity's onExit() to kill any running task.
  void onExit();

  // True when the controller owns input/render (LookingUp, AltFormPrompt, NotFound).
  bool isActive() const { return state != LookupState::Idle; }

  // Process input for the current state.  Returns an event the activity must handle.
  LookupEvent handleInput();

  // Draw the appropriate overlay and call displayBuffer.  Returns true when fully
  // handled (activity must return immediately from render()).
  bool render();

  // Inform the activity's skipLoopDelay() override.
  bool skipLoopDelay() const { return state == LookupState::LookingUp; }

  // Show the "no word" popup with a 1-second delay, then request update.
  void showNoWordPopup();

  // Clean the word and start lookup; shows no-word popup if cleaning yields empty.
  void lookupOrPopup(const std::string& rawWord);

  // Handle multi-select input from the navigator. Returns true if input was consumed
  // (caller should return from loop). Cleans the phrase and starts lookup or shows popup.
  bool handleMultiSelect(WordSelectNavigator& navigator);

  // Handle single-word confirm lookup from the navigator. Returns true if input was consumed
  // (caller should return from loop). Gets the selected word and starts lookup or shows popup.
  bool handleConfirmLookup(const WordSelectNavigator& navigator);

  const std::string& getLookupWord() const { return lookupWord; }
  const std::string& getFoundWord() const { return foundWord; }
  const DictLocation& getFoundLocation() const { return foundLocation; }
  FoundStatus getFoundStatus() const { return foundStatus; }
  bool getRecordHistory() const { return recordHistory_; }

  // The history write handleLookupFailed() performed, if any, consumed once. A recorded miss grows
  // the log without any navigation happening, so an owner that addresses the log by position
  // (DictionaryDefinitionActivity's LookupChain) has to re-index exactly once when it does.
  // Also cleared by startLookup, so a stale write can never be claimed by the next lookup.
  LookupHistory::WriteResult takeHistoryWrite() {
    const LookupHistory::WriteResult result = historyWrite_;
    historyWrite_ = {};
    return result;
  }

 private:
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;
  Activity& owner;
  std::string cachePath;

  // See setPreLookupHook. Null on every owner but the reader's word select.
  void (*preLookupFn_)(void*, const std::string&) = nullptr;
  void* preLookupCtx_ = nullptr;

  LookupState state = LookupState::Idle;
  FoundStatus foundStatus = FoundStatus::Direct;
  bool nextIsSuggestion = false;
  bool recordHistory_ = true;

  std::string lookupWord;
  std::string foundWord;
  DictLocation foundLocation;
  std::string altFormWord;

  // Set by the sweep in runLookup() when a dictionary OTHER than the active one answered;
  // empty otherwise. Written on the lookup task and read on the UI task, published by the same
  // lookupDone barrier that already publishes foundLocation. handleInput() promotes it to the
  // session dictionary — see the comment there for why that has to happen on the UI task.
  std::string fallbackDictPath_;
  int fallbackHops_ = 0;  // dictionaries tried past the active one, for the SD log

  // The stem variant that answered when the exact word missed; empty when the exact word hit
  // (or nothing did). Written on the lookup task and published by the same lookupDone barrier
  // that already publishes foundLocation and fallbackDictPath_.
  std::string stemWord_;

  // A sweep hop slower than this gets named in the SD log. Above any indexed dictionary's probe
  // and below the multi-second stall an unindexed one produces, so it only fires on real
  // trouble.
  static constexpr uint32_t SLOW_HOP_LOG_MS = 500;

  // A fallback promotion this lookup suspended so it could start from the configured
  // dictionary (see startLookup). Re-installed if the lookup produces no new definition,
  // because the entry it belongs to is then still the one on screen. Empty otherwise.
  std::string suspendedPromotion_;

  // Set by handleLookupFailed() when it records the miss in history; consumed by takeHistoryWrite().
  LookupHistory::WriteResult historyWrite_;

  // What the NotFound popup says. A genuine miss reads "Not found"; an unset or unreadable
  // dictionary says so instead, rather than sending the user hunting for a typo.
  StrId notFoundMsg_ = StrId::STR_DICT_NOT_FOUND;

  // A genuine miss auto-dismisses as a toast: the word simply is not in the dictionary,
  // there is nothing to decide, and making the reader press Back for that on every miss
  // is the whole cost of the feature. The two dictionary FAULTS keep the press-to-dismiss
  // popup -- they mean every later lookup will fail too, so they must not flash past.
  static constexpr uint32_t NOT_FOUND_TOAST_MS = 1200;
  bool notFoundIsToast_ = false;  // notFoundMsg_ is a miss, not a fault
  uint32_t notFoundShownMs_ = 0;  // millis() when the popup went up

  // CLEANUP: on Auto-only commit, delete only this line (threshold/cache/method below drive Auto mode — keep)
  static constexpr uint32_t AUTO_POPUP_CSPT_ENTRY_THRESHOLD = 50000;
  uint32_t csptEntryCountCached = UINT32_MAX;  // sentinel: not yet read
  bool shouldShowPopup();

  volatile int lookupProgress = 0;
  volatile bool lookupDone = false;
  volatile bool lookupCancelled = false;
  volatile bool lookupCancelRequested = false;

  std::unique_ptr<DictLookupTask> task;

  // millis() at the start of the current *machine* search, for logLookupOutcome below.
  // Re-stamped when a user prompt resumes the search so no logged duration ever contains
  // the user's think-time — see the alt-form resume in handleInput().
  uint32_t lookupStartMs_ = 0;
  void logLookupOutcome(const char* outcome) const;

  void runLookup();
  // The same-category sweep runLookup() delegates to once the active dictionary has missed.
  // Returns the winning location, or `primary` untouched when nothing in the group has the word.
  DictLocation sweepGroup(Dictionary::LookupCtx& ctx, const std::string& activeBase, const DictLocation& primary,
                          const DictLookupCallbacks& cbs);
  void handleLookupFailed();
  // Put back a promotion suspended by startLookup. No-op when none is held.
  void restoreSuspendedPromotion();
  void showMemoryErrorAndReset();
  static void progressCallback(void* ctx, int percent);
  static bool cancelCallback(void* ctx);
};
