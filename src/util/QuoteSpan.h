#pragma once
#include <cstdint>

/**
 * Which part of a quote a given page holds.
 *
 * A quote is stored as a pair of page fractions within its chapter. A selection made on
 * this device cannot cross a page break, so its end equals its start and it always falls
 * in one page. A selection made on a KOReader peer is made against that reader's larger
 * page and routinely covers two or three here, so the quote has to be drawn as the part
 * of it each page actually holds.
 *
 * Pure arithmetic, kept out of PageMarks so the boundaries can be tested directly: an
 * off-by-one here draws a band over text the reader never highlighted, or none at all,
 * and neither shows up anywhere but on the screen.
 */
namespace QuoteSpan {

enum class Role : uint8_t {
  NotHere,   // no part of the quote is on this page
  Whole,     // starts and ends here: the ordinary single-page mark
  Starts,    // starts here and carries on past the page
  Through,   // began earlier and carries on past: the page is highlighted end to end
  Ends,      // began earlier and stops on this page
};

/**
 * @param start      the quote's start, as a fraction of the chapter
 * @param end        its end, likewise; values below `start` are treated as `start`
 * @param pageStart  this page's fraction (pageNumber / pageCount)
 * @param pageSlice  one page's share of the chapter (1 / pageCount)
 */
inline Role pageRole(const float start, float end, const float pageStart, const float pageSlice) {
  if (end < start) end = start;
  const float pageEnd = pageStart + pageSlice;
  const bool startsHere = start >= pageStart && start < pageEnd;
  const bool endsHere = end >= pageStart && end < pageEnd;
  if (startsHere) return endsHere ? Role::Whole : Role::Starts;
  if (endsHere) return Role::Ends;
  return (start < pageStart && end >= pageEnd) ? Role::Through : Role::NotHere;
}

/** How far into this page the quote stops, in 255ths. Meaningful for Role::Ends. */
inline uint8_t endFraction(const float end, const float pageStart, const float pageSlice) {
  if (pageSlice <= 0.0f) return 255;
  const float frac = (end - pageStart) / pageSlice;
  if (frac <= 0.0f) return 0;
  if (frac >= 1.0f) return 255;
  return static_cast<uint8_t>(frac * 255.0f);
}

/**
 * The last token index a Role::Ends quote covers, given the page's final token index.
 *
 * An estimate: the section cache records a visible-character offset per PAGE, not per
 * word, so there is nothing exact to place the end against. Interpolating over the page's
 * tokens can be a word or two out, and it only ever pulls the end in from the page edge,
 * so the error shortens the band rather than marking text outside the quote.
 */
inline uint16_t stopIndex(const uint8_t frac, const uint16_t lastIndex) {
  const uint32_t est = static_cast<uint32_t>(frac) * (static_cast<uint32_t>(lastIndex) + 1u) / 255u;
  return static_cast<uint16_t>(est > lastIndex ? lastIndex : est);
}

}  // namespace QuoteSpan
