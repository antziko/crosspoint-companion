#include "FontComparePane.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "SdCardFontSystem.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ButtonNavigator.h"

namespace {
constexpr const char* ELLIPSIS_UTF8 = "\xe2\x80\xa6";
// Gap between the two panes and slim internal padding, so the stacked panes stay tight/short.
constexpr int kPaneGap = 2;
constexpr int kPanePad = 4;
}  // namespace

void FontComparePane::build(const SdCardFontRegistry* registry, uint8_t currentBuiltinFamily,
                            const char* currentSdFamilyName) {
  currentBuiltinFamily_ = currentBuiltinFamily;
  currentSdFamilyName_ = currentSdFamilyName ? currentSdFamilyName : "";

  fonts_.clear();
  fonts_.reserve(CrossPointSettings::BUILTIN_FONT_COUNT + (registry ? registry->getFamilyCount() : 0));

  fonts_.push_back({I18N.get(StrId::STR_NOTO_SERIF), true, 0, "@b0", false});
  fonts_.push_back({I18N.get(StrId::STR_NOTO_SANS), true, 1, "@b1", false});

  if (registry) {
    const auto& families = registry->getFamilies();
    for (int i = 0; i < static_cast<int>(families.size()); i++) {
      fonts_.push_back({families[i].name, false, static_cast<uint8_t>(CrossPointSettings::BUILTIN_FONT_COUNT + i),
                        "@s" + families[i].name, false});
    }
  }

  // Resolve pinned state per entry, then float pinned fonts to the top.
  for (auto& f : fonts_) f.pinned = SETTINGS.isFontPinned(f.key.c_str());
  applyPinSort();

  selectedIndex_ = currentSelectionIndex();
  committedIndex_ = selectedIndex_;
  navLocked_ = false;
  didLoadPreview_ = false;
}

int FontComparePane::currentSelectionIndex() const {
  // Scan the (pinned-first sorted) fonts_ for the committed selection. Sorting makes registry-index
  // arithmetic invalid, so match by identity instead.
  for (int i = 0; i < static_cast<int>(fonts_.size()); i++) {
    const auto& f = fonts_[i];
    if (!currentSdFamilyName_.empty()) {
      if (!f.isBuiltin && f.name == currentSdFamilyName_) return i;
    } else if (f.isBuiltin && f.settingIndex == currentBuiltinFamily_) {
      return i;
    }
  }
  return 0;
}

void FontComparePane::applyPinSort() {
  // stable_sort keeps the original relative order within the pinned and unpinned groups.
  std::stable_sort(fonts_.begin(), fonts_.end(),
                   [](const FontEntry& a, const FontEntry& b) { return a.pinned && !b.pinned; });
}

void FontComparePane::togglePinSelected() {
  if (fonts_.empty()) return;
  const std::string key = fonts_[selectedIndex_].key;  // copy: selectedIndex_ moves after re-sort
  const bool newPinned = !fonts_[selectedIndex_].pinned;
  SETTINGS.setFontPinned(key.c_str(), newPinned);
  SETTINGS.saveToFile();
  fonts_[selectedIndex_].pinned = newPinned;
  applyPinSort();
  // Keep the highlight (and, if it was the committed row, the committed marker) on the same font.
  const std::string committedKey =
      (committedIndex_ >= 0 && committedIndex_ < size()) ? fonts_[committedIndex_].key : std::string();
  for (int i = 0; i < static_cast<int>(fonts_.size()); i++) {
    if (fonts_[i].key == key) selectedIndex_ = i;
    if (!committedKey.empty() && fonts_[i].key == committedKey) committedIndex_ = i;
  }
}

void FontComparePane::setHighlight(int index) {
  if (fonts_.empty()) return;
  selectedIndex_ = std::clamp(index, 0, size() - 1);
  navLocked_ = true;
}

void FontComparePane::moveNext() { setHighlight(ButtonNavigator::nextIndex(selectedIndex_, size())); }
void FontComparePane::movePrevious() { setHighlight(ButtonNavigator::previousIndex(selectedIndex_, size())); }
void FontComparePane::movePageNext(int pageItems) {
  setHighlight(ButtonNavigator::nextPageIndex(selectedIndex_, size(), pageItems));
}
void FontComparePane::movePagePrevious(int pageItems) {
  setHighlight(ButtonNavigator::previousPageIndex(selectedIndex_, size(), pageItems));
}

int FontComparePane::loadPaneFontId(GfxRenderer& renderer, int index) {
  if (index < 0 || index >= static_cast<int>(fonts_.size())) return 0;
  const auto& font = fonts_[index];
  const uint8_t size = SETTINGS.getReaderFontSize();
  if (font.isBuiltin) {
    // Always resident in flash — no load, no swap of the resident SD family.
    return CrossPointSettings::computeBuiltinFontId(font.settingIndex, size);
  }
  // SD font: make it resident (swaps out any other SD family), returning its fontId. Fast path
  // inside loadFamilyForPreview when it is already the resident family.
  const int id = sdFontSystem.loadFamilyForPreview(font.name.c_str(), size, renderer);
  didLoadPreview_ = true;  // restore() must reload the user's actual selection
  return id;
}

void FontComparePane::renderPreviewPane(GfxRenderer& renderer, int top, int height, int fontId,
                                        const char* label) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int left = metrics.previewPadding;
  const int width = renderer.getScreenWidth() - (metrics.previewPadding * 2);
  if (width <= 0 || height <= 0) return;

  // Caption at the bottom of the pane: the font's name, so each pane is identifiable.
  const int labelFontId = UI_10_FONT_ID;
  const int labelH = renderer.getTextHeight(labelFontId);
  const int labelGap = 2;
  const int labelReserved = labelH + labelGap + kPanePad;
  if (label && *label) {
    renderer.drawText(labelFontId, left, top + height - kPanePad - labelH, label);
  }

  if (fontId == 0) return;

  const int lineH = renderer.getTextHeight(fontId);
  if (lineH <= 0) return;

  const int innerHeight = height - kPanePad - labelReserved;
  const int maxLines = std::max(1, innerHeight / (lineH + 2));

  const char* previewText = I18N.get(StrId::STR_FONT_PREVIEW_TEXT);
  if (auto* fcm = renderer.getFontCacheManager()) {
    char prewarmBuf[256];
    snprintf(prewarmBuf, sizeof(prewarmBuf), "%s %s", previewText, ELLIPSIS_UTF8);
    fcm->prewarmCache(fontId, prewarmBuf, 0x01);
  }

  const auto lines = renderer.wrappedText(fontId, previewText, width, maxLines);

  int y = top + kPanePad;
  const int textBottomLimit = top + height - labelReserved;
  for (const auto& line : lines) {
    if (y + lineH > textBottomLimit) break;
    renderer.drawText(fontId, left, y, line.c_str());
    y += lineH + 2;
  }
}

void FontComparePane::renderPanes(GfxRenderer& renderer, int top, int height) {
  const int paneHeight = (height - kPaneGap) / 2;
  if (paneHeight <= 0) {
    navLocked_ = false;
    return;
  }
  const int fontsSize = static_cast<int>(fonts_.size());
  const int committedTop = top;
  const int previewTop = top + paneHeight + kPaneGap;

  const char* committedName =
      (committedIndex_ >= 0 && committedIndex_ < fontsSize) ? fonts_[committedIndex_].name.c_str() : "";
  const char* previewName =
      (selectedIndex_ >= 0 && selectedIndex_ < fontsSize) ? fonts_[selectedIndex_].name.c_str() : "";

  // Top (committed): load FIRST — its pixels land before the highlighted load below can swap out the
  // resident SD family, so only one SD family is ever resident at a time.
  const int committedFontId = loadPaneFontId(renderer, committedIndex_);
  renderPreviewPane(renderer, committedTop, paneHeight, committedFontId, committedName);

  // Bottom (highlighted): load second (may swap out the committed SD font, now already drawn).
  const int previewFontId = loadPaneFontId(renderer, selectedIndex_);
  renderPreviewPane(renderer, previewTop, paneHeight, previewFontId, previewName);

  // The requested preview is now on screen — accept up/down again.
  navLocked_ = false;
}

int FontComparePane::loadHighlightedFontId(GfxRenderer& renderer) {
  const int id = loadPaneFontId(renderer, selectedIndex_);
  navLocked_ = false;  // the requested preview font is now resident; accept up/down again
  return id;
}

void FontComparePane::renderList(GfxRenderer& renderer, int listTop, int listHeight) const {
  const int committedIndex = committedIndex_;
  listTouch_.record(Rect{0, listTop, renderer.getScreenWidth(), listHeight}, static_cast<int>(fonts_.size()),
                    selectedIndex_);
  GUI.drawList(
      renderer, Rect{0, listTop, renderer.getScreenWidth(), listHeight}, static_cast<int>(fonts_.size()),
      selectedIndex_,
      [this](int index) -> std::string {
        // Mark pinned fonts (they sort to the top) with a leading bullet.
        const auto& f = fonts_[index];
        return f.pinned ? ("* " + f.name) : f.name;
      },
      nullptr, nullptr,
      [committedIndex](int index) -> std::string {
        // Preview is live (highlight = previewed), so only tag the committed selection.
        return index == committedIndex ? tr(STR_SELECTED) : "";
      },
      true);
}

void FontComparePane::restore(GfxRenderer& renderer) {
  // Previewing an SD font swaps the single resident family in the font manager. Restore the user's
  // actual selection so backing out never leaves a previewed-but-unchosen font loaded.
  if (didLoadPreview_) {
    sdFontSystem.ensureLoaded(renderer);
    didLoadPreview_ = false;
  }
}

bool FontComparePane::selectAtPoint(const GfxRenderer& renderer, const int x, const int y) {
  const int hit = listTouch_.touchRow(renderer, x, y);
  if (hit < 0) return false;
  // setHighlight, not a bare assignment: it also takes the nav lock render() clears once
  // the newly highlighted font has been loaded from SD, exactly as a button move does.
  if (hit != selectedIndex_) setHighlight(hit);
  return true;
}
