#pragma once

#include <cstddef>
#include <cstdint>

#include "DictLayout.h"
#include "Dictionary.h"

// A bounded "peek" at a dictionary entry: enough of it to fill a small on-page gloss box, and
// never more. The reader's word-select overlay repeats this on every cursor move, so it cannot
// use the full definition path — DictionaryLookupController spawns a task, may draw a popup,
// then walks stems -> alt-form prompt -> suggestions, and its caller enrolls a flashcard
// (DictionaryWordSelectActivity.cpp:497-531). Per keypress that is an SD write per keypress.
// Nothing here writes, allocates a task, or falls back past the exact word.
//
// Deliberately split in two. readEntry() does the SD work and hands back raw bytes; fit()
// measures and wraps them. The caller MUST prewarm the font from those raw bytes BETWEEN the
// two calls: a codepoint that reaches SdCardFont's per-glyph fallback costs ~50 ms, and one
// CJK gloss is 30-60 glyphs, so measuring before prewarming turns a ~0.5 s frame into ~3 s.
// A single peek+fit call would hide that ordering constraint and it would rot.
//
// No Arduino APIs in this file: src/util is compiled by the host test suite, so millis() would
// break test/dict-gloss. Timing instrumentation lives in the calling activity for that reason.
namespace DictGloss {

// Bytes of one entry the peek ever reads. This cap is what bounds the per-move cost
// independent of entry length — a StarDict entry can be tens of KB while three rows need a few
// hundred bytes. 512 covers more than three rows in either script (~170 CJK characters, ~90
// ASCII words), so the wrap always has enough to fill the box.
constexpr size_t kPeekBytes = 512;

struct GlossResult {
  static constexpr int kMaxRows = 3;
  // Widest row this can be asked to hold: the X3's 792 px landscape width less margins, at the
  // smallest reader font (~16 px per CJK character, ~7 px per ASCII one) is ~46 CJK characters
  // (138 bytes) or ~105 ASCII ones. 160 clears both with margin. Rows are fixed char arrays
  // rather than std::string because this struct is refilled on every cursor move, on the
  // heap that WordSelectNavigator.h:28-63 documents as the tightest in the firmware.
  static constexpr size_t kRowBytes = 160;

  char rows[kMaxRows][kRowBytes] = {};
  // The one bold run on each row, as a byte offset and length into rows[i] — the bracketed
  // reading (see findReading), which sits after the headword rather than at the start, so an
  // offset is needed as well as a length. Two bytes per row rather than a per-segment style list,
  // which would cost a vector per row on the tightest heap in the firmware for a case that cannot
  // arise: a plain-text entry carries exactly one styled run. boldLen == 0 means the whole row is
  // regular. Both fit a uint8_t because kRowBytes does.
  uint8_t boldStart[kMaxRows] = {};
  uint8_t boldLen[kMaxRows] = {};
  int rowCount = 0;
  // The entry had more rows, or a wider row, than fitted. The last row carries an ellipsis.
  bool truncated = false;
  // A row was produced. False for a miss, so the caller can say so rather than drawing blank.
  bool found = false;

  // Cheaper than assigning a default-constructed temporary: this struct is ~490 bytes, well
  // past the 256-byte budget CLAUDE.md sets for locals.
  void reset() {
    for (int i = 0; i < kMaxRows; i++) {
      rows[i][0] = '\0';
      boldStart[i] = 0;
      boldLen[i] = 0;
    }
    rowCount = 0;
    truncated = false;
    found = false;
  }
};

// Byte range of the entry's pronunciation, brackets included, or found=false when it has none.
struct ReadingSpan {
  size_t start = 0;
  size_t len = 0;
  bool found = false;
};

// The reading in a plain-text ('m') Chinese entry is the first bracketed run on its FIRST line —
// "拼音 [pin1 yin1] /pinyin/" — so that is what this looks for, brackets included, because the
// brackets are what make the run legible as a reading once it is bold.
//
// Deliberately narrow. An earlier version took "everything before the first Han character",
// which is structural and notation-independent but wrong on the real card: it swallowed the
// headword and any Latin definition text ahead of the first Han character. The bracket is the
// actual marker.
//
// A run is rejected — leaving the row unbolded — when the '[' is further in than
// kMaxReadingOffset, when no ']' follows within kMaxReadingBytes, when the brackets are empty, or
// when a second '[' arrives first. Every failure mode is "no bold", never "the wrong text
// bolded". Scanning by byte is safe: '[' and ']' are ASCII, and no UTF-8 continuation byte can
// equal them.
//
// The first-line limit only bites on the RAW entry: fit() collapses newlines to spaces before it
// wraps, so it makes the check twice (see the note there).
ReadingSpan findReading(const char* s);

// Probe `token` and read up to bufSize-1 bytes of its entry into buf, NUL-terminated.
// Returns the byte count; 0 when the word is absent or the read failed. No stemming, no alt
// forms, no fuzzy suggestions — a miss is a miss, because every fallback is another SD probe
// on a path that runs per keypress.
//
// The returned buffer is what the caller hands to ensureSdCardFontReady/prewarmCache before
// calling fit() (see the ordering note above), which is why it is NUL-terminated.
//
// A read cut short by bufSize is trimmed back to a whole UTF-8 codepoint, so a split sequence
// can never reach the renderer — a bad codepoint draws as U+FFFD, which these fonts render as
// a bare '?'.
//
// dictFile must be an already-open handle on ctx's ".dict". Opening it once per session is the
// point: a per-move open is the SD round trip this whole path exists to avoid.
//
// Note on raw content: a StarDict entry whose .ifo omits sametypesequence carries a one-byte
// field-type prefix. That byte is NOT stripped here, matching
// DictionaryDefinitionActivity::wrapPlain (which also reads raw from the offset), so the gloss
// and the full definition screen always show the same thing.
size_t readEntry(Dictionary::LookupCtx& ctx, HalFile& dictFile, const char* token, char* buf, size_t bufSize);

// Wrap NUL-terminated buf into at most kMaxRows rows of metrics.maxWidth pixels.
//
// buf is MODIFIED IN PLACE — control bytes become spaces and whitespace runs are collapsed —
// so no copy of the entry is ever made. Pass the buffer readEntry filled and treat it as
// consumed afterwards.
//
// Wrapping goes through DictLayout::Wrapper rather than a local loop because its breakToken
// (DictLayout.cpp:65-91) breaks per codepoint when a token overflows, which is the only reason
// a CJK run — one token with no spaces in it — wraps at all instead of running off the row.
void fit(char* buf, const DictLayout::WrapMetrics& metrics, const DictLayout::Measurer& measure, GlossResult& out);

}  // namespace DictGloss
