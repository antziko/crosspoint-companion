#pragma once

#include <SdCardFontRegistry.h>

#include <string>
#include <vector>

#include "activities/ListTouchTarget.h"

class GfxRenderer;

// Reusable two-pane font comparison + pinnable font list.
//
// Extracted from FontSelectionActivity so the same battle-tested widget backs two hosts:
//   * FontSelectionActivity      — per-book font override (reads highlighted() into a result)
//   * TextSettingsActivity::Font  — global Text Settings Font tab (#2605; live-applies)
//
// The pane owns the font list (built-in + SD, pinned-first), the highlight/committed indices,
// the nav-lock that gates input against slow SD preview loads, and the single-resident-SD-font
// bookkeeping (loading a highlighted SD font swaps the resident family; restore() puts it back).
//
// Layout is caller-driven: renderPanes()/renderList() take pixel regions, so each host places
// the two compare panes and the list wherever its own screen layout dictates.
class FontComparePane {
 public:
  struct FontEntry {
    std::string name;
    bool isBuiltin;
    uint8_t settingIndex;  // built-in family index, or BUILTIN_FONT_COUNT + SD index
    std::string key;       // pin key: "@b<index>" (built-in) or "@s<familyName>" (SD)
    bool pinned = false;
  };

  // Build the combined (pinned-first) font list and select the current committed font.
  void build(const SdCardFontRegistry* registry, uint8_t currentBuiltinFamily, const char* currentSdFamilyName);

  // Two stacked compare panes in [top, top+height): top = committed font, bottom = highlighted.
  // Loads the highlighted font on demand (may swap the resident SD family) and clears the nav-lock.
  void renderPanes(GfxRenderer& renderer, int top, int height);
  // The font list within [listTop, listTop+listHeight): pinned entries float to the top with a
  // '*' marker; the committed font is tagged [Selected].
  void renderList(GfxRenderer& renderer, int listTop, int listHeight) const;

  // Make the highlighted font resident (SD swap) and return its fontId WITHOUT drawing the
  // compare panes — for hosts that render their own single preview (e.g. the in-reader
  // book-text preview). Clears the nav-lock, exactly as renderPanes does once the requested
  // preview would be on screen.
  int loadHighlightedFontId(GfxRenderer& renderer);

  // Highlight movement. Each arms the nav-lock until the next renderPanes() so held/rapid input
  // cannot outrun the (slow, SD-loading) bottom preview pane.
  void moveNext();
  void movePrevious();
  void movePageNext(int pageItems);
  void movePagePrevious(int pageItems);
  // Point the highlight at an absolute row (host nav rings map their position here). Arms nav-lock.
  void setHighlight(int index);
  bool navLocked() const { return navLocked_; }

  // Move the highlight to the row under (x, y). Returns true when a row was hit, so
  // the owning activity can decide whether the same tap also commits the choice.
  // Both owners (Font Family, and the reader's inline picker) drive input themselves,
  // so the pane only resolves the hit.
  bool selectAtPoint(const GfxRenderer& renderer, int x, int y);

  // Pin/unpin the highlighted font, persist, re-sort pinned-first, keep the highlight on it.
  void togglePinSelected();

  // Mark the highlighted font as committed (host has applied it) — moves the top pane onto it.
  void commitHighlighted() { committedIndex_ = selectedIndex_; }

  // Restore the user's actual resident SD font. Call from the host's onExit().
  void restore(GfxRenderer& renderer);

  bool empty() const { return fonts_.empty(); }
  int size() const { return static_cast<int>(fonts_.size()); }
  int highlightedIndex() const { return selectedIndex_; }
  const FontEntry& highlighted() const { return fonts_[selectedIndex_]; }
  const FontEntry& committed() const { return fonts_[committedIndex_]; }

 private:
  int loadPaneFontId(GfxRenderer& renderer, int index);
  void renderPreviewPane(GfxRenderer& renderer, int top, int height, int fontId, const char* label) const;
  void applyPinSort();
  int currentSelectionIndex() const;

  std::vector<FontEntry> fonts_;
  uint8_t currentBuiltinFamily_ = 0;
  std::string currentSdFamilyName_;
  int selectedIndex_ = 0;   // highlighted row (bottom pane / list cursor)
  int committedIndex_ = 0;  // committed row (top pane / [Selected] tag)
  // Set once a preview has loaded a non-resident SD family into the font manager, so restore()
  // knows to reload the user's actual selection.
  bool didLoadPreview_ = false;
  // Nav-lock: a navigation step arms this; renderPanes() clears it once the requested preview is
  // on screen. While armed the host must not advance the highlight (stops overshoot).
  bool navLocked_ = false;

  // Rows the last render drew, so a tap can pick one (see ListTouchTarget). Mutable
  // because render() is const here: this is a record of what was painted, not state
  // the pane reasons about.
  mutable ListTouchTarget listTouch_;
};
