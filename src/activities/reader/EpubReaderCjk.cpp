// Book-language, furigana and (later) vertical-text glue for EpubReaderActivity, ported from
// matcha-reader. Function names match matcha's EpubReaderActivity so its fixes can be carried
// across by name; see docs/matcha-sync.md.
#include <DictIndex.h>
#include <Epub/Kinsoku.h>
#include <Epub/Page.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Print.h>
#include <Utf8.h>
#include <esp_heap_caps.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "EpubReaderActivity.h"
#include "WordSelectionScan.h"

namespace {
// Print sink for the language sniff: feeds the sample and stops the stream once it has enough.
class SniffSink : public Print {
 public:
  cjk::ScriptSniff sniff;
  size_t fed = 0;
  // A Latin chapter never reaches enough(): cap what one item costs instead of inflating it whole.
  static constexpr size_t kMaxBytesPerItem = 24 * 1024;
  size_t write(const uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t* buffer, const size_t size) override {
    if (sniff.enough() || fed >= kMaxBytesPerItem) return 0;  // a short write ends the stream (allowEarlyStop)
    sniff.feed(buffer, size);
    fed += size;
    return size;
  }
};
}  // namespace

void EpubReaderActivity::loadLanguageChoice() {
  if (!epub) return;
  cjkPrefs.load(epub->getCachePath());
}

void EpubReaderActivity::refreshBookScript() {
  effectiveLanguage_ = epub ? cjkPrefs.effectiveLanguage(epub->getLanguage()) : std::string();
  bookScript_ = cjk::scriptForLanguage(effectiveLanguage_);
  // Traditional Chinese centres its vertical punctuation; Japanese and Simplified sit it in the
  // corner. Process-wide, so settled here before any vertical layout runs.
  Kinsoku::setCentredPunctuation(bookScript_ == CjkScript::TraditionalChinese);
  // The converted /dictionaries/<folder> word-select segments with (also process-wide).
  DictIndex::setLanguageFolder(cjk::dictFolderFor(effectiveLanguage_, bookScript_));
}

void EpubReaderActivity::sniffLanguageIfNeeded() {
  if (!epub || cjkPrefs.languageOverride != cjk::LANG_AUTO || cjkPrefs.detectedLanguage != 0) return;
  // The tag is enough -- unless it is Chinese without a script, where it is only a default.
  const std::string& tag = epub->getLanguage();
  if (cjk::scriptForLanguage(tag) != CjkScript::None && !cjk::isUnscriptedChinese(tag)) return;
  const int spineCount = epub->getSpineItemsCount();
  if (currentSpineIndex < 0 || currentSpineIndex >= spineCount) return;
  // Up to three items are read, each judged on its own: an English copyright page ahead of the
  // first chapter must not outvote the chapter, and a cover page with no text decides nothing.
  // Nothing is written down when no item could be judged, so the next open looks again.
  auto sink = makeUniqueNoThrow<SniffSink>();
  if (!sink) return;
  constexpr uint32_t kTextEnoughToJudge = 300;  // letters and CJK characters
  CjkScript found = CjkScript::None;
  int itemsWithText = 0;
  int latinItems = 0;
  for (int spine = currentSpineIndex; spine < spineCount && spine < currentSpineIndex + 3; spine++) {
    sink->sniff = cjk::ScriptSniff{};
    sink->fed = 0;
    epub->readItemContentsToStream(epub->getSpineItem(spine).href, *sink, 1024, /*allowEarlyStop=*/true);
    const cjk::ScriptSniff& sn = sink->sniff;
    found = sn.verdict();
    if (found != CjkScript::None) break;
    if (sn.han + sn.kana + sn.latin >= kTextEnoughToJudge) {
      itemsWithText++;
      latinItems++;
    }
  }
  const bool judged = found != CjkScript::None || (latinItems > 0 && latinItems == itemsWithText);
  switch (found) {
    case CjkScript::Japanese:
      cjkPrefs.detectedLanguage = cjk::LANG_JA;
      break;
    case CjkScript::SimplifiedChinese:
      cjkPrefs.detectedLanguage = cjk::LANG_ZH_HANS;
      break;
    case CjkScript::TraditionalChinese:
      cjkPrefs.detectedLanguage = cjk::LANG_ZH_HANT;
      break;
    default:
      cjkPrefs.detectedLanguage = CjkBookPrefs::SNIFFED_NONE;
      break;
  }
  LOG_INF("ERS", "Language sniff: tag '%s' -> %u%s", tag.c_str(), cjkPrefs.detectedLanguage,
          judged ? "" : " (not saved)");
  if (judged) cjkPrefs.save(epub->getCachePath());
}

ReaderRenderSpec EpubReaderActivity::readerSpec(const uint16_t viewportWidth, const uint16_t viewportHeight) const {
  ReaderRenderSpec spec = SETTINGS.readerRenderSpec(viewportWidth, viewportHeight);
  spec.furiganaEnabled = useFurigana();
  return spec;
}

void EpubReaderActivity::applyCjkPrefsChange(const bool languageChanged) {
  if (!epub) return;
  cjkPrefs.save(epub->getCachePath());
  {
    RenderLock lock(*this);
    rememberPositionForReflow();
    resetSections();
    // Vertical caches bake in the language's punctuation and are keyed by font and viewport
    // only, so a re-tagged book must lay out afresh.
    if (languageChanged) Storage.removeDir((epub->getCachePath() + "/vsections").c_str());
    refreshBookScript();
    // A book re-tagged as Latin loses the Vertical Text and Furigana rows.
    panelIndex = std::min(panelIndex, textPanelRowCount() - 1);
  }
  // The stored page is the old layout; renderBook() redraws the page with the panel on top.
  discardOverlayPage();
  requestUpdate();
}

std::unique_ptr<WordSelectionScan> EpubReaderActivity::newWordScan() const {
  if (!epub || !isCjkBook() || !DictIndex::isAvailable()) return nullptr;
  // The scan's glyph list (~8 KB on a dense page) plus the dictionary caches (~30 KB, in small
  // blocks) live only while word-select is up. Below this the page keeps per-character selection.
  constexpr size_t kMinFree = 40 * 1024;
  constexpr size_t kMinBlock = 12 * 1024;
  const size_t freeHeap = ESP.getFreeHeap();
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (freeHeap < kMinFree || largest < kMinBlock) {
    LOG_INF("ERS", "Word scan skipped: free=%u largest=%u", static_cast<unsigned>(freeHeap),
            static_cast<unsigned>(largest));
    return nullptr;
  }
  auto scan = makeUniqueNoThrow<WordSelectionScan>();
  if (!scan) LOG_ERR("ERS", "OOM: WordSelectionScan");
  return scan;
}

std::string EpubReaderActivity::wordScanCachePath() const { return epub->getCachePath() + "/wlscan.bin"; }

std::string EpubReaderActivity::wordScanLookupTail(const Page& next) {
  auto isAsciiWord = [](const unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
  };
  std::string tail;
  int taken = 0;
  for (const auto& el : next.elements) {
    if (taken >= WordSelectionScan::kLookupContextChars) break;
    if (el->getTag() != TAG_PageLine) continue;
    const auto& line = static_cast<const PageLine&>(*el);
    if (!line.getBlock()) continue;
    const TextBlock& block = *line.getBlock();
    for (uint16_t wi = 0; wi < block.wordCount() && taken < WordSelectionScan::kLookupContextChars; wi++) {
      const char* w = block.wordText(wi);
      const size_t len = block.wordTextLen(wi);
      if (len == 0) continue;
      if (!tail.empty() && isAsciiWord(static_cast<unsigned char>(tail.back())) &&
          isAsciiWord(static_cast<unsigned char>(w[0]))) {
        tail += ' ';
        taken++;
      }
      // wordText is NUL-terminated, so the decoder stops at the word's end.
      const auto* p = reinterpret_cast<const unsigned char*>(w);
      while (taken < WordSelectionScan::kLookupContextChars) {
        const uint32_t cp = utf8NextCodepoint(&p);
        if (cp == 0) break;
        utf8AppendCodepoint(cp, tail);
        taken++;
      }
    }
  }
  return tail;
}
