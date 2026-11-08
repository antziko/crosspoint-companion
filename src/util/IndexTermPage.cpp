#include "IndexTermPage.h"

#include <Epub/Page.h>
#include <Logging.h>
#include <Memory.h>
#include <SdDebugLog.h>

#include <cstring>
#include <utility>

#include "IndexTerm.h"

namespace IndexTermPage {
namespace {

// Calls fn(line, block, word, tokenIndex, lineOrdinal) for every word on the page, in order.
template <typename Fn>
void forEachWord(const Page& page, Fn&& fn) {
  size_t index = 0;
  uint16_t lineOrdinal = 0;
  for (const auto& element : page.elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(element.get());
    const auto& block = line->getBlock();
    if (!block) continue;
    for (uint16_t w = 0; w < block->wordCount(); w++) {
      if (!fn(*line, *block, w, index++, lineOrdinal)) return;
    }
    lineOrdinal++;
  }
}

// A word belongs to a link when its line sits in the link's box and the word starts inside it.
// Link boxes share the line's coordinates (ChapterHtmlSlimParser::addLineToPage).
bool inLink(const PageLine& line, const TextBlock& block, const uint16_t w, const PageLink& link) {
  // The box starts above the line by a ruby shift or a superscript lift, never by a whole line.
  constexpr int kLiftSlack = 8;
  if (line.yPos + kLiftSlack < link.y || line.yPos >= link.y + link.height) return false;
  const int x = line.xPos + block.wordXpos(w);
  return x >= link.x - 1 && x < link.x + link.width;
}

size_t firstTokenOfLink(const Page& page, const PageLink& link) {
  size_t found = SIZE_MAX;
  forEachWord(page, [&](const PageLine& line, const TextBlock& block, uint16_t w, size_t index, uint16_t) {
    if (!inLink(line, block, w, link)) return true;
    if (IndexTerm::isSeparator(block.wordText(w), block.wordTextLen(w))) return true;
    found = index;
    return false;
  });
  return found;
}

// The link's own words. Punctuation laid tight against it (the "," of "186, 191") is kerned back
// inside the box and would otherwise read as part of the label.
bool labelOf(const Page& page, const PageLink& link, char* out, const size_t cap) {
  size_t used = 0;
  out[0] = '\0';
  forEachWord(page, [&](const PageLine& line, const TextBlock& block, uint16_t w, size_t, uint16_t) {
    if (!inLink(line, block, w, link)) return true;
    const size_t len = block.wordTextLen(w);
    if (IndexTerm::isSeparator(block.wordText(w), len)) return true;
    const size_t need = len + (used > 0 ? 1 : 0);
    if (used + need + 1 > cap) return false;
    if (used > 0) out[used++] = ' ';
    std::memcpy(out + used, block.wordText(w), len);
    used += len;
    out[used] = '\0';
    return true;
  });
  return used > 0;
}

}  // namespace

bool linkLabel(const Page& page, const size_t linkIndex, char* out, const size_t cap) {
  if (cap == 0 || linkIndex >= page.links.size()) return false;
  return labelOf(page, page.links[linkIndex], out, cap);
}

bool termForLink(const Page& page, const Page* before, const size_t linkIndex, char* out, const size_t cap) {
  if (cap == 0) return false;
  out[0] = '\0';
  if (linkIndex >= page.links.size()) return false;

  // An index reads as page references: count the links that are one.
  size_t refLinks = 0;
  for (const auto& link : page.links) {
    char label[24];
    if (labelOf(page, link, label, sizeof(label)) && IndexTerm::isPageRefLabel(label)) refLinks++;
  }
  char chosen[24];
  if (!labelOf(page, page.links[linkIndex], chosen, sizeof(chosen))) std::strcpy(chosen, "?");
  if (!IndexTerm::looksLikeIndex(page.links.size(), refLinks)) {
    // Logged on every followed link (a deliberate action, so a handful of lines a session): the
    // one place an index can be missed without trace otherwise.
    SdDebugLog::log("IDX", "not an index: link %u \"%s\" links=%u refs=%u", static_cast<unsigned>(linkIndex),
                    chosen, static_cast<unsigned>(page.links.size()), static_cast<unsigned>(refLinks));
    return false;
  }

  // The page before goes first, so a term or heading that begins there reads on into this page.
  const auto countWords = [](const Page& p) {
    size_t n = 0;
    uint16_t lines = 0;
    forEachWord(p, [&](const PageLine&, const TextBlock&, uint16_t, size_t, uint16_t line) {
      n++;
      lines = static_cast<uint16_t>(line + 1);
      return true;
    });
    return std::pair<size_t, uint16_t>{n, lines};
  };
  const auto [beforeCount, beforeLines] = before ? countWords(*before) : std::pair<size_t, uint16_t>{0, 0};
  const size_t pageCount = countWords(page).first;
  if (pageCount == 0) return false;
  const size_t count = beforeCount + pageCount;
  // The term is found walking backwards across lines, so it needs the words as one array. Heap
  // rather than stack: two pages are a few hundred words (16 bytes each), built once per index tap
  // and freed on return.
  auto tokens = makeUniqueNoThrow<IndexTerm::Token[]>(count);
  if (!tokens) {
    LOG_ERR("IDX", "OOM: %u index tokens", static_cast<unsigned>(count));
    return false;
  }
  const auto fill = [&](const Page& p, const size_t offset, const uint16_t lineOffset) {
    forEachWord(p, [&](const PageLine& line, const TextBlock& block, uint16_t w, size_t index, uint16_t lineOrdinal) {
      tokens[offset + index] = {block.wordText(w), block.wordTextLen(w), static_cast<uint16_t>(lineOffset + lineOrdinal),
                                static_cast<int16_t>(line.xPos + block.wordXpos(w))};
      return true;
    });
  };
  if (before) fill(*before, 0, 0);
  fill(page, beforeCount, beforeLines);
  const size_t onPage = firstTokenOfLink(page, page.links[linkIndex]);
  if (onPage == SIZE_MAX) {
    SdDebugLog::log("IDX", "no term: link %u has no word under its box (links=%u refs=%u)",
                    static_cast<unsigned>(linkIndex), static_cast<unsigned>(page.links.size()),
                    static_cast<unsigned>(refLinks));
    return false;
  }
  const size_t linkToken = beforeCount + onPage;
  size_t first = 0;
  if (IndexTerm::extractTerm(tokens.get(), count, linkToken, out, cap, &first)) {
    // A sub-entry carries its heading: out = "term" kParentSeparator "heading".
    char parent[IndexTerm::kMaxTerm];
    const bool hasParent = IndexTerm::extractParent(tokens.get(), count, first, parent, sizeof(parent));
    const size_t termLen = std::strlen(out);
    if (hasParent && termLen + 1 + std::strlen(parent) < cap) {
      out[termLen] = IndexTerm::kParentSeparator;
      std::strcpy(out + termLen + 1, parent);
    }
    const auto& lineHead = tokens[first];
    SdDebugLog::log("IDX", "term \"%.*s\" parent=\"%s\" for link %u \"%s\" x=%d (links=%u refs=%u)", (int)termLen,
                    out, hasParent ? parent : "", static_cast<unsigned>(linkIndex), chosen, lineHead.x,
                    static_cast<unsigned>(page.links.size()), static_cast<unsigned>(refLinks));
    return true;
  }
  // An index page whose entry yielded nothing: name the words around the link so the log shows why.
  const auto& at = tokens[linkToken];
  const auto& prev = linkToken > 0 ? tokens[linkToken - 1] : at;
  const auto& prev2 = linkToken > 1 ? tokens[linkToken - 2] : prev;
  SdDebugLog::log("IDX", "no term: link=\"%.*s\" before=\"%.*s\" \"%.*s\" (links=%u refs=%u)", (int)at.len, at.text,
                  (int)prev2.len, prev2.text, (int)prev.len, prev.text, static_cast<unsigned>(page.links.size()),
                  static_cast<unsigned>(refLinks));
  return false;
}

}  // namespace IndexTermPage
