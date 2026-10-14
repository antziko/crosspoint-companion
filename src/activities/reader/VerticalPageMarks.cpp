#include "VerticalPageMarks.h"

#include <BookmarkStore.h>
#include <GfxRenderer.h>

#include <algorithm>
#include <cstring>

#include "CrossPointSettings.h"
#include "VerticalWordTokens.h"
#include "util/LookupMarks.h"
#include "util/SnippetMatch.h"

namespace VerticalPageMarks {

namespace {

// Same limits and dot pitch as PageMarks: a reading page holds a handful of each.
constexpr size_t kMaxLookupsPerPage = 8;
constexpr size_t kMinSnippet = 8;
constexpr int kRule = 2;       // rule thickness
constexpr int kLookupGap = 1;  // px between the cells and the lookup dots
constexpr int kQuoteGap = 4;   // px between the cells and a quote's rule, clear of the dots
constexpr int kDotPitch = 4;   // one 2x2 dot every this many pixels
constexpr int kBandPadX = 1;

struct SnippetFind {
  SnippetMatch::Matcher matcher;
  bool found = false;
};

// A looked-up word's segment down one column, and the one before it when the word wrapped.
struct LookupRun {
  LookupMarks::RunState state;
  int16_t x = 0;
  int16_t y0 = 0;
  int16_t y1 = 0;
  int16_t headX = 0;
  int16_t headY0 = 0;
  int16_t headY1 = 0;
};

struct DrawCtx {
  const GfxRenderer* renderer;
  const QuoteRange* quotes;
  size_t quoteCount;
  uint8_t quoteStyle;
  const LookupMarks::Mark* lookups[kMaxLookupsPerPage];
  LookupRun runs[kMaxLookupsPerPage];
  size_t lookupCount;
};

void dottedRule(const GfxRenderer& r, const int x, const int y0, const int y1) {
  for (int y = y0; y < y1; y += kDotPitch) r.fillRect(x, y, kRule, std::min(kRule, y1 - y), true);
}

}  // namespace

bool findSnippet(const GfxRenderer& renderer, const VerticalPage& page, const int fontId, const int marginLeft,
                 const int marginTop, const int cellPx, const char* snippet, uint16_t& start) {
  if (!snippet || std::strlen(snippet) < kMinSnippet) return false;
  SnippetFind f;
  f.matcher.snippet = snippet;
  f.matcher.begin(BOOKMARK_SNIPPET_MAX);
  forEachVerticalToken(renderer, page, fontId, marginLeft, marginTop, cellPx, &f,
                       [](void* ctx, const VerticalToken& t) {
                         auto* s = static_cast<SnippetFind*>(ctx);
                         s->found = s->matcher.offer(t.text, t.len, t.index);
                         return !s->found;
                       });
  // A quote whose opening words run off the page foot still starts here.
  if (!f.found && !f.matcher.open) return false;
  start = f.matcher.start;
  return true;
}

bool tokenAtOffset(const GfxRenderer& renderer, const VerticalPage& page, const int fontId, const int marginLeft,
                   const int marginTop, const int cellPx, const uint32_t offset, const char* snippet, uint16_t& index) {
  if (offset == 0) return false;
  struct Ctx {
    uint32_t offset;
    const char* snippet;
    uint16_t index = 0;
    bool found = false;
  } c{offset, snippet};
  forEachVerticalToken(renderer, page, fontId, marginLeft, marginTop, cellPx, &c,
                       [](void* raw, const VerticalToken& t) {
                         auto* cc = static_cast<Ctx*>(raw);
                         if (t.cell.startOffset < cc->offset) return true;
                         if (t.cell.startOffset == cc->offset) {
                           // The snippet must open with this token's text: an offset from a
                           // different layout of the chapter names some other character.
                           SnippetMatch::Matcher m;
                           m.snippet = cc->snippet ? cc->snippet : "";
                           m.begin(BOOKMARK_SNIPPET_MAX);
                           cc->found = m.offer(t.text, t.len, t.index) || m.open;
                           cc->index = t.index;
                         }
                         return false;
                       });
  if (!c.found) return false;
  index = c.index;
  return true;
}

bool offsetOfToken(const GfxRenderer& renderer, const VerticalPage& page, const int fontId, const int marginLeft,
                   const int marginTop, const int cellPx, const uint16_t index, uint32_t& offset) {
  struct Ctx {
    uint16_t index;
    uint32_t offset = 0;
    bool found = false;
  } c{index};
  forEachVerticalToken(renderer, page, fontId, marginLeft, marginTop, cellPx, &c,
                       [](void* raw, const VerticalToken& t) {
                         auto* cc = static_cast<Ctx*>(raw);
                         if (t.index != cc->index) return true;
                         cc->offset = t.cell.startOffset;
                         cc->found = true;
                         return false;
                       });
  if (!c.found) return false;
  offset = c.offset;
  return true;
}

void draw(const GfxRenderer& renderer, const VerticalPage& page, const int fontId, const int marginLeft,
          const int marginTop, const int cellPx, const uint32_t chapterHash, const int pageNumber, const int pageCount,
          const QuoteRange* quotes, const size_t quoteCount) {
  DrawCtx d{};
  d.renderer = &renderer;
  d.quoteStyle = SETTINGS.quoteHighlightStyle;
  d.quotes = d.quoteStyle == CrossPointSettings::QUOTE_STYLE_OFF ? nullptr : quotes;
  d.quoteCount = d.quotes ? quoteCount : 0;
  if (SETTINGS.lookupUnderline) {
    d.lookupCount = static_cast<size_t>(
        LookupMarks::getInstance().collectForPage(chapterHash, pageNumber, pageCount, d.lookups, kMaxLookupsPerPage));
  }
  if (d.quoteCount == 0 && d.lookupCount == 0) return;  // nearly every page: nothing walked

  forEachVerticalToken(
      renderer, page, fontId, marginLeft, marginTop, cellPx, &d, [](void* raw, const VerticalToken& t) {
        auto* c = static_cast<DrawCtx*>(raw);
        const GfxRenderer& r = *c->renderer;
        const VerticalClipCell& cell = t.cell;
        for (size_t i = 0; i < c->quoteCount; i++) {
          if (t.index < c->quotes[i].start || t.index > c->quotes[i].end) continue;
          if (c->quoteStyle == CrossPointSettings::QUOTE_STYLE_UNDERLINE) {
            r.fillRect(cell.x - kQuoteGap - kRule, cell.y, kRule, cell.height, true);
          } else {
            // washRectDither keeps the glyphs already drawn (see PageMarks).
            r.washRectDither(cell.x - kBandPadX, cell.y, cell.width + 2 * kBandPadX, cell.height, Color::LightGray);
          }
          break;
        }

        if (c->lookupCount == 0) return true;
        uint16_t tokenLen = 0;
        const uint32_t tokenHash = LookupMarks::hashAppend(LookupMarks::FNV_OFFSET, t.text, t.len, &tokenLen);
        const int16_t x = static_cast<int16_t>(cell.x - kLookupGap - kRule);
        const int16_t y0 = cell.y;
        const int16_t y1 = static_cast<int16_t>(cell.y + cell.height);
        for (size_t i = 0; i < c->lookupCount; i++) {
          LookupRun& run = c->runs[i];
          using Step = LookupMarks::Step;
          // The column stands in for the row: a word wrapping into the next column is two segments.
          const Step outcome = LookupMarks::step(*c->lookups[i], run.state, t.isCjk, tokenHash, tokenLen, t.text, t.len,
                                                 static_cast<int16_t>(cell.column));
          if (outcome == Step::None) continue;
          if (run.state.wrappedHere) {
            run.headX = run.x;
            run.headY0 = run.y0;
            run.headY1 = run.y1;
            run.x = x;
            run.y0 = y0;
            run.y1 = y0;
          }
          switch (outcome) {
            case Step::Opened:
              run.x = x;
              run.y0 = y0;
              run.y1 = y1;
              break;
            case Step::Extended:
              run.y1 = std::max(run.y1, y1);
              break;
            case Step::MatchedRun:
              if (run.state.wrapped) dottedRule(r, run.headX, run.headY0, run.headY1);
              dottedRule(r, run.x, run.y0, std::max(run.y1, y1));
              break;
            case Step::MatchedToken:
              dottedRule(r, x, y0, y1);
              break;
            case Step::None:
              break;
          }
        }
        return true;
      });
}

}  // namespace VerticalPageMarks
