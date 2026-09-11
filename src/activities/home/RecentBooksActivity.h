#pragma once
#include <I18n.h>

#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

class RecentBooksActivity final : public UiListActivity {
 public:
  explicit RecentBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;
  // Guards the view picker ahead of the base frame, and drives the cover pass
  // from the render tail (see loadShelfCovers).
  void render(RenderLock&&) override;

 private:
  int listCount() const override { return static_cast<int>(recentBooks.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  // Grid cursor step, clamped rather than wrapped: wrapping a 2-D cursor is
  // disorienting, and clamping to the last book keeps a partial final row
  // reachable. delta is +/-1 along the row, +/-columns for a row jump.
  void stepShelfSelection(int delta);
  // Popup input first; then, in shelf mode, a vertical swipe pages the
  // selection (the shelf viewport follows it, so nothing else scrolls it).
  bool handleCustomInput() override;
  // Confirm activates on RELEASE here (a hold is "remove from list"), and Back
  // goes home rather than finishing.
  bool handleButtons() override;
  // No-op: handleButtons() already owns every button this screen navigates
  // with. See the definition for why the base tail double-handled them.
  void navigateButtons() override;
  // "Recent Books (7)". Formatted into a fixed buffer by rebuildRowItems()
  // whenever the list changes, so the header shows the count without building a
  // std::string on every repaint.
  const char* headerTitle() const override { return headerTitleBuf; }
  char headerTitleBuf[64] = "";
  void drawFooter() override;

  // Set when a long-press has fired; input is swallowed until Confirm is released
  // again so the release doesn't also open the book.
  bool longPressFired = false;
  // OptionPopup acts on button PRESS edges; this screen acts on Confirm/Back
  // RELEASE edges. Without this latch the release that ends the press which
  // chose a view would fall straight through to handleButtons() and open the
  // selected book (or, after a Back dismissal, exit to Home). Armed when the
  // popup closes, cleared once both buttons are observed up.
  bool swallowReleaseAfterPopup = false;

  // --- list vs bookshelf -----------------------------------------------------
  // Both read SETTINGS.recentBooksView; there is no cached copy, so a change
  // made in Settings takes effect on the next entry without a sync step.
  bool isShelf() const;
  // 2 or 3; meaningless in list mode.
  int shelfColumns() const;
  // Thumbnail height chosen by the last buildShelf() for the current theme,
  // orientation and column count. Snapped to a fixed ladder, so the set of
  // cached thumb_<H>.bmp sizes stays small; mismatches against the cell are
  // cropped, never scaled (scaling darkens a pre-dithered 1-bit bitmap).
  // The cover pass and the painter must both read THIS, not recompute.
  int shelfCoverHeight() const { return shelfCoverH; }
  int shelfCoverH = 210;
  static int pickShelfCoverHeight(int bodyHeight, int cellWidth, int columns);

  // The view picker, opened by a Back hold. Writes the setting, drops the stale
  // interaction table, and repaints.
  void showViewPicker();
  OptionPopup optionPopup;

  // Switch to `view` and do the bookkeeping a view change needs. No-op when
  // already on it. Shared by the picker and the header tap -- the ordering
  // inside is load-bearing, so there must be exactly one copy of it.
  void applyView(int view);

  // A tap anywhere on the header band cycles List -> 2x2 -> 3x3. The X4 Pro
  // wires no Back button, so the Back hold that opens the picker is
  // unreachable there and the title is the only in-screen way to switch view.
  static constexpr freeink::ui::ActionId ACTION_HEADER = ACTION_USER;
  static void headerActionTrampoline(const freeink::ui::ActionEvent& event, void* user);

  // Generates any missing cover thumbnails for the visible books. Runs from the
  // render tail on the render task — it takes a GfxRenderer::FrameBufferLoan,
  // which is legal only outside a frame build, and draws its own progress
  // popup. Latched so it runs once per entry.
  void loadShelfCovers();
  bool shelfCoversLoaded = false;
  bool shelfCoversLoading = false;
  // Cells the last shelf build laid out; the swipe pager's page size. Written
  // on the render task in buildScreen, read on the loop task under RenderLock.
  int shelfPageItems = 1;

  // Cover rects the last shelf build actually drew, recorded BY the painter as it paints
  // rather than recomputed from coverGrid's layout maths -- so the targeted repaint below
  // cannot drift from what is on screen if that layout ever changes. Written on the render
  // task, read on the loop task under RenderLock, like shelfPageItems above.
  // 24 slots (192 bytes) covers the densest rung of SHELF_COVER_LADDER with room spare; a
  // build that somehow lays out more simply stops recording and falls back to a full render.
  static constexpr int MAX_SHELF_CELLS = 24;
  freeink::ui::Rect shelfCellRects[MAX_SHELF_CELLS]{};
  int shelfCellFirst = -1;  // absolute book index of shelfCellRects[0]; -1 = nothing recorded
  int shelfCellCount = 0;

  // Repaint ONLY the two covers a selection step touches and push them to the panel,
  // returning false when that is not possible (nothing recorded yet, or the step pages the
  // shelf) so the caller can fall back to a full render.
  bool repaintShelfSelection(int previous, int next);
  // One cell: clear the ring band, redraw the cover, stroke the ring when selected.
  void paintShelfCell(int index, bool selected);

  // The shelf half of buildScreen(): lays out fui::coverGrid over the whole
  // content band. Deliberately does NOT call syncListViewport() -- that helper
  // is row-height based; the shelf derives its own page from the SDK's
  // coverGridVisibleCells/coverGridTopIndexFor.
  void buildShelf(UiScreen& screen);

  // fui::coverGrid callbacks. The provider supplies titles straight out of
  // recentBooks (no parallel CoverGridItem array); the painter streams the
  // cached BMP row-by-row and must never trigger generation.
  static freeink::ui::CoverGridItem shelfItemProvider(uint16_t index, void* userData);
  static bool shelfCoverPainter(freeink::ui::DrawTarget& target, freeink::ui::Rect rect,
                                const freeink::ui::CoverGridItem& item, uint16_t index, void* userData);

  // Title-line font, matched to the OPDS browser's book rows: the touch-target
  // sized body font on touch hardware, the denser small font on X3/X4 (where it
  // also fits more of a long title before the line ellipsizes).
  bool usesBodyLabel() const;

  std::vector<RecentBook> recentBooks;
  // Row buffer, built in loadRecentBooks() (not buildScreen(), which reuses
  // it on every repaint instead of rebuilding a ListItem vector per render).
  std::vector<freeink::ui::ListItem> rowItems;
  void rebuildRowItems();

  // Data loading
  void loadRecentBooks();

  // Move the selected entry up/down one slot, persist, and repaint. Returns false
  // (no-op) at the list boundary. Driven by Left/Right taps — a tap, not a hold,
  // because the X3 front buttons can't sustain a reliable held reading.
  bool moveSelectedUp();
  bool moveSelectedDown();

  // Show an OK/Cancel prompt to remove the given book from the Recent Books list.
  void promptRemoveBook(const std::string& path, const std::string& title);
};
