#pragma once

#include <cstddef>
#include <cstdint>

class GfxRenderer;
struct VerticalPage;

// The marks the reader draws on a vertical (tategaki) page: saved quotes and looked-up words,
// the counterpart of PageMarks for pages laid out in lines. Tokens are numbered by
// forEachVerticalToken, the numbering word-select gives the same page, so a quote saved from a
// selection here names the same tokens. Rules sit LEFT of the column: ruby is on the right.
namespace VerticalPageMarks {

// One quote resolved onto this page, as token indices (inclusive).
struct QuoteRange {
  uint16_t start;
  uint16_t end;
};

// Where `snippet` opens on this page, by the matcher horizontal quotes are relocated with.
// Snippets shorter than 8 bytes are never relocated (they would match anywhere).
bool findSnippet(const GfxRenderer& renderer, const VerticalPage& page, int fontId, int marginLeft, int marginTop,
                 int cellPx, const char* snippet, uint16_t& start);

// The token whose cell starts at chapter offset `offset` (VerticalClipCell::startOffset), when the
// snippet's opening text is there too. For a quote saved on a vertical page, whose offset is
// exact, so even a one-character quote is found.
bool tokenAtOffset(const GfxRenderer& renderer, const VerticalPage& page, int fontId, int marginLeft, int marginTop,
                   int cellPx, uint32_t offset, const char* snippet, uint16_t& index);

// Chapter offset of token `index`'s first character, or false.
bool offsetOfToken(const GfxRenderer& renderer, const VerticalPage& page, int fontId, int marginLeft, int marginTop,
                   int cellPx, uint16_t index, uint32_t& offset);

// Draws `quotes` (band or rule per SETTINGS.quoteHighlightStyle) and a dotted rule beside every
// looked-up word LookupMarks anchors to this page. Call on the black-and-white pass, after the
// glyphs. Allocates nothing.
void draw(const GfxRenderer& renderer, const VerticalPage& page, int fontId, int marginLeft, int marginTop, int cellPx,
          uint32_t chapterHash, int pageNumber, int pageCount, const QuoteRange* quotes, size_t quoteCount);

}  // namespace VerticalPageMarks
