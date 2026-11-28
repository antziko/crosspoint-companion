#include "DictLayout.h"

#include <Memory.h>
#include <Utf8.h>

#include <cstring>
#include <numeric>
#include <utility>

#include "IpaUtils.h"

namespace DictLayout {

Wrapper::Wrapper(const WrapMetrics& metrics, const Measurer& measure, const LineSink& sink, const IpaFontCoverage& cov)
    : maxWidth_(metrics.maxWidth),
      indentStep_(metrics.indentStep),
      bulletWidth_(metrics.bulletWidth),
      tracking_(metrics.tracking),
      measure_(measure),
      sink_(sink),
      cov_(cov) {
  ipaRuns_.reserve(4);
  startLine(0, false);
}

// Width of a string, accounting for mixed IPA/non-IPA runs (each run measured
// with the appropriate font via the injected measurer).
//
// The textHasIpa() fast path is not an optimisation of a rare case: it is the normal case.
// splitIpaRuns() on IPA-free text produces exactly one non-IPA run holding a heap copy of the
// whole string, which this then measures and throws away — so for every CJK definition (and
// every Latin one without phonetics) the split was pure allocation. Measuring `text` directly
// is the same arithmetic over the same single run.
int Wrapper::getMixedWidth(const char* text, EpdFontFamily::Style style) {
  if (!text || !text[0]) return 0;
  if (!textHasIpa(text, cov_)) return measure_(text, style, false);
  ipaRuns_.clear();
  if (!splitIpaRuns(text, ipaRuns_, cov_)) {
    oom_ = true;
    return 0;
  }
  return std::accumulate(ipaRuns_.begin(), ipaRuns_.end(), 0, [&](int sum, const IpaTextSpan& run) {
    return sum + measure_(run.text.c_str(), style, run.isIpa);
  });
}

void Wrapper::flushLine() {
  if (!currentLine_.segments.empty()) {
    sink_(std::move(currentLine_));
    currentLine_ = LayoutLine{};
  }
}

void Wrapper::startLine(uint8_t indent, bool listItem) {
  currentLine_.indentLevel = indent;
  currentLine_.isListItem = listItem;
  currentX_ = indent * indentStep_ + (listItem ? bulletWidth_ : 0);
}

void Wrapper::appendToLine(const char* text, size_t len, EpdFontFamily::Style style, bool isIpa, int width) {
  if (!currentLine_.segments.empty() && currentLine_.segments.back().style == style &&
      currentLine_.segments.back().isIpa == isIpa) {
    std::string& seg = currentLine_.segments.back().text;
    // Merged, the two pieces draw as one string, which tracks the glyphs either side of the join.
    if (!seg.empty() && seg.back() != ' ' && len > 0 && text[0] != ' ') width += tracking_;
    if (!appendNoThrow(seg, text, len)) {
      oom_ = true;
      return;
    }
  } else {
    if (currentLine_.segments.size() == currentLine_.segments.capacity() &&
        !reserveNoThrow(currentLine_.segments,
                        currentLine_.segments.capacity() ? currentLine_.segments.capacity() * 2 : 4)) {
      oom_ = true;
      return;
    }
    // Build the segment in place so a failed text copy leaves no half-formed segment behind:
    // the reserve above guarantees the push_back itself cannot reallocate.
    currentLine_.segments.push_back({std::string{}, style, isIpa});
    if (!appendNoThrow(currentLine_.segments.back().text, text, len)) {
      currentLine_.segments.pop_back();
      oom_ = true;
      return;
    }
  }
  currentX_ += width;
}

int Wrapper::joinGap(const char* text) const {
  if (tracking_ <= 0 || currentLine_.segments.empty() || !text || text[0] == '\0' || text[0] == ' ') return 0;
  const std::string& last = currentLine_.segments.back().text;
  return !last.empty() && last.back() != ' ' ? tracking_ : 0;
}

void Wrapper::appendMixed(const char* text, EpdFontFamily::Style style, const int forceWordIpa) {
  if (!text || !text[0]) return;
  // Same fast path as getMixedWidth(), and it must stay in lockstep with it: one non-IPA run
  // covering the whole string is exactly what splitIpaRuns() would have produced.
  if (forceWordIpa == 0 || (forceWordIpa < 0 && !textHasIpa(text, cov_))) {
    appendToLine(text, strlen(text), style, false, measure_(text, style, false));
    return;
  }
  ipaRuns_.clear();
  if (!splitIpaRuns(text, ipaRuns_, cov_, forceWordIpa)) {
    oom_ = true;
    return;
  }
  for (const auto& run : ipaRuns_) {
    appendToLine(run.text.c_str(), run.text.size(), style, run.isIpa, measure_(run.text.c_str(), style, run.isIpa));
  }
}

// Break a single token at codepoint boundaries when it is wider than the available line width.
// The token is classified once, as a whole word, and every piece keeps that classification:
// measuring a piece and drawing it must pick the same font per codepoint (splitIpaRuns rules).
//
// This is the hot path for CJK: the script has no spaces, so a definition's whole run arrives
// as one token that always overflows and always lands here. The per-codepoint std::string this
// used to build was therefore one malloc/free pair per character on the very heap that had
// nothing left — replaced by a stack buffer, with pending_ a reused member so its capacity
// carries across tokens.
void Wrapper::breakToken(const char* tok, EpdFontFamily::Style style, uint8_t indentLevel) {
  const bool tokIsIpa = wordNeedsIpaFont(tok, tok + strlen(tok), cov_);
  const int force = tokIsIpa ? 1 : 0;
  const auto* bp = reinterpret_cast<const uint8_t*>(tok);
  pending_.clear();
  int pendingWidth = 0;
  bool prevIsIpa = false;
  uint32_t cp;
  while ((cp = utf8NextCodepoint(&bp))) {
    if (oom_) return;
    const bool cpIsIpa = utf8IsCombiningMark(cp) && !pending_.empty() ? prevIsIpa : tokIsIpa && cov_.has(true, cp);
    prevIsIpa = cpIsIpa;
    char cpBuf[5];
    const int cpLen = utf8EncodeCodepoint(cp, cpBuf);
    cpBuf[cpLen] = '\0';  // measure_ takes a C string
    // Measured alone, a codepoint carries none of the tracking it gets beside its neighbour.
    const int cpBase = measure_(cpBuf, style, cpIsIpa);
    int cpWidth = cpBase + (pending_.empty() ? joinGap(cpBuf) : (cp == ' ' ? 0 : tracking_));
    if (!pending_.empty() && currentX_ + pendingWidth + cpWidth > maxWidth_) {
      appendMixed(pending_.c_str(), style, force);
      flushLine();
      startLine(indentLevel, false);
      pending_.clear();
      pendingWidth = 0;
      cpWidth = cpBase;
    }
    if (!appendNoThrow(pending_, cpBuf, static_cast<size_t>(cpLen))) {
      oom_ = true;
      return;
    }
    pendingWidth += cpWidth;
  }
  if (!pending_.empty()) appendMixed(pending_.c_str(), style, force);
}

void Wrapper::onSpan(const StyledSpan& span) {
  if (oom_) return;  // wrap already abandoned; the caller reports truncation
  if (!span.text || span.text[0] == '\0') return;

  EpdFontFamily::Style style;
  if (span.bold && span.italic) {
    style = EpdFontFamily::BOLD_ITALIC;
  } else if (span.bold) {
    style = EpdFontFamily::BOLD;
  } else if (span.italic) {
    style = EpdFontFamily::ITALIC;
  } else {
    style = EpdFontFamily::REGULAR;
  }
  if (span.underline) style = static_cast<EpdFontFamily::Style>(style | EpdFontFamily::UNDERLINE);

  if (span.newlineBefore) {
    flushLine();
    startLine(span.indentLevel, span.isListItem);
  }

  const int spanWidth = getMixedWidth(span.text, style);
  if (currentX_ + joinGap(span.text) + spanWidth <= maxWidth_) {
    // Fast path: entire span fits on the current line.
    appendMixed(span.text, style);
  } else {
    // Word-wrap within the span.
    const char* p = span.text;
    while (*p) {
      if (oom_) return;
      bool hadSpace = false;
      while (*p == ' ') {
        hadSpace = true;
        ++p;
      }
      if (!*p) break;

      const char* tokStart = p;
      while (*p && *p != ' ') ++p;
      // tok_ is a reused member: clear() keeps the capacity earned by the previous token, so
      // only the longest token in a definition actually allocates.
      tok_.clear();
      if (!appendNoThrow(tok_, tokStart, static_cast<size_t>(p - tokStart))) {
        oom_ = true;
        return;
      }

      bool lineIsEmpty = currentLine_.segments.empty();
      bool useSpace = !lineIsEmpty && hadSpace;
      const int tokWidth = getMixedWidth(tok_.c_str(), style) + (useSpace ? 0 : joinGap(tok_.c_str()));
      const int spaceWidth = useSpace ? measure_(" ", style, false) : 0;

      if (currentX_ + spaceWidth + tokWidth > maxWidth_ && !lineIsEmpty) {
        flushLine();
        startLine(span.indentLevel, false);
        useSpace = false;
      }

      if (currentX_ + (useSpace ? spaceWidth : 0) + tokWidth > maxWidth_) {
        breakToken(tok_.c_str(), style, span.indentLevel);
      } else {
        if (useSpace) appendToLine(" ", 1, style, false, spaceWidth);
        appendMixed(tok_.c_str(), style);
      }
    }
  }
}

// Flushes even after an OOM: appendToLine() never leaves a half-written segment behind (it
// pops the segment it could not fill), so the in-progress line is short but coherent, and
// emitting it hands the reader one more line of real text than dropping it would.
void Wrapper::finish() { flushLine(); }

// --------------------------------------------------------------------------
// Free-function drivers over Wrapper
// --------------------------------------------------------------------------

void wrapSpans(const std::vector<StyledSpan>& spans, const WrapMetrics& metrics, const Measurer& measure,
               const LineSink& sink) {
  Wrapper wrapper(metrics, measure, sink);
  for (const auto& span : spans) wrapper.onSpan(span);
  wrapper.finish();
}

void wrapSpans(const std::vector<StyledSpan>& spans, const WrapMetrics& metrics, const Measurer& measure,
               std::vector<LayoutLine>& out) {
  out.clear();
  out.reserve(32);
  const LineSink sink{&out, [](void* ctx, LayoutLine&& line) {
                        static_cast<std::vector<LayoutLine>*>(ctx)->push_back(std::move(line));
                      }};
  wrapSpans(spans, metrics, measure, sink);
}

}  // namespace DictLayout
