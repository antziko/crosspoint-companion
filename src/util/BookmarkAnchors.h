#pragma once
#include <Epub.h>
#include <GfxRenderer.h>

#include <memory>

/**
 * Fills in the KOReader XPath anchors that bookmark sync needs.
 *
 * CrossPoint anchors a bookmark by (spineIndex, paragraphIndex, progress); crengine can
 * only place one from an XPath. Resolving an XPath re-streams and re-parses the whole
 * spine item — the `Section` drops its parse context once the chapter is laid out
 * (Section.cpp) — so this is deliberately NOT done when a bookmark is created. It runs
 * once per sync instead, grouped by chapter, and only for bookmarks not already anchored.
 */
namespace BookmarkAnchors {

/**
 * Anchor every loaded bookmark that lacks an XPath, writing results to the .xpath sidecar.
 *
 * MUST be called with the same heap discipline as the progress XPath resolution it sits
 * beside in EpubReaderActivity::launchKoSync: the paginated Section released and the
 * framebuffer lent, and never from KOReaderSyncActivity, which runs against a ~56KB
 * post-WiFi ceiling.
 *
 * A point bookmark resolves from its paragraph index. A quote has none (it is keyed by a
 * word range, which only describes the pagination that produced it), so it resolves instead
 * by finding its stored text in the chapter — which also yields the end anchor KOReader
 * needs, since CrossPoint's endProgress is a copy of the start. A quote whose text is
 * missing, absent, or occurring more than once is left unanchored rather than guessed at.
 * Session "return here" marks are skipped — they are never synced.
 *
 * @param epub A loaded book; spine items are streamed from it.
 * @return How many bookmarks were newly anchored.
 */
int backfill(const std::shared_ptr<Epub>& epub);

/**
 * Re-file every mark a KOReader peer made under this device's own coordinates.
 *
 * Such a mark arrives with a correct XPath but only a guess at CrossPoint's position,
 * and an identity hashed into the reserved band (see FOREIGN_KEY_BASE), because a
 * KOReader client cannot derive a page-local word index or a section-cache paragraph
 * number. Resolving the anchor here — the inverse direction to backfill() above, and the
 * same cost, so it belongs in the same pass — gives the mark a real position, and for a
 * point bookmark a real identity.
 *
 * Same heap discipline as backfill(), for the same reason, and must be called after it
 * so a mark with no sidecar entry yet is not skipped.
 *
 * Convergence takes two syncs: a mark pulled in one is adopted at the start of the next,
 * which is when the re-filed record and the tombstone for the synthetic one go out.
 *
 * A mark is re-filed only when the chapter's page table can say which page the anchor
 * lands on. Bookmark::progress is a page fraction (currentPage / pageCount), the same
 * unit every native mark is stored in and the one PageMarks::drawForPage slices a page
 * against; the anchor itself resolves to a character offset, which is a different
 * measure and lands on a different page wherever text density varies. With no cached
 * pagination for that chapter there is nothing to convert through, so the mark keeps the
 * peer's own estimate -- which is at least in the right unit -- and is left for a later
 * sync.
 *
 * @param renderer needed only to open the chapter's cached page table; nothing is drawn,
 *                 and the framebuffer may be on loan.
 * @param onlyUnplaced consider only marks no device has placed yet
 *                 (BookmarkStore::isUnplacedForeign), skipping the re-check of ones
 *                 already filed here. Set on the book-open pass, where the point is to
 *                 place what a sync just pulled and a full re-check would re-stream a
 *                 chapter per adopted quote on every open. Left clear for the sync pass,
 *                 which should re-check: the pagination a mark was filed against may have
 *                 changed since, and the user is already waiting on that operation.
 * @return How many marks were re-filed.
 */
int adoptForeign(const std::shared_ptr<Epub>& epub, GfxRenderer& renderer, bool onlyUnplaced = false);

}  // namespace BookmarkAnchors
