#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../Activity.h"
#include "activities/ListTouchTarget.h"
#include "util/ButtonNavigator.h"
#include "util/DictNotes.h"
#include "util/DictionaryLookupController.h"

// The text kept from dictionary definitions: which dictionaries hold notes, one dictionary's
// notes, and one note in full. Sibling of LookedUpWordsActivity, which lists the words looked up
// in one book; notes are scoped to the DICTIONARY instead, so the same screen answers from any
// book (DictNotes).
//
// Three modes in one activity rather than three screens: the paging, the windowed SD reads, the
// landscape scrub cadence and the delete confirmation are shared, and the note reader would
// otherwise need its own copy of the note the list already has the index of.
//
// The dictionary picker exists because a dictionary switch inside a definition is SESSION-scoped
// (DictionaryDefinitionActivity restores the entry override in onExit), so notes kept in the
// switched-to dictionary would be unreachable from a screen that only ever showed the active
// one. It is skipped when only one dictionary has notes, which is the common case.
//
// Selecting a note LOOKS ITS WORD UP in the dictionary it belongs to and opens the definition,
// where the note's text is drawn highlighted (DictionaryDefinitionActivity::computeNoteSpans) --
// a note is a mark on a definition, so the definition is what it should open. The plain text
// reader below survives only as the fallback for a note whose word can no longer be resolved.
//
// Notes are paged from SD, never materialized: only the on-screen window lives in RAM, and its
// rows hold a TEASER rather than the note in full -- a screen that runs with the reader still
// resident cannot afford 16 notes at their full on-disk length. Opening one re-reads just that
// note.
class DictNotesActivity final : public Activity {
 public:
  // dictHash/dictName name the PREFERRED dictionary -- the picker opens with the cursor on it
  // when it has notes. Callers pass whichever dictionary is active for them, so opening this
  // from the reader still lands on the book's dictionary in one press.
  // bookCachePath is the book this was opened from, empty when opened from Settings: it only
  // decides what a definition opened from here can do BESIDES showing the word (record history,
  // offer a flashcard, walk a chain back), all of which are guarded on it being non-empty.
  explicit DictNotesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, uint32_t dictHash,
                             std::string dictName, std::string bookCachePath = "")
      : Activity("DictNotes", renderer, mappedInput),
        cachePath_(std::move(bookCachePath)),
        preferredHash_(dictHash),
        dictName_(std::move(dictName)),
        controller_(renderer, mappedInput, *this, cachePath_) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Mode : uint8_t { Dictionaries, List, Note };

  // One row of the picker: a dictionary that has notes.
  struct DictRow {
    uint32_t hash = 0;
    uint16_t count = 0;
    std::string name;  // registry name, or the hash in hex when that dictionary is not installed
  };

  // Read the dictionaries that have notes, resolve their names and sort by name. Called on entry
  // and again whenever a delete may have emptied one.
  void loadDictionaries();
  // Open the notes of the picker row at `index`.
  void enterDictionary(int index);
  // Leave the current mode the way the user entered it.
  void goBack();

  // Refresh totalCount from SD and invalidate the cached window.
  void refreshCount();
  // Note at a newest-first UI index, paging the window in if needed. nullptr when out of range.
  const DictNotes::Note* entryAt(int uiIndex);
  // Push the framebuffer with the list refresh policy (fast in portrait; fast + a periodic HALF
  // scrub in landscape, where repeated FAST accumulates DC bias -- see LookedUpWordsActivity).
  void displayList();
  // Look the selected note's word up in this dictionary, so its definition opens with the note
  // marked on it. Falls back to openNoteText() when the dictionary is not installed here.
  void openSelected();
  // Show the selected note's text on its own. The fallback path only: a note whose dictionary is
  // gone, or whose word the dictionary no longer answers, must still be readable.
  void openNoteText();
  // Confirm, then delete the selected note. A pushed ConfirmationActivity rather than an inline
  // confirm mode: the inline ones are answered with Confirm/Back, which a touch-only board does
  // not have.
  void promptDelete();

  void renderDictionaries();
  void renderList();
  void renderNote();

  // The book that was open, "" when there is none. Declared before controller_, which binds it.
  std::string cachePath_;

  Mode mode_ = Mode::Dictionaries;
  // True when entry skipped the picker because only one dictionary had notes, so Back from the
  // list leaves the screen instead of opening a picker the user never saw.
  bool pickerSkipped_ = false;

  uint32_t preferredHash_ = 0;
  uint32_t dictHash_ = 0;  // the dictionary whose notes the list and reader are showing
  std::string dictName_;

  std::vector<DictRow> dicts_;
  int dictIndex_ = 0;  // picker cursor, kept across a visit to one dictionary's notes

  int totalCount = 0;
  // >= the most rows a page can show. Teasers, so the window is ~1 KB rather than the ~4 KB the
  // same count of full notes would cost.
  static constexpr int WINDOW_CAP = 16;
  static constexpr size_t TEASER_MAX = 72;
  DictNotes::Note window[WINDOW_CAP];
  int windowStart = -1;  // newest-first index of window[0]; -1 = invalid
  int windowLen = 0;
  int selectedIndex = 0;
  int pagesUntilFullRefresh = 0;  // landscape ghost-scrub cadence counter (0 = scrub next render)

  // Reader mode: the one note currently open, wrapped to the content width.
  std::string viewWord_;
  std::vector<std::string> wrappedLines_;
  int pageOffset_ = 0;
  int lineHeight_ = 0;
  int linesPerPage_ = 1;

  // Clears the session dictionary override on destruction, so the dictionary a note installed to
  // open its definition cannot outlive this screen -- activities are heap-allocated and deleted
  // on exit, so the destructor always runs. Same member LookedUpWordsActivity holds.
  Dictionary::SessionOverrideScope dictOverrideScope_;
  DictionaryLookupController controller_;
  ButtonNavigator buttonNavigator;
  // Rows the last render drew, so a tap can pick one (see ListTouchTarget).
  ListTouchTarget listTouch_;

  bool skipLoopDelay() override { return controller_.skipLoopDelay(); }
};
