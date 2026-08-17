#pragma once
#include <I18n.h>

#include <cstdint>
#include <string>

#include "CrossPointSettings.h"
#include "SdCardFontSystem.h"
#include "activities/Activity.h"
#include "util/Dictionary.h"
#include "util/DictionaryRegistry.h"

namespace DictUtils {

// Human-readable name for a dictionary base path. Paths are <root>/<folder>/<stem>
// (e.g. /.dictionaries/dict-en-en/dict-data) and <folder> is what the user recognises,
// so pull that component out. Falls back to the whole path when it has no folder
// component, and to the "None" label when empty.
inline std::string dictDisplayName(const std::string& basePath) {
  if (basePath.empty()) return tr(STR_DICT_NONE);
  const size_t lastSlash = basePath.rfind('/');
  if (lastSlash == std::string::npos || lastSlash == 0) return basePath;
  const size_t prevSlash = basePath.rfind('/', lastSlash - 1);
  if (prevSlash == std::string::npos) return basePath.substr(0, lastSlash);
  return basePath.substr(prevSlash + 1, lastSlash - prevSlash - 1);
}

// "Same as book" resolves the definition font to the reader's FAMILY at the DICTIONARY's
// size, so an SD family has to be resident at that size before getDefinitionFontId() can
// return it — otherwise the resolver falls back to the nearest resident size, which is the
// reader's, and the text renders at the book's size. Every screen that draws
// definition-font text has to do this for itself: DictionaryDefinitionActivity::onEnter()
// does, and so must the flashcard faces (FlashcardCardFace::render also resolves through
// getDefinitionFontId()).
//
// Cheap to call repeatedly: a no-op once the size is resident. Must be re-called on
// onResume(), because a sub-activity's releaseDefinitionFont() drops the size again.
inline void ensureDefinitionFontResident(GfxRenderer& renderer) {
  if (SETTINGS.dictionaryFontFamily != CrossPointSettings::DICT_FONT_MATCH_READER) return;
  sdFontSystem.ensureFontSize(SETTINGS.getReaderSdFontFamilyName(), SETTINGS.getDefinitionPointSize(), renderer);
}

// Undo of the above, for onExit(). An extra .cpfont holds session-lifetime per-style tables
// mid-heap, and these activities run with the reader still allocated below them — see
// SdCardFontSystem::releaseExtraSizes(), which keeps the reader-size font and the CJK UI
// fallback sizes, so this cannot strip whatever screen is resumed underneath.
inline void releaseDefinitionFont(GfxRenderer& renderer) { sdFontSystem.releaseExtraSizes(renderer); }

// Stable identity of the dictionary a lookup would currently resolve through, for recording on
// a flashcard. Reads activeDictPath(), so a session override (the long-press dictionary switch)
// is captured rather than only the configured selection. 0 when no dictionary is set.
//
// The path is <root>/<folder>/<stem>; only <folder> is hashed, because that is the component
// the user copies between devices — see DictionaryRegistry::nameHash.
inline uint32_t activeDictHash(const char* cachePath) {
  const std::string basePath = Dictionary::activeDictPath(cachePath);
  if (basePath.empty()) return 0;
  const size_t lastSlash = basePath.rfind('/');
  if (lastSlash == std::string::npos || lastSlash == 0) return 0;
  const size_t prevSlash = basePath.rfind('/', lastSlash - 1);
  if (prevSlash == std::string::npos) return 0;
  return DictionaryRegistry::nameHash(basePath.substr(prevSlash + 1, lastSlash - prevSlash - 1).c_str());
}

// Point subsequent lookups at the dictionary a flashcard was saved from. Returns false — falling
// back to the configured dictionary — when the card records none, or when the recorded one is not
// installed here (deleted, renamed, or the card came from a device with a different set). That
// fallback is deliberate and silent: showing the definition from some other dictionary is a far
// better outcome than refusing to show one.
//
// The failure paths CLEAR the override rather than leaving it alone. These screens call this once
// per card, so "leave it" would mean an unassociated card inherits whatever the previously viewed
// card set, and the definition you get for a legacy card would depend on review order. Clearing
// is also what the host's SessionOverrideScope does on exit, so the two agree.
//
// THREADING: setSessionDictPath is UI-task-only and must not race a running lookup
// (Dictionary.h:94-99) — call this immediately before startLookup(), never from render().
// Callers must own a Dictionary::SessionOverrideScope so the override cannot outlive the screen.
inline bool applyCardDict(uint32_t dictHash) {
  const int idx = dictHash != 0 ? dictionaryRegistry.indexOfHash(dictHash) : -1;
  if (idx < 0) {
    Dictionary::setSessionDictPath("");
    return false;
  }
  // basePath, not name: despite its parameter being called folderPath, setSessionDictPath stores
  // exactly what activeDictPath() returns, and that is consumed as a base path (Dictionary.cpp:669
  // feeds it to buildPath). The existing long-press switch passes basePath for the same reason
  // (DictionaryDefinitionActivity.cpp:1084).
  Dictionary::setSessionDictPath(dictionaryRegistry.getEntries()[idx].basePath.c_str());
  return true;
}

// D-006: Back-cancel pattern — sets isCancelled=true and finishes the activity.
inline void cancelAndFinish(Activity& act) {
  ActivityResult r;
  r.isCancelled = true;
  act.setResult(std::move(r));
  act.finish();
}

}  // namespace DictUtils
