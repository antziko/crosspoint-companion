#pragma once
#include <Epub.h>
#include <GfxRenderer.h>

#include <memory>
#include <optional>
#include <string>

#include "CrossPointPosition.h"
#include "KOReaderSyncClient.h"

/**
 * Progress position representation.
 */
struct SavedProgressPosition {
  std::string xpath;  // XPath-like progress string
  float percentage;   // Progress percentage (0.0 to 1.0)
};

/**
 * Maps between CrossPoint and SavedProgress position formats, such as those used by KOReader.
 *
 * CrossPoint tracks position as (spineIndex, visibleTextOffset). Page number is
 * derived from the current section layout.
 * SavedProgress uses XPath-like strings + percentage.
 *
 * The section cache records page-start visible offsets during pagination. The
 * same body-text counting rules are used to generate and resolve KOReader
 * XPaths. Percentage remains metadata and a fallback only.
 */
class ProgressMapper {
 public:
  /** Where a KOReader XPath lands inside its own spine item. */
  struct XPathAnchor {
    int spineIndex = -1;
    uint32_t visibleTextOffset = 0;
    // 1-based, in the same counting the section cache uses. UINT16_MAX when the path
    // named no paragraph (a chapter start, or a position inside some other element).
    uint16_t paragraphIndex = UINT16_MAX;
    float intraSpineProgress = 0.0f;
  };

  /**
   * Resolve a KOReader XPath to the position it names inside its spine item.
   *
   * The inverse of ChapterXPathResolver::findXPathsForOffsets, and what lets a mark a
   * KOReader peer made — which arrives with a correct anchor but only a guess at
   * CrossPoint's own coordinates — be re-filed under this device's. Streams the spine
   * item once; needs no renderer, no Section and no pagination, so it can run wherever
   * the anchor backfill does.
   *
   * @return false when the path names no DocFragment, names one outside this book, or
   *         resolves to no text within it.
   */
  static bool resolveXPathAnchor(const std::shared_ptr<Epub>& epub, const std::string& xpath, XPathAnchor& out);

  /**
   * Convert CrossPoint position to SavedProgress format.
   *
   * @param epub The EPUB book
   * @param pos CrossPoint position
   * @return SavedProgress position
   */
  static SavedProgressPosition toSavedProgress(const std::shared_ptr<Epub>& epub, const CrossPointPosition& pos);

  /**
   * Convert SavedProgress position to CrossPoint format.
   *
   * Note: The returned pageNumber may be approximate since different
   * rendering settings produce different page counts.
   *
   * @param epub The EPUB book
   * @param savedPos SavedProgress position
   * @param renderer GfxRenderer for page count estimation
   * @param currentSpineIndex Index of the currently open spine item (for density estimation)
   * @param totalPagesInCurrentSpine Total pages in the current spine item (for density estimation)
   * @return CrossPoint position
   */
  static CrossPointPosition toCrossPoint(const std::shared_ptr<Epub>& epub, const SavedProgressPosition& savedPos,
                                         GfxRenderer& renderer, int currentSpineIndex = -1,
                                         int totalPagesInCurrentSpine = 0, int fallbackTotalPages = 0);

  /**
   * Resolve a rich CrossPoint position (downloaded from a crosspoint-sync server)
   * using its spine/page/paragraph hints.
   *
   * PRECONDITION: the caller must have already resolved the standard KOReader
   * XPath via toCrossPoint() and found no visible text offset. rich.xpath is the
   * same string as the standard progress field, so this function deliberately does
   * not re-resolve it — doing so would repeat that failed spine streaming for an
   * identical result. Call this only as the fallback for that miss.
   *
   * @return The position, or std::nullopt when the rich hints cannot be applied
   *         (spine out of range, no section cache), leaving the caller's existing
   *         toCrossPoint() result in place.
   */
  static std::optional<CrossPointPosition> fromRichPosition(const std::shared_ptr<Epub>& epub,
                                                            const KOReaderRichPosition& rich, GfxRenderer& renderer);

 private:
  /**
   * Generate a fallback XPath by streaming the spine item's XHTML and resolving
   * a paragraph/text position from intra-spine progress.
   * Produces a full ancestry path such as
   * /body/DocFragment[3]/body/p[42]/text().17.
   */
  static std::string generateXPath(const std::shared_ptr<Epub>& epub, int spineIndex, float intraSpineProgress);
};
