#pragma once
#include <I18n.h>

#include <string>

#include "CrossPointSettings.h"
#include "SdCardFontSystem.h"
#include "activities/Activity.h"

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

// D-006: Back-cancel pattern — sets isCancelled=true and finishes the activity.
inline void cancelAndFinish(Activity& act) {
  ActivityResult r;
  r.isCancelled = true;
  act.setResult(std::move(r));
  act.finish();
}

}  // namespace DictUtils
