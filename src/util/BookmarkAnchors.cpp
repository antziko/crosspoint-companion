#include "BookmarkAnchors.h"

#include <ChapterXPathResolver.h>
#include <DocFragmentPath.h>
#include <Epub/Section.h>
#include <Logging.h>
#include <Memory.h>
#include <ProgressMapper.h>
#include <SdDebugLog.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

#include "BookmarkStore.h"

namespace {
// Point bookmarks resolved per chapter pass. Bounds the temporary std::string array: the
// whole frame is has[128] + targets[2*8] + srcIdx[8] + out[8 headers] ~= 350 bytes, which
// suits the main loop task this runs on. A chapter with more pending marks takes another
// pass over the same stream rather than a bigger frame.
constexpr size_t kMaxBatch = 8;

// Quotes resolved per chapter pass. Lower than kMaxBatch because each one carries its
// quoted text as a needle (up to QUOTE_PREVIEW_MAX) plus two resolved anchors, so the heap
// this holds at once is ~4 * (512 + 2 * BOOKMARK_XPATH_MAX) rather than a few headers.
constexpr size_t kMaxQuoteBatch = 4;
static_assert(kMaxQuoteBatch <= ChapterXPathResolver::kMaxTextNeedles, "quote batch exceeds the locator's capacity");

// srcIdx below stores a bookmark index in a uint8_t to keep the frame small.
static_assert(MAX_BOOKMARKS <= 256, "bookmark indices no longer fit the uint8_t back-reference");

// A foreign mark's merge key, snapshotted so it can be re-found after a re-sort.
struct ForeignKey {
  bool quote;
  uint16_t spine;
  uint16_t para;
  uint16_t startWord;
  uint16_t endWord;
  float progress;
};

// Point bookmarks: one paragraph index each, resolved to one anchor.
int anchorPoints(const std::shared_ptr<Epub>& epub, const BookmarkStore& store, const bool* has, const size_t from,
                 const size_t to, const uint16_t spine) {
  const auto& bms = store.getBookmarks();
  int anchored = 0;
  size_t cursor = from;
  while (cursor < to) {
    uint16_t targets[kMaxBatch];
    uint8_t srcIdx[kMaxBatch];
    size_t k = 0;
    while (cursor < to && k < kMaxBatch) {
      const size_t j = cursor++;
      // Return marks are device-only; quotes are handled by anchorQuotes below.
      if (has[j] || bms[j].returnMark || bms[j].quote) continue;
      // Without a paragraph index there is nothing to match on here; anchorPointsByProgress
      // takes those, using the same progress value the device itself navigates by.
      if (bms[j].paragraphIndex == UINT16_MAX) continue;
      targets[k] = bms[j].paragraphIndex;
      srcIdx[k] = static_cast<uint8_t>(j);
      k++;
    }
    if (k == 0) continue;

    std::string resolved[kMaxBatch];
    ChapterXPathResolver::findXPathsForParagraphs(epub, spine, targets, resolved, k);
    for (size_t t = 0; t < k; t++) {
      // Only write what is missing: the sidecar is append-only, so re-anchoring an
      // already-anchored bookmark would grow the file on every sync.
      if (resolved[t].empty()) continue;
      if (store.setXPath(bms[srcIdx[t]], resolved[t])) anchored++;
    }
  }
  return anchored;
}

// Point bookmarks whose paragraph index was never recorded -- the section cache had no
// paragraph LUT when they were made, so Section::getParagraphIndexForPage returned nothing.
// Their progress is still exact, and is what the device uses to place them, so it anchors
// them here too. The chapter length is counted once for the whole chapter rather than per
// bookmark, which is what findXPathForProgress would cost.
int anchorPointsByProgress(const std::shared_ptr<Epub>& epub, const BookmarkStore& store, const bool* has,
                           const size_t from, const size_t to, const uint16_t spine) {
  const auto& bms = store.getBookmarks();
  size_t pending = 0;
  for (size_t j = from; j < to; j++) {
    if (has[j] || bms[j].returnMark || bms[j].quote) continue;
    if (bms[j].paragraphIndex == UINT16_MAX) pending++;
  }
  if (pending == 0) return 0;

  const bool singleSpine = epub->getSpineItemsCount() == 1;
  int anchored = 0;
  const size_t totalVisibleChars = ChapterXPathResolver::countVisibleChars(epub, spine);
  if (totalVisibleChars == 0) {
    LOG_DBG("BKA", "spine %u: no visible text, %u progress-anchored mark(s) skipped", spine,
            static_cast<unsigned>(pending));
    return 0;
  }

  size_t cursor = from;
  while (cursor < to) {
    uint32_t offsets[kMaxBatch];
    bool inclusive[kMaxBatch];
    uint8_t srcIdx[kMaxBatch];
    size_t k = 0;
    while (cursor < to && k < kMaxBatch) {
      const size_t j = cursor++;
      if (has[j] || bms[j].returnMark || bms[j].quote) continue;
      if (bms[j].paragraphIndex != UINT16_MAX) continue;
      if (!(bms[j].progress > 0.0f)) {
        // The chapter start has no offset to resolve into; name the fragment itself.
        if (store.setXPath(bms[j], DocFragmentPath::body(spine, singleSpine))) anchored++;
        continue;
      }
      offsets[k] = ChapterXPathResolver::offsetForProgress(bms[j].progress, totalVisibleChars);
      // Progress names a character rather than a gap, so the target resolves inside the
      // text node holding it.
      inclusive[k] = true;
      srcIdx[k] = static_cast<uint8_t>(j);
      k++;
    }
    if (k == 0) continue;

    std::string resolved[kMaxBatch];
    ChapterXPathResolver::findXPathsForOffsets(epub, spine, offsets, resolved, k, inclusive);
    for (size_t t = 0; t < k; t++) {
      if (resolved[t].empty()) {
        LOG_DBG("BKA", "spine %u: progress %.3f did not resolve", spine, static_cast<double>(bms[srcIdx[t]].progress));
        SdDebugLog::log("BKA", "spine %u: progress %.3f did not resolve", spine,
                        static_cast<double>(bms[srcIdx[t]].progress));
        continue;
      }
      if (store.setXPath(bms[srcIdx[t]], resolved[t])) anchored++;
    }
  }
  return anchored;
}

// Quotes: the stored text located in the chapter, then both of its ends resolved. Two
// passes over the spine item, because the second needs offsets the first produces.
int anchorQuotes(const std::shared_ptr<Epub>& epub, const BookmarkStore& store, const bool* has, const size_t from,
                 const size_t to, const uint16_t spine) {
  const auto& bms = store.getBookmarks();
  int anchored = 0;
  size_t cursor = from;
  while (cursor < to) {
    std::string needles[kMaxQuoteBatch];
    uint8_t srcIdx[kMaxQuoteBatch];
    // Whether the needle is the whole quote. A snippet at the cap may be a prefix, which
    // locates the start correctly but says nothing about where the quote ends.
    bool wholeQuote[kMaxQuoteBatch];
    size_t k = 0;
    while (cursor < to && k < kMaxQuoteBatch) {
      const size_t j = cursor++;
      if (has[j] || !bms[j].quote) continue;
      // The quoted text is the only layout-independent thing a quote stores; without it
      // there is nothing to search the chapter for.
      if (store.readPreviewAt(j, needles[k]) && !needles[k].empty()) {
        wholeQuote[k] = true;
      } else {
        // Quotes made before the preview sidecar existed have no .qtext entry, but the
        // resident teaser in the .bin was always written. It is the same text, capped.
        needles[k] = bms[j].snippet;
        if (needles[k].empty()) {
          LOG_DBG("BKA", "spine %u: quote has neither preview nor snippet", spine);
          SdDebugLog::log("BKA", "spine %u: quote has neither preview nor snippet", spine);
          continue;
        }
        wholeQuote[k] = needles[k].size() < BOOKMARK_SNIPPET_MAX - 1;
      }
      srcIdx[k] = static_cast<uint8_t>(j);
      k++;
    }
    if (k == 0) continue;

    ChapterXPathResolver::TextRange ranges[kMaxQuoteBatch];
    if (ChapterXPathResolver::findTextRanges(epub, spine, needles, ranges, k) == 0) {
      for (size_t t = 0; t < k; t++) {
        LOG_DBG("BKA", "spine %u: no unique match for \"%.40s\"", spine, needles[t].c_str());
        SdDebugLog::log("BKA", "spine %u: no unique match for \"%.40s\"", spine, needles[t].c_str());
      }
      continue;
    }

    // Both ends of every located quote go into one offset pass, interleaved so entry 2i is
    // a start and 2i+1 its end.
    uint32_t offsets[2 * kMaxQuoteBatch];
    bool endOfRange[2 * kMaxQuoteBatch];
    std::string paths[2 * kMaxQuoteBatch];
    for (size_t t = 0; t < k; t++) {
      offsets[2 * t] = ranges[t].found ? ranges[t].start : 0;
      offsets[2 * t + 1] = ranges[t].found ? ranges[t].end : 0;
      endOfRange[2 * t] = false;
      endOfRange[2 * t + 1] = true;
    }
    ChapterXPathResolver::findXPathsForOffsets(epub, spine, offsets, paths, 2 * k, endOfRange);

    for (size_t t = 0; t < k; t++) {
      if (!ranges[t].found) {
        LOG_DBG("BKA", "spine %u: no unique match for \"%.40s\"", spine, needles[t].c_str());
        SdDebugLog::log("BKA", "spine %u: no unique match for \"%.40s\"", spine, needles[t].c_str());
        continue;
      }
      if (paths[2 * t].empty()) {
        LOG_DBG("BKA", "spine %u: quote start at %u did not resolve", spine, ranges[t].start);
        SdDebugLog::log("BKA", "spine %u: quote start at %u did not resolve", spine, ranges[t].start);
        continue;
      }
      // Both ends make a highlight. A truncated snippet only locates the start, so that
      // anchor goes out alone and the peer draws a bookmark rather than a short highlight.
      const bool bothEnds = wholeQuote[t] && !paths[2 * t + 1].empty();
      if (!bothEnds) {
        LOG_DBG("BKA", "spine %u: anchoring quote start only (text is a prefix)", spine);
      }
      if (store.setXPath(bms[srcIdx[t]], paths[2 * t], bothEnds ? paths[2 * t + 1] : std::string())) anchored++;
    }
  }
  return anchored;
}
}  // namespace

int BookmarkAnchors::backfill(const std::shared_ptr<Epub>& epub) {
  if (!epub) return 0;

  const auto& store = BOOKMARKS;
  const auto& bms = store.getBookmarks();
  const size_t n = std::min<size_t>(bms.size(), MAX_BOOKMARKS);
  if (n == 0) return 0;

  bool has[MAX_BOOKMARKS];
  const size_t already = store.whichHaveXPaths(has, n);
  if (already >= n) return 0;  // everything is anchored; no chapter needs streaming

  int anchored = 0;
  size_t i = 0;
  while (i < n) {
    // Bookmarks are sorted by spineIndex, so one chapter's marks are contiguous.
    const uint16_t spine = bms[i].spineIndex;
    size_t spineEnd = i;
    while (spineEnd < n && bms[spineEnd].spineIndex == spine) spineEnd++;

    anchored += anchorPoints(epub, store, has, i, spineEnd, spine);
    anchored += anchorPointsByProgress(epub, store, has, i, spineEnd, spine);
    anchored += anchorQuotes(epub, store, has, i, spineEnd, spine);
    i = spineEnd;
  }

  const size_t unanchored = n - already;
  if (anchored > 0 || unanchored > 0) {
    LOG_DBG("BKA", "Anchored %d of %u pending bookmark(s) for sync", anchored, static_cast<unsigned>(unanchored));
    SdDebugLog::log("BKA", "anchored %d of %u pending (%u of %u marks already had anchors)", anchored,
                    static_cast<unsigned>(unanchored), static_cast<unsigned>(already), static_cast<unsigned>(n));
  }
  return anchored;
}

int BookmarkAnchors::adoptForeign(const std::shared_ptr<Epub>& epub, GfxRenderer& renderer, const bool onlyUnplaced) {
  if (!epub) return 0;
  auto& store = BOOKMARKS;
  const auto wanted = onlyUnplaced ? &BookmarkStore::isUnplacedForeign : &BookmarkStore::isForeignMark;

  // Snapshot the identities before touching anything. adoptForeignMark() re-sorts, so
  // indices do not survive it, and re-scanning for "still foreign" instead would re-visit
  // every adopted quote -- whose identity deliberately does not change -- and re-stream
  // its chapter once per adoption. Each mark is resolved exactly once this way.
  size_t count = 0;
  for (const auto& bm : store.getBookmarks()) {
    if (wanted(bm)) count++;
  }
  if (count == 0) return 0;

  // Bounded by the marks actually present, 14 bytes each, and released before the sync
  // runs. The stack is not an option: this pass shares a frame budget with a chapter
  // parse, and the array would be up to 1.8KB.
  auto keys = makeUniqueNoThrow<ForeignKey[]>(count);
  if (!keys) {
    LOG_ERR("BKA", "OOM: %u bytes for the foreign-mark key set", static_cast<unsigned>(count * sizeof(ForeignKey)));
    return 0;
  }
  size_t k = 0;
  for (const auto& bm : store.getBookmarks()) {
    if (!wanted(bm) || k >= count) continue;
    keys[k++] = ForeignKey{bm.quote, bm.spineIndex, bm.paragraphIndex, bm.startWord, bm.endWord, bm.progress};
  }

  // Every outcome is counted. Two of them used to leave no trace at all, which made
  // "already settled" and "silently skipped" the same number in the log.
  int adopted = 0, settled = 0, unplaced = 0, gone = 0, noAnchor = 0, unresolved = 0, noPages = 0;
  for (size_t j = 0; j < k; j++) {
    const ForeignKey& key = keys[j];
    const size_t i = store.indexOfMark(key.quote, key.spine, key.para, key.progress, key.startWord, key.endWord);
    if (i == SIZE_MAX) {
      gone++;  // deleted, or re-keyed by an adoption earlier in this same pass
      continue;
    }

    // Before the stream, not after: a mark from a KOReader peer often sits in a chapter
    // this device has never opened, so there is no page table to convert through and
    // resolving the anchor would buy nothing. The peer's spine is its DocFragment index,
    // derived the same way this device derives its own, so it is reliable enough to
    // decide whether to pay -- and a disagreement is counted below rather than silent.
    if (Section(epub, key.spine, renderer).getCachedPageCount().value_or(0) == 0) {
      SdDebugLog::log("BKA", "no page table spine=%u", key.spine);
      noPages++;
      continue;
    }

    std::string xpath;
    std::string endXPath;
    if (!store.getXPath(store.getBookmarks()[i], xpath, &endXPath) || xpath.empty()) {
      LOG_DBG("BKA", "foreign mark at spine %u has no anchor to resolve", key.spine);
      SdDebugLog::log("BKA", "no anchor q=%d spine=%u sw=%u", key.quote ? 1 : 0, key.spine, key.startWord);
      noAnchor++;
      continue;
    }

    ProgressMapper::XPathAnchor anchor;
    if (!ProgressMapper::resolveXPathAnchor(epub, xpath, anchor)) {
      LOG_DBG("BKA", "foreign anchor does not resolve here: %.60s", xpath.c_str());
      SdDebugLog::log("BKA", "unresolved: %.60s", xpath.c_str());
      unresolved++;
      continue;
    }
    if (anchor.spineIndex != key.spine) {
      // The peer derives its spine from the same DocFragment index, so a disagreement
      // means one of the two read the path wrongly. The resolved one wins: it is the one
      // that actually found the text.
      LOG_DBG("BKA", "foreign mark claims spine %u, anchor resolves to %d", key.spine, anchor.spineIndex);
    }

    // The anchor resolves to a character offset; a mark is stored as a page fraction.
    // Converting needs the chapter's cached page table, which the Section reads without
    // laying anything out. Writing the character fraction straight through would put the
    // mark on whatever page shares its number, and PageMarks would then look for the
    // highlight on a page its text is not on.
    Section pages(epub, anchor.spineIndex, renderer);
    // getCachedPageCount() refuses a partially built chapter on purpose: its count is the
    // suspended build's watermark, not the chapter total, and dividing by it would push
    // every mark in that chapter too far forward.
    const uint16_t total = pages.getCachedPageCount().value_or(0);
    const auto page = pages.getPageForVisibleTextOffset(anchor.visibleTextOffset);
    if (!page || total == 0) {
      // No cached pagination for this chapter yet, or the anchor sits past what a partial
      // build has reached. Read into the chapter once and the next sync will place it.
      LOG_DBG("BKA", "no page for spine %d offset %u; leaving the peer's estimate", anchor.spineIndex,
              anchor.visibleTextOffset);
      SdDebugLog::log("BKA", "unplaced spine=%d off=%u pages=%u", anchor.spineIndex, anchor.visibleTextOffset,
                      static_cast<unsigned>(total));
      unplaced++;
      continue;
    }
    const float progress = static_cast<float>(*page) / static_cast<float>(total);

    // Where the quote ENDS, so a selection made on the peer's larger page can be drawn
    // across the two or three pages it covers here. Another chapter stream, so only for a
    // quote that has an end anchor at all; failing to resolve it leaves the mark
    // single-page, which is what it was before ends were resolved.
    float endProgress = -1.0f;
    if (key.quote && !endXPath.empty() && endXPath != xpath) {
      ProgressMapper::XPathAnchor endAnchor;
      if (ProgressMapper::resolveXPathAnchor(epub, endXPath, endAnchor) && endAnchor.spineIndex == anchor.spineIndex) {
        if (const auto endPage = pages.getPageForVisibleTextOffset(endAnchor.visibleTextOffset)) {
          endProgress = static_cast<float>(*endPage) / static_cast<float>(total);
          if (endProgress < progress) endProgress = progress;  // inverted anchors: trust the start
        }
      }
      if (endProgress < 0.0f) {
        SdDebugLog::log("BKA", "no end page for %.40s", endXPath.c_str());
      }
    }

    const int tocIdx = epub->getTocIndexForSpineIndex(anchor.spineIndex);
    const std::string chapter = (tocIdx >= 0) ? epub->getTocItem(tocIdx).title : std::string();

    const float was = store.getBookmarks()[i].progress;
    if (store.adoptForeignMark(i, static_cast<uint16_t>(anchor.spineIndex), progress, anchor.paragraphIndex,
                               chapter.c_str(), xpath, endXPath, *page, total, anchor.visibleTextOffset, endProgress)) {
      adopted++;
      // Per mark, because the counts alone cannot show a mark that is re-filed to the
      // same place on every sync -- which is what a mark that never settles looks like.
      SdDebugLog::log("BKA", "adopt q=%d spine=%d off=%u page=%u/%u p=%.4f->%.4f end=%.4f", key.quote ? 1 : 0,
                      anchor.spineIndex, anchor.visibleTextOffset, static_cast<unsigned>(*page),
                      static_cast<unsigned>(total), static_cast<double>(was), static_cast<double>(progress),
                      static_cast<double>(endProgress));
    } else {
      settled++;
    }
  }

  // Always traced, and with the counts: "adopted 0" and "never ran" are the same silence
  // otherwise, and they call for opposite fixes.
  LOG_DBG("BKA", "Re-filed %d of %u mark(s) made on a KOReader peer", adopted, static_cast<unsigned>(k));
  SdDebugLog::log("BKA", "%s foreign=%u adopted=%d settled=%d noanchor=%d unresolved=%d nopages=%d unplaced=%d gone=%d",
                  onlyUnplaced ? "open" : "sync", static_cast<unsigned>(k), adopted, settled, noAnchor, unresolved,
                  noPages, unplaced, gone);
  return adopted;
}
