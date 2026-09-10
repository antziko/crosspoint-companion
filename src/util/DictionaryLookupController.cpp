#include "DictionaryLookupController.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>
#include <Logging.h>
#include <SdDebugLog.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "../activities/Activity.h"
#include "../activities/reader/DictionarySuggestionsActivity.h"
#include "CrossPointSettings.h"
#include "DictLookupTask.h"
#include "MappedInputManager.h"
#include "Memory.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/Dictionary.h"
#include "util/DictionaryRegistry.h"

DictionaryLookupController::DictionaryLookupController(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                       Activity& owner, std::string cachePath)
    : renderer(renderer), mappedInput(mappedInput), owner(owner), cachePath(std::move(cachePath)) {}

DictionaryLookupController::~DictionaryLookupController() = default;

// One SD line at every terminal exit of a lookup. Until now nothing in this controller was
// timed at all, so "the dictionary feels slower" was unanswerable from a device dump: every
// duration in the log came from DictionaryDefinitionActivity, and those are font and e-ink
// panel time (display alone is ~3.2s on open, ~438ms per page). Reading DICT: lookup beside
// DDA: render separates index cost from panel cost instead of leaving it to inference.
//
// To SD rather than serial-only because the sessions worth diagnosing are untethered. One
// line per lookup is the same order of traffic as the DDA: prewarm / wrap / render lines
// already written per definition open.
void DictionaryLookupController::logLookupOutcome(const char* outcome) const {
  SdDebugLog::log("DICT", "lookup '%s' %s %lums free=%u largest=%u", lookupWord.c_str(), outcome,
                  static_cast<unsigned long>(millis() - lookupStartMs_), static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
}

void DictionaryLookupController::startLookup(const std::string& word, bool recordHistory) {
  lookupStartMs_ = millis();
  lookupWord = word;
  foundWord.clear();
  foundLocation = DictLocation{};
  // Per-lookup, not sticky: without this reset a later genuine miss would still be
  // reported with the previous lookup's "No dictionary set" / "Dictionary unreadable".
  notFoundMsg_ = StrId::STR_DICT_NOT_FOUND;
  lookupProgress = 0;
  lookupDone = false;
  lookupCancelled = false;
  lookupCancelRequested = false;
  recordHistory_ = recordHistory;
  historyWrite_ = {};
  // Per-word dictionary selection, BEFORE takeFallbackPromotion() below — the ordering is
  // load-bearing. The hook may call setSessionDictPath, which clears the promotion mark
  // (Dictionary.h:111), so running it first means an explicit per-word dictionary simply
  // outranks an automatic promotion and there is nothing left to suspend. Running it after
  // would leave setNotFound() re-promoting the fallback OVER the word's own dictionary.
  //
  // Safe against the setSessionDictPath threading rule: this is the UI task, and the lookup
  // task is not created until further down this function.
  if (preLookupFn_) preLookupFn_(preLookupCtx_, word);
  // A fallback promotion is scoped to the entry it answered: this lookup runs against the
  // dictionary the user actually configured (or explicitly switched to), not against
  // whatever a previous miss happened to land on. Held, not discarded — if this lookup
  // fails, the promoted entry stays on screen and setNotFound() puts its promotion back.
  suspendedPromotion_ = Dictionary::takeFallbackPromotion();
  fallbackDictPath_.clear();
  fallbackHops_ = 0;
  stemWord_.clear();
  state = LookupState::LookingUp;
  // CLEANUP: on Auto-only commit, delete only this line (gate below stays — it's the Auto check)
  const bool showPopup = shouldShowPopup();
  // Why the "Looking up" toast did or didn't appear: it is Auto-gated on the .cspt index
  // size (see shouldShowPopup), so a normally-indexed dictionary suppresses it on purpose.
  LOG_DBG("DICT", "startLookup: cspt entries=%u popup=%d free=%u", csptEntryCountCached, showPopup ? 1 : 0,
          static_cast<unsigned>(ESP.getFreeHeap()));
  // Task BEFORE the toast, deliberately. The toast's panel refresh is ~437ms of which almost all
  // is waiting on the panel's BUSY line, and the lookup is 500-800ms of SD reads on its own
  // FreeRTOS task — so drawing first made them serial for no reason. Started first, they overlap
  // and the toast becomes very nearly free. progressCallback() only stores an int and explicitly
  // does not requestUpdate(), so nothing the task does can race the draw below.
  //
  // (The SPI contention this might look like is not one: the framebuffer push is short and the
  // rest of the refresh is a BUSY wait with the bus idle, which is when the SD reads happen.)
  task = makeUniqueNoThrow<DictLookupTask>(*this);
  if (!task) {
    LOG_ERR("DICT", "OOM: DictLookupTask");
    showMemoryErrorAndReset();
    return;
  }
  task->start("DictLookup", 4096, 1);

  if (showPopup) {
    // Toast overlay: draw popup directly over whatever the user is currently viewing.
    // RenderLock serializes against the render task — without it, a prior requestUpdate()
    // (e.g. from navigation) may still be mid-refresh, and concurrent framebuffer / SPI
    // access from two tasks crashes the e-ink driver.
    RenderLock lock;
    // No displayBuffer() after this: BaseTheme::drawPopup already ends with one
    // (BaseTheme.cpp:803) and no theme overrides it, so a second call here was a second
    // full-panel FAST refresh of pixels the panel had just been given.
    GUI.drawPopup(renderer, tr(STR_DICT_LOOKING_UP));
    // This box is why the next full paint has to scrub instead of taking a plain differential.
    // Setting the flag HERE rather than in DictionaryDefinitionActivity::onEnter ties the cost to
    // its cause: when the toast is suppressed the definition's first paint is an ordinary FAST
    // refresh (~437ms) instead of a scrub (~730ms). The flag is one-shot and consumed by the next
    // displayBuffer, which is whichever screen replaces this one — the definition, the
    // "not found" popup, or the word-select repaint after a cancel. All three need the box gone.
    renderer.forceCleanRefreshNextPaint();
  }
}

void DictionaryLookupController::startLookupAsSuggestion(const std::string& word) {
  nextIsSuggestion = true;
  startLookup(word);
}

void DictionaryLookupController::restoreSuspendedPromotion() {
  if (suspendedPromotion_.empty()) return;
  Dictionary::promoteFallbackDictPath(suspendedPromotion_.c_str());
  suspendedPromotion_.clear();
}

void DictionaryLookupController::setNotFound() {
  // Nothing new reaches the screen, so the definition still on it is the promoted
  // dictionary's. Put that promotion back, or the header would name one dictionary while
  // the body shows another's entry — the disagreement the promotion exists to prevent.
  restoreSuspendedPromotion();
  state = LookupState::NotFound;
  // Only a genuine miss toasts; a fault waits for a press. notFoundMsg_ is set by the
  // caller before this runs, so it is the authority on which of the two this is.
  notFoundIsToast_ = (notFoundMsg_ == StrId::STR_DICT_NOT_FOUND);
  notFoundShownMs_ = millis();
  owner.requestUpdate();
}

void DictionaryLookupController::onExit() {
  if (task) {
    task->stop();
    // Task::wait() is an unbounded `while (handle) vTaskDelay(1)` (Task.h:31-33). Because it
    // YIELDS, the idle task keeps feeding the watchdog, so a lookup that never returns hangs the
    // UI silently and forever — no panic, no reboot, no crash report. That is the shape of the
    // stuck-needs-power-cycle report: opds_debug.txt ends at `MEM: enter DictionaryWordSelect`
    // and the next boot goes Boot -> Reader with no `enter Crash`.
    //
    // Not bounding the wait: on timeout the task is still live and would write lookupDone /
    // foundLocation into a destroyed controller. A use-after-free is worse than a hang. So log
    // the entry instead — if the hang recurs, this line is the last one written and names the
    // wait as the place it stopped, which is exactly what the capture was missing.
    if (task->isRunning()) {
      SdDebugLog::log("DICT", "onExit: joining lookup task for '%s'", lookupWord.c_str());
    }
    task->wait();
    task.reset();
  }
}

DictionaryLookupController::LookupEvent DictionaryLookupController::handleInput() {
  if (state == LookupState::LookingUp) {
    if (lookupDone) {
      state = LookupState::Idle;
      task.reset();

      if (lookupCancelled) {
        nextIsSuggestion = false;
        restoreSuspendedPromotion();  // same reason as setNotFound(): the old entry stays on screen
        logLookupOutcome("cancelled");
        return LookupEvent::Cancelled;
      }

      if (foundLocation.found) {
        // stemWord_ is set only when the exact word missed and a stem variant answered — see
        // runLookup(). A suggestion the user picked from the list keeps its own status either
        // way: they chose that spelling, so reporting it as a stem would be wrong.
        const bool viaStem = !stemWord_.empty();
        foundWord = viaStem ? stemWord_ : lookupWord;
        foundStatus = nextIsSuggestion ? FoundStatus::Suggestion : (viaStem ? FoundStatus::Stem : FoundStatus::Direct);
        nextIsSuggestion = false;
        // A same-category fallback answered: promote that dictionary while its entry is on
        // screen. The definition itself streams from foundLocation.folderPath either way, but
        // the header name, the long-press cycle origin and the dictionary a flashcard records
        // each re-read activeDictPath() independently — without this they would all name the
        // configured dictionary while showing another one's entry.
        //
        // Scoped to THIS entry, not to the reading session: the next startLookup takes it back
        // so a search the user starts afterwards runs against the dictionary they configured.
        // A long-press switch is the opposite — an explicit choice, and it persists.
        //
        // UI task, and the lookup task is already joined by the task.reset() above: that is the
        // no-lookup-in-flight invariant the session-path setters require (Dictionary.h).
        if (!fallbackDictPath_.empty()) Dictionary::promoteFallbackDictPath(fallbackDictPath_.c_str());
        // A promotion suspended for this lookup belonged to the entry just replaced; the one
        // above (or no promotion at all) stands from here.
        suspendedPromotion_.clear();
        logLookupOutcome(!fallbackDictPath_.empty() ? "fallback" : (viaStem ? "stem" : "direct"));
        return LookupEvent::FoundDefinition;
      }

      // No usable dictionary: the search never ran, so stems, alt forms and fuzzy
      // suggestions would all re-fail against the same missing files. Say what is actually
      // wrong instead of "Not found", which reads as "you spelled it wrong".
      if (foundLocation.status == LookupStatus::NoDictionary || foundLocation.status == LookupStatus::ReadError) {
        notFoundMsg_ = foundLocation.status == LookupStatus::NoDictionary ? StrId::STR_DICT_NO_DICT_SET
                                                                          : StrId::STR_DICT_UNREADABLE;
        nextIsSuggestion = false;
        logLookupOutcome(foundLocation.status == LookupStatus::NoDictionary ? "nodict" : "unreadable");
        setNotFound();
        return LookupEvent::None;
      }

      // Stem variants ran on the lookup task, before the group sweep — see runLookup().

      // Try alt forms
      if (Dictionary::hasAltForms(cachePath.c_str())) {
        altFormWord = lookupWord;
        state = LookupState::AltFormPrompt;
        // The machine search stops here and waits for the user. Log what it cost so far;
        // the resume below re-stamps the clock so the user's think-time is never counted.
        logLookupOutcome("altprompt");
        owner.requestUpdate();
        return LookupEvent::None;
      }

      handleLookupFailed();
      return LookupEvent::None;
    }

    // Task still running — check for cancel
    if (!lookupCancelRequested && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      lookupCancelRequested = true;
      owner.requestUpdate();
    }
    return LookupEvent::None;
  }

  if (state == LookupState::AltFormPrompt) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      state = LookupState::Idle;
      // Restart the clock: everything between "altprompt" and here was the user deciding,
      // and folding that into a duration would make the number useless for diagnosing speed.
      lookupStartMs_ = millis();
      std::string canonical = Dictionary::resolveAltForm(altFormWord, cachePath.c_str());
      if (!canonical.empty()) {
        auto loc = Dictionary::locate(canonical, {}, cachePath.c_str());
        if (loc.found) {
          foundWord = canonical;
          foundLocation = std::move(loc);
          foundStatus = nextIsSuggestion ? FoundStatus::Suggestion : FoundStatus::AltForm;
          nextIsSuggestion = false;
          logLookupOutcome("altform");
          return LookupEvent::FoundDefinition;
        }
      }
      handleLookupFailed();
      return LookupEvent::None;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = LookupState::Idle;
      nextIsSuggestion = false;
      return LookupEvent::Cancelled;
    }
    return LookupEvent::None;
  }

  if (state == LookupState::NotFound) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      state = LookupState::Idle;
      return LookupEvent::NotFoundDismissedDone;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = LookupState::Idle;
      return LookupEvent::NotFoundDismissedBack;
    }
    // Toast expiry. Reported as a Back dismissal, which every caller reads as "stay where
    // you are and repaint" -- the reader did not ask to leave, so Done would be wrong. The
    // dismissal must still travel as an event: callers do real cleanup on it (the definition
    // screen reverts a failed dictionary switch, word-select forces a full repaint).
    if (notFoundIsToast_ && (millis() - notFoundShownMs_) >= NOT_FOUND_TOAST_MS) {
      state = LookupState::Idle;
      return LookupEvent::NotFoundDismissedBack;
    }
    return LookupEvent::None;
  }

  return LookupEvent::None;
}

bool DictionaryLookupController::render() {
  const auto& metrics = UITheme::getInstance().getMetrics();

  if (state == LookupState::LookingUp) {
    // Popup is drawn inline as a toast in startLookup(); nothing to do from the render task.
    // Returning false lets the activity's normal render run (e.g. on cancel, the page repaints
    // which naturally wipes the toast overlay).
    return false;
  }

  if (state == LookupState::AltFormPrompt) {
    const int pageWidth = renderer.getScreenWidth();
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                   tr(STR_DICT_SEARCH_ALT_FORMS));
    const int y =
        metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + renderer.getLineHeight(UI_10_FONT_ID);
    renderer.drawCenteredText(UI_10_FONT_ID, y, altFormWord.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONFIRM), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return true;
  }

  if (state == LookupState::NotFound) {
    GUI.drawPopup(renderer, I18n::getInstance().get(notFoundMsg_));
    // A toast dismisses itself, so hints promising Back/Done would be noise on a popup
    // that is gone before they can be read. A fault still needs them.
    if (!notFoundIsToast_) {
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DONE), "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return true;
  }

  return false;
}

bool DictionaryLookupController::handleMultiSelect(WordSelectNavigator& navigator) {
  std::string msPhrase;
  const auto msAction = navigator.handleMultiSelectInput(mappedInput, msPhrase);
  if (msAction == WordSelectNavigator::MultiSelectAction::None) return false;
  switch (msAction) {
    case WordSelectNavigator::MultiSelectAction::PhraseReady:
      lookupOrPopup(msPhrase);
      return true;
    case WordSelectNavigator::MultiSelectAction::ExitedMultiSelect:
    case WordSelectNavigator::MultiSelectAction::EnteredMultiSelect:
      owner.requestUpdate();
      return true;
    default:
      return true;
  }
}

bool DictionaryLookupController::handleConfirmLookup(const WordSelectNavigator& navigator) {
  if (!mappedInput.wasReleased(MappedInputManager::Button::Confirm)) return false;
  lookupSelected(navigator);
  return true;  // consumed the input even if nothing was selected
}

void DictionaryLookupController::lookupSelected(const WordSelectNavigator& navigator) {
  const auto* sel = navigator.getSelected();
  if (!sel) return;
  lookupOrPopup(navigator.getLookup(*sel));
}

void DictionaryLookupController::lookupOrPopup(const std::string& rawWord) {
  std::string cleaned = Dictionary::cleanWord(rawWord);
  if (cleaned.empty()) {
    showNoWordPopup();
  } else {
    startLookup(cleaned);
  }
}

void DictionaryLookupController::showMemoryErrorAndReset() {
  {
    RenderLock lock;
    GUI.drawPopup(renderer, tr(STR_MEMORY_ERROR));  // refreshes internally — see startLookup()
  }
  vTaskDelay(1000 / portTICK_PERIOD_MS);
  state = LookupState::Idle;
  owner.requestUpdate();
}

void DictionaryLookupController::showNoWordPopup() {
  {
    // Serialize with render task — see comment in startLookup() for the race this prevents.
    RenderLock lock;
    GUI.drawPopup(renderer, tr(STR_DICT_NO_WORD));  // refreshes internally — see startLookup()
  }
  vTaskDelay(1000 / portTICK_PERIOD_MS);
  owner.requestUpdate();
}

void DictionaryLookupController::handleLookupFailed() {
  auto similar = Dictionary::findSimilar(lookupWord, 6, cachePath.c_str());
  // Logged after findSimilar, not before: its PAGE_RADIUS sweep is part of what a miss
  // costs, and a miss is the expensive case — it is the only one that runs the widened
  // retry in locateIn AND this sweep. If dictionary lookups ever get slower, this is the
  // line that will show it.
  logLookupOutcome(similar.empty() ? "miss" : "miss+sug");
  if (!similar.empty()) {
    auto sugActivity = makeUniqueNoThrow<DictionarySuggestionsActivity>(renderer, mappedInput, std::move(similar));
    if (!sugActivity) {
      LOG_ERR("DICT", "OOM: DictionarySuggestionsActivity");
      nextIsSuggestion = false;
      showMemoryErrorAndReset();
      return;
    }
    owner.startActivityForResult(std::move(sugActivity), [this](const ActivityResult& result) {
      if (result.isCancelled) {
        setNotFound();
        return;
      }
      const auto& wr = std::get<WordResult>(result.data);
      startLookupAsSuggestion(wr.word);
    });
    return;
  }
  nextIsSuggestion = false;
  setNotFound();
  // Record after setNotFound() so the popup's requestUpdate() has kicked the render task —
  // the SD write below overlaps the e-ink refresh on the main task.
  // Captured, not discarded: this write moves every position in the log, and the owner's
  // back-navigation chain addresses the log by position (see takeHistoryWrite).
  historyWrite_ = LookupHistory::addWordIf(cachePath, lookupWord, LookupHistory::Status::NotFound, recordHistory_);
}

void DictionaryLookupController::progressCallback(void* ctx, int percent) {
  auto* self = static_cast<DictionaryLookupController*>(ctx);
  self->lookupProgress = percent;
  // Intentionally no requestUpdate() here — popup is a single static frame.
}

bool DictionaryLookupController::cancelCallback(void* ctx) {
  return static_cast<DictionaryLookupController*>(ctx)->lookupCancelRequested;
}

// Try the exact word in each OTHER dictionary of the active one's category before giving up.
// Category is the folder-name partition in DictionaryRegistry::nameIsStGroup: an "st-" dictionary
// only ever falls back to another "st-" one. Exact match only — stems, alt forms and suggestions
// stay with the active dictionary, which keeps every hop on this task (cancellable, with the
// toast already on glass) and never multiplies the user-blocking alt-form prompt.
DictLocation DictionaryLookupController::sweepGroup(Dictionary::LookupCtx& ctx, const std::string& activeBase,
                                                    const DictLocation& primary, const DictLookupCallbacks& cbs) {
  if (!SETTINGS.dictFallbackGroup) return primary;
  // Nothing configured at all has no group to sweep, and "No dictionary set" is the honest
  // message — silently answering out of some other dictionary would hide the real problem.
  if (primary.status == LookupStatus::NoDictionary) return primary;
  if (dictionaryRegistry.count() < 2) return primary;

  // Reading entries_ from the lookup task needs no lock: discover() runs at boot (main.cpp) and
  // from DictionarySelectActivity, and refreshIfDirty() only from SettingsActivity::onEnter —
  // none of which can run while a reader lookup is in flight.
  const int startIdx = dictionaryRegistry.indexOf(activeBase);
  // Not in the registry (dictionary.bin empty, or pointing at a folder discover() skipped as
  // ambiguous): it has no group to match, and nextIndexInGroup would fall back to index 0 — the
  // one path that crosses the partition. Decline, exactly as the long-press switch does.
  if (startIdx < 0) return primary;

  const auto& entries = dictionaryRegistry.getEntries();
  const uint32_t sweepStartMs = millis();
  int idx = startIdx;
  int hops = 0;
  int skipped = 0;
  while (true) {
    if (cbs.shouldCancel && cbs.shouldCancel(cbs.ctx)) return primary;
    idx = dictionaryRegistry.nextEntryIndexInGroup(idx);
    if (idx < 0 || idx == startIdx) break;  // sole member of its group, or wrapped back to the start
    hops++;
    // Reuses the caller's ctx: openLookupCtxAt resets it first, so the previous hop's handles are
    // released before this one opens and only ever one dictionary is open at a time.
    if (!Dictionary::openLookupCtxAt(ctx, entries[idx].basePath.c_str())) continue;
    // openLookupCtxAt tries .idx.oft.cspt then .idx.oft, so no page index here proves neither
    // sidecar exists — and without one, locateIn's bounds stay at 0..idxFileSize
    // (Dictionary.cpp: resolveScanBoundsIn returns immediately) and it scans the entire index.
    // That measured 7.5-10.2s per lookup on a ~1.4MB .idx, on the critical path of every miss,
    // and logged nothing because the widened-retry line only fires when widening widens.
    //
    // A fallback hop is opportunistic, so decline it rather than pay that. The active
    // dictionary is deliberately NOT protected this way: scanning the one the user configured
    // is correct behaviour. The fix for the skipped dictionary is Prepare, which builds both
    // sidecars — hence naming it in the summary below.
    if (!ctx.hasPageIndex) {
      skipped++;
      continue;
    }
    const uint32_t hopStartMs = millis();
    DictLocation loc = Dictionary::locateIn(ctx, lookupWord, cbs);
    const uint32_t hopMs = millis() - hopStartMs;
    // Only slow hops, so this stays quiet in normal use. An indexed dictionary answers a probe
    // in tens of milliseconds; anything near a second means that dictionary is the reason a
    // lookup felt slow, and naming it is what turns "the dictionary is slow" into one line.
    if (hopMs >= SLOW_HOP_LOG_MS) {
      SdDebugLog::log("DICT", "sweep: slow hop %s %lums", entries[idx].name.c_str(), static_cast<unsigned long>(hopMs));
    }
    if (!loc.found) continue;
    fallbackDictPath_ = entries[idx].basePath;
    fallbackHops_ = hops;
    // To SD as well as serial: which dictionary answered, and whether it stayed in the right
    // group, is only diagnosable from a device session — same reason the long-press logs here.
    SdDebugLog::log("DICT", "fallback: %s(%s) -> %s(%s) after %d, %lums", entries[startIdx].name.c_str(),
                    entries[startIdx].nameIsSt ? "st" : "other", entries[idx].name.c_str(),
                    entries[idx].nameIsSt ? "st" : "other", hops, static_cast<unsigned long>(millis() - sweepStartMs));
    return loc;
  }
  // The sweep is the expensive half of a missed lookup and until now said nothing when it
  // failed. skipped>0 is the actionable part: those dictionaries need Prepare run on them.
  SdDebugLog::log("DICT", "sweep: miss hops=%d skipped=%d %lums", hops, skipped,
                  static_cast<unsigned long>(millis() - sweepStartMs));
  return primary;
}

void DictionaryLookupController::runLookup() {
  DictLookupCallbacks cbs;
  cbs.ctx = this;
  cbs.onProgress = &DictionaryLookupController::progressCallback;
  cbs.shouldCancel = &DictionaryLookupController::cancelCallback;

  // Resolved once and handed to sweepGroup for its registry lookup: activeDictPath() reads
  // dictionary.bin off SD whenever no session override is set, so re-deriving it on the miss
  // path would cost a second open.
  const std::string activeBase = Dictionary::activeDictPath(cachePath.c_str());

  // Stem probes get cancellation but not progress: onProgress would drive the bar back to 70
  // on every extra probe, so a word with six variants would appear to restart six times.
  DictLookupCallbacks probeCbs;
  probeCbs.ctx = this;
  probeCbs.shouldCancel = &DictionaryLookupController::cancelCallback;

  // One ctx for the active dictionary and every fallback hop — see sweepGroup.
  Dictionary::LookupCtx ctx;
  if (Dictionary::openLookupCtxAt(ctx, activeBase.c_str())) {
    foundLocation = Dictionary::locateIn(ctx, lookupWord, cbs);

    // Stems BEFORE the group sweep, and against the ctx that is already open.
    //
    // The other order cost 8.5-11.1s per inflected word on device: "tarps" missed in all six
    // dictionaries of the group — including one with no page index, whose locateIn degrades to
    // a full linear scan — and only then found "tarp" on the first stem probe here, a probe the
    // log prices at ~440ms. Trying the user's own dictionary's stems first answers those words
    // in under a second.
    //
    // It also changes which answer wins: a stem hit in the configured dictionary now outranks
    // an exact hit in a sibling. That is the better answer as well as the faster one — the
    // sibling is a fallback, not a peer.
    //
    // Reusing ctx means zero extra SD opens (the old UI-task loop opened its own), and running
    // here rather than in handleInput() makes the probes cancellable and stops them blocking
    // the UI task.
    if (!foundLocation.found && !lookupCancelRequested) {
      for (const auto& stem : Dictionary::getStemVariants(lookupWord)) {
        DictLocation loc = Dictionary::locateIn(ctx, stem, probeCbs);
        if (!loc.found) continue;
        stemWord_ = stem;
        foundLocation = std::move(loc);
        break;
      }
    }
  } else {
    foundLocation = DictLocation{};
    // base is filled only when the dictionary resolved but its .idx would not open, which is what
    // separates "nothing configured" from "configured but broken" for the popup wording above.
    foundLocation.status = ctx.base[0] == '\0' ? LookupStatus::NoDictionary : LookupStatus::ReadError;
    foundLocation.folderPath = ctx.base;
  }
  if (!foundLocation.found) foundLocation = sweepGroup(ctx, activeBase, foundLocation, cbs);

  lookupCancelled = lookupCancelRequested;
  lookupDone = true;
  // Don't call requestUpdate(true) here - it triggers an unnecessary e-ink refresh
  // of the word select activity before transitioning to the definition activity.
  // The main loop polls lookupDone every ~10ms, so response time is still fast.
}

bool DictionaryLookupController::shouldShowPopup() {
  if (csptEntryCountCached == UINT32_MAX) {
    csptEntryCountCached = Dictionary::readCsptEntryCount(cachePath.c_str());
  }
  // Two independent costs, and the popup has to cover both because nothing repaints the panel
  // between here and the definition's first displayBuffer (DictionaryDefinitionActivity.cpp:1147)
  // — e-ink holds the last frame, so this toast stays on glass through the whole open.
  //
  // 1. Lookup: the .cspt entry count predicts how long Dictionary::locate() scans. 0 means no
  //    optimized index at all (full scan).
  // 2. Render: an SD-card definition font. Built-ins decompress into a RAM cache and never pay
  //    per-glyph SD I/O, which is why prewarmDefinitionFont() returns early for them
  //    (DictionaryDefinitionActivity.cpp:352). The prewarm scan, the extra style loads and the
  //    glyph-miss path exist only on the SD path. The .cspt count cannot see any of that, so a
  //    well-indexed dictionary used to suppress the toast and then render for seconds against a
  //    frozen word-select page.
  //
  //    This term is deliberately KEPT even though that render is now ~950ms rather than the
  //    ~4470ms it was: the toast is the only signal that a lookup fired at all, and a lookup that
  //    silently does nothing for a second and a half reads as a dropped keypress. Its cost is
  //    what changed — startLookup() now starts the lookup task BEFORE drawing it, so the toast's
  //    panel refresh overlaps the SD scan instead of preceding it.
  //
  // Not also testing whether the definition is markup: markup costs extra only when it forces
  // extra font styles, and extra styles cost real time only on the SD path already covered here.
  // It would buy no discrimination and cost a second Dictionary::readInfo() SD read per lookup.
  return csptEntryCountCached == 0 || csptEntryCountCached > AUTO_POPUP_CSPT_ENTRY_THRESHOLD ||
         renderer.isSdCardFont(SETTINGS.getDefinitionFontId());
}
