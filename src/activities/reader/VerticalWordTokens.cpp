#include "VerticalWordTokens.h"

#include <Epub/VerticalParsedText.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_heap_caps.h>

#include "VerticalClipCells.h"
#include "util/PageTokenScan.h"
#include "util/TextPool.h"

namespace {

// The text a cell carries: its run string, or its single codepoint encoded as UTF-8.
size_t cellText(const VerticalPage& page, const VerticalClipCell& cell, char (&buf)[5], const char*& out) {
  const VerticalGlyph& g = page.glyphs[cell.glyphIndex];
  if (g.renderKind == VerticalGlyph::RotatedRun || g.renderKind == VerticalGlyph::UprightRun) {
    const std::string& run = page.glyphTextStr(g);
    out = run.c_str();
    return run.size();
  }
  const uint32_t cp = g.codepoint;
  size_t n;
  if (cp < 0x80) {
    buf[0] = static_cast<char>(cp);
    n = 1;
  } else if (cp < 0x800) {
    buf[0] = static_cast<char>(0xC0 | (cp >> 6));
    buf[1] = static_cast<char>(0x80 | (cp & 0x3F));
    n = 2;
  } else if (cp < 0x10000) {
    buf[0] = static_cast<char>(0xE0 | (cp >> 12));
    buf[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    buf[2] = static_cast<char>(0x80 | (cp & 0x3F));
    n = 3;
  } else {
    buf[0] = static_cast<char>(0xF0 | (cp >> 18));
    buf[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    buf[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    buf[3] = static_cast<char>(0x80 | (cp & 0x3F));
    n = 4;
  }
  buf[n] = '\0';
  out = buf;
  return n;
}

}  // namespace

namespace {
struct TokenWalk {
  const VerticalPage* page;
  void* ctx;
  VerticalTokenVisitor visit;
  uint16_t index = 0;
};
}  // namespace

void forEachVerticalToken(const GfxRenderer& renderer, const VerticalPage& page, const int fontId,
                          const int marginLeft, const int marginTop, const int cellPx, void* ctx,
                          const VerticalTokenVisitor visit) {
  TokenWalk walk{&page, ctx, visit};
  forEachVerticalClipCell(
      renderer, page, fontId, marginLeft, marginTop, &walk,
      [](void* raw, const VerticalClipCell& cell) {
        auto* w = static_cast<TokenWalk*>(raw);
        char buf[5];
        VerticalToken t{cell};
        t.len = cellText(*w->page, cell, buf, t.text);
        if (t.len == 0 || !PageTokens::isSelectable(t.text, t.len, t.isCjk)) return true;
        t.index = w->index++;
        return w->visit(w->ctx, t);
      },
      cellPx);
}

namespace {

struct CountCtx {
  size_t words = 0;
  size_t poolBytes = 0;
  size_t rows = 0;
  int lastColumn = -1;
};

struct BuildCtx {
  const VerticalPage* page;
  std::vector<WordSelectNavigator::WordInfo>* words;
  std::vector<WordSelectNavigator::Row>* rows;
  std::string* pool;
  const uint16_t* scanIndex;  // page glyph -> scan cell, or null
  std::vector<CjkWordMerge::TokenGlyphs>* scanCells;
  int lastColumn = -1;
  bool ok = true;
};

}  // namespace

bool buildVerticalWordTokens(const GfxRenderer& renderer, const VerticalPage& page, const int fontId,
                             const int marginLeft, const int marginTop, const int cellPx,
                             std::vector<WordSelectNavigator::WordInfo>& words,
                             std::vector<WordSelectNavigator::Row>& rows, std::string& textPool,
                             std::vector<CjkWordMerge::TokenGlyphs>* scanCells) {
  words.clear();
  rows.clear();
  textPool.clear();
  if (scanCells) scanCells->clear();

  // Count first, then one allocation each: a CJK page is hundreds of single-character tokens,
  // and a vector doubling its way there asks for a block the heap may not have (the same abort
  // extractWords guards against).
  CountCtx count;
  forEachVerticalToken(renderer, page, fontId, marginLeft, marginTop, cellPx, &count,
                       [](void* ctx, const VerticalToken& t) {
                         auto* c = static_cast<CountCtx*>(ctx);
                         c->words++;
                         c->poolBytes += t.len + 1;
                         if (t.cell.column != c->lastColumn) {
                           c->rows++;
                           c->lastColumn = t.cell.column;
                         }
                         return true;
                       });
  if (count.words == 0) return true;

  constexpr size_t kHeadroom = 4096;
  const size_t wordBytes = count.words * sizeof(WordSelectNavigator::WordInfo);
  if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < wordBytes + kHeadroom) {
    LOG_ERR("DWS", "Vertical tokens: %u words need %u bytes; heap too fragmented", (unsigned)count.words,
            (unsigned)wordBytes);
    return false;
  }
  words.reserve(count.words);
  rows.reserve(count.rows);
  textPool.reserve(count.poolBytes);

  // WordSelectionScan::initFromVerticalPage numbers the page's glyphs leaving out rotated runs
  // and inline images.
  std::unique_ptr<uint16_t[]> scanIndex;
  if (scanCells) {
    scanIndex = makeUniqueNoThrow<uint16_t[]>(page.glyphs.size() + 1);
    if (!scanIndex || !reserveNoThrow(*scanCells, count.words)) {
      LOG_ERR("DWS", "OOM: vertical scan cell map");
      scanIndex.reset();
      scanCells = nullptr;
    } else {
      uint16_t next = 0;
      for (size_t i = 0; i < page.glyphs.size(); i++) {
        const VerticalGlyph& g = page.glyphs[i];
        const bool skipped = g.renderKind == VerticalGlyph::RotatedRun || VerticalParsedText::isImageMarker(g.codepoint);
        scanIndex[i] = skipped ? CjkWordMerge::TokenGlyphs::kNoGlyph : next++;
      }
    }
  }

  BuildCtx build{&page, &words, &rows, &textPool, scanIndex.get(), scanCells};
  forEachVerticalToken(
      renderer, page, fontId, marginLeft, marginTop, cellPx, &build, [](void* ctx, const VerticalToken& t) {
        auto* b = static_cast<BuildCtx*>(ctx);
        const VerticalClipCell& cell = t.cell;
        uint16_t offset = 0;
        if (!TextPool::appendNoThrow(*b->pool, t.text, t.len, offset)) {
          b->ok = false;
          return false;
        }
        if (cell.column != b->lastColumn) {
          // Row::yPos holds the column's x: a vertical row is a column.
          b->rows->push_back({cell.x, static_cast<int16_t>(b->words->size()), 0});
          b->lastColumn = cell.column;
        }
        WordSelectNavigator::WordInfo w;
        w.textOffset = offset;
        w.textLen = static_cast<uint16_t>(t.len);
        w.lookupOffset = offset;
        w.lookupLen = static_cast<uint16_t>(t.len);
        w.screenX = cell.x;
        w.screenY = cell.y;
        w.width = cell.height;  // the extent down the column
        w.row = static_cast<int16_t>(b->rows->size() - 1);
        w.style = static_cast<EpdFontFamily::Style>(b->page->glyphs[cell.glyphIndex].style);
        b->words->push_back(w);
        b->rows->back().wordCount++;
        if (b->scanCells) {
          CjkWordMerge::TokenGlyphs tg;
          tg.start = b->scanIndex[cell.glyphIndex];
          tg.count = tg.start == CjkWordMerge::TokenGlyphs::kNoGlyph ? 0 : 1;
          b->scanCells->push_back(tg);
        }
        return true;
      });
  if (!build.ok) {
    words.clear();
    rows.clear();
    textPool.clear();
    if (scanCells) scanCells->clear();
    return false;
  }
  return true;
}
