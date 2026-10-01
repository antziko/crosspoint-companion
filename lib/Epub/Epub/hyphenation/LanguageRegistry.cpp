#include "LanguageRegistry.h"

#include <algorithm>

#include "HyphenationCommon.h"

// Which pattern sets are built. scripts/select_hyphenation.py turns the
// `custom_hyphenation_langs` option into CP_HYPH_<TAG>=0 for the languages left out; an
// undefined tag means built, so this file still compiles all of them on its own. The
// patterns are the largest block of data after the fonts -- German alone is 206 KB of the
// 351 KB total -- and a language left out only loses its line breaking.
//
// English is unconditional: it is the fallback, and an empty registry would leave
// getLanguageEntries() with nothing to return.
#ifndef CP_HYPH_DE
#define CP_HYPH_DE 1
#endif
#ifndef CP_HYPH_ES
#define CP_HYPH_ES 1
#endif
#ifndef CP_HYPH_FI
#define CP_HYPH_FI 1
#endif
#ifndef CP_HYPH_FR
#define CP_HYPH_FR 1
#endif
#ifndef CP_HYPH_IT
#define CP_HYPH_IT 1
#endif
#ifndef CP_HYPH_PL
#define CP_HYPH_PL 1
#endif
#ifndef CP_HYPH_PT
#define CP_HYPH_PT 1
#endif
#ifndef CP_HYPH_RU
#define CP_HYPH_RU 1
#endif
#ifndef CP_HYPH_SV
#define CP_HYPH_SV 1
#endif
#ifndef CP_HYPH_UK
#define CP_HYPH_UK 1
#endif

#include "generated/hyph-en.trie.h"
#if CP_HYPH_DE
#include "generated/hyph-de.trie.h"
#endif
#if CP_HYPH_ES
#include "generated/hyph-es.trie.h"
#endif
#if CP_HYPH_FI
#include "generated/hyph-fi.trie.h"
#endif
#if CP_HYPH_FR
#include "generated/hyph-fr.trie.h"
#endif
#if CP_HYPH_IT
#include "generated/hyph-it.trie.h"
#endif
#if CP_HYPH_PL
#include "generated/hyph-pl.trie.h"
#endif
#if CP_HYPH_PT
#include "generated/hyph-pt.trie.h"
#endif
#if CP_HYPH_RU
#include "generated/hyph-ru.trie.h"
#endif
#if CP_HYPH_SV
#include "generated/hyph-sv.trie.h"
#endif
#if CP_HYPH_UK
#include "generated/hyph-uk.trie.h"
#endif

namespace {

// English hyphenation patterns (3/3 minimum prefix/suffix length)
LanguageHyphenator englishHyphenator(en_patterns, isLatinLetter, toLowerLatin, 3, 3);
#if CP_HYPH_FR
LanguageHyphenator frenchHyphenator(fr_patterns, isLatinLetter, toLowerLatin);
#endif
#if CP_HYPH_DE
LanguageHyphenator germanHyphenator(de_patterns, isLatinLetter, toLowerLatin);
#endif
#if CP_HYPH_RU
LanguageHyphenator russianHyphenator(ru_patterns, isCyrillicLetter, toLowerCyrillic);
#endif
#if CP_HYPH_ES
LanguageHyphenator spanishHyphenator(es_patterns, isLatinLetter, toLowerLatin);
#endif
#if CP_HYPH_IT
LanguageHyphenator italianHyphenator(it_patterns, isLatinLetter, toLowerLatin);
#endif
#if CP_HYPH_SV
LanguageHyphenator swedishHyphenator(sv_patterns, isLatinLetter, toLowerLatin);
#endif
#if CP_HYPH_UK
LanguageHyphenator ukrainianHyphenator(uk_patterns, isCyrillicLetter, toLowerCyrillic);
#endif
#if CP_HYPH_PL
LanguageHyphenator polishHyphenator(pl_patterns, isLatinLetter, toLowerLatin);
#endif
#if CP_HYPH_FI
LanguageHyphenator finnishHyphenator(fi_patterns, isLatinLetter, toLowerLatin);
#endif
#if CP_HYPH_PT
LanguageHyphenator portugueseHyphenator(pt_patterns, isLatinLetter, toLowerLatin);
#endif

// Sized by the rows that survive the guards above, so leaving a language out is one
// edit rather than two.
const LanguageEntry kEntries[] = {
    {"english", "en", &englishHyphenator},
#if CP_HYPH_FR
    {"french", "fr", &frenchHyphenator},
#endif
#if CP_HYPH_DE
    {"german", "de", &germanHyphenator},
#endif
#if CP_HYPH_RU
    {"russian", "ru", &russianHyphenator},
#endif
#if CP_HYPH_ES
    {"spanish", "es", &spanishHyphenator},
#endif
#if CP_HYPH_IT
    {"italian", "it", &italianHyphenator},
#endif
#if CP_HYPH_PL
    {"polish", "pl", &polishHyphenator},
#endif
#if CP_HYPH_SV
    {"swedish", "sv", &swedishHyphenator},
#endif
#if CP_HYPH_UK
    {"ukrainian", "uk", &ukrainianHyphenator},
#endif
#if CP_HYPH_FI
    {"finnish", "fi", &finnishHyphenator},
#endif
#if CP_HYPH_PT
    {"portuguese", "pt", &portugueseHyphenator},
#endif
};

}  // namespace

const LanguageHyphenator* getLanguageHyphenatorForPrimaryTag(const std::string& primaryTag) {
  const auto* const last = std::end(kEntries);
  const auto* const it = std::find_if(
      std::begin(kEntries), last, [&primaryTag](const LanguageEntry& entry) { return primaryTag == entry.primaryTag; });
  return (it != last) ? it->hyphenator : nullptr;
}

LanguageEntryView getLanguageEntries() { return LanguageEntryView{kEntries, std::size(kEntries)}; }
