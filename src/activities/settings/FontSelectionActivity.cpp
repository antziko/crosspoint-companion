#include "FontSelectionActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr const char* ELLIPSIS_UTF8 = "\xe2\x80\xa6";
// Hold Confirm at least this long to pin/unpin (vs a tap, which commits the font).
constexpr unsigned long kPinHoldMs = 600;
// Gap between the two preview panes and between the panes and the list. Kept minimal
// (the panes already carry kPanePad on each edge) so metrics.verticalSpacing's larger
// value doesn't push the stacked sections apart.
constexpr int kPaneGap = 2;
// Per-pane height as a % of the usable area. Slimmer than the theme's previewHeightPercent
// so the two stacked panes leave more room for the list below.
constexpr int kPanePercent = 20;
// Slim internal padding inside each preview pane (vs the theme's larger previewPadding),
// so the panes are visually tight and short.
constexpr int kPanePad = 4;
}  // namespace

FontSelectionActivity::FontSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             const SdCardFontRegistry* registry, uint8_t currentBuiltinFamily,
                                             std::string currentSdFamilyName)
    : Activity("FontSelect", renderer, mappedInput),
      registry_(registry),
      currentBuiltinFamily_(currentBuiltinFamily),
      currentSdFamilyName_(std::move(currentSdFamilyName)) {}

int FontSelectionActivity::currentSelectionIndex() const {
  // Scan the (pinned-first sorted) fonts_ for the committed selection. Sorting makes
  // the old registry-index arithmetic invalid, so match by identity instead.
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

void FontSelectionActivity::onEnter() {
  Activity::onEnter();

  // Cache layout dims so loop() (page sizing) and render() agree without recompute.
  const auto& metrics = UITheme::getInstance().getMetrics();
  afterHeader_ = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  bottomReserved_ = metrics.buttonHintsHeight + metrics.verticalSpacing;
  usableHeight_ = renderer.getScreenHeight() - afterHeader_ - bottomReserved_;
  previewHeight_ = usableHeight_ * kPanePercent / 100;

  // Build combined font list: built-in + SD card fonts
  fonts_.clear();
  fonts_.reserve(CrossPointSettings::BUILTIN_FONT_COUNT + (registry_ ? registry_->getFamilyCount() : 0));

  fonts_.push_back({I18N.get(StrId::STR_NOTO_SERIF), true, 0, "@b0", false});
  fonts_.push_back({I18N.get(StrId::STR_NOTO_SANS), true, 1, "@b1", false});

  if (registry_) {
    const auto& families = registry_->getFamilies();
    for (int i = 0; i < static_cast<int>(families.size()); i++) {
      fonts_.push_back({families[i].name, false, static_cast<uint8_t>(CrossPointSettings::BUILTIN_FONT_COUNT + i),
                        "@s" + families[i].name, false});
    }
  }

  // Resolve pinned state per entry, then float pinned fonts to the top.
  for (auto& f : fonts_) f.pinned = SETTINGS.isFontPinned(f.key.c_str());
  applyPinSort();

  selectedIndex_ = currentSelectionIndex();
  previewFontIndex_ = selectedIndex_;

  requestUpdate();
}

void FontSelectionActivity::onExit() {
  // Previewing an SD font swaps the single resident family in the font manager. Restore
  // the user's actual selection so backing out never leaves a previewed-but-unchosen font
  // loaded. (On commit the caller re-applies the chosen font; this restore is harmless.)
  if (didLoadPreview_) sdFontSystem.ensureLoaded(renderer);
  Activity::onExit();
}

void FontSelectionActivity::previewSelected() {
  // The preview pane shows the highlighted font. render() loads it on demand, so this
  // only re-targets and locks nav until that render completes. onExit() restores the
  // user's actual selection.
  previewFontIndex_ = selectedIndex_;
  // Block further up/down until this preview has rendered (cleared in render()).
  navLocked_ = true;
  requestUpdate();
}

void FontSelectionActivity::applyPinSort() {
  // stable_sort keeps the original relative order within the pinned and unpinned groups.
  std::stable_sort(fonts_.begin(), fonts_.end(),
                   [](const FontEntry& a, const FontEntry& b) { return a.pinned && !b.pinned; });
}

void FontSelectionActivity::togglePinSelected() {
  if (fonts_.empty()) return;
  const std::string key = fonts_[selectedIndex_].key;  // copy: selectedIndex_ moves after re-sort
  const bool newPinned = !fonts_[selectedIndex_].pinned;
  SETTINGS.setFontPinned(key.c_str(), newPinned);
  SETTINGS.saveToFile();
  fonts_[selectedIndex_].pinned = newPinned;
  applyPinSort();
  // Keep the highlight on the same font after it moves in the re-sort.
  for (int i = 0; i < static_cast<int>(fonts_.size()); i++) {
    if (fonts_[i].key == key) {
      selectedIndex_ = i;
      break;
    }
  }
  previewFontIndex_ = selectedIndex_;
  requestUpdate();
}

void FontSelectionActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    // Preview never mutated SETTINGS, so there is nothing to restore — just exit.
    finish();
    return;
  }

  // Confirm: a tap commits the highlighted font (preview is live); a hold pins/unpins
  // it. The hold fires once mid-press; the matching release must then NOT also commit.
  // Arm only on a press seen *inside* this activity, so the release of the press that
  // opened the screen is ignored (otherwise it commits + exits on the first loop).
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) confirmArmed_ = true;
  if (confirmArmed_ && mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
    if (!pinFiredThisHold_ && mappedInput.getHeldTime() > kPinHoldMs) {
      togglePinSelected();
      pinFiredThisHold_ = true;
    }
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const bool armed = confirmArmed_;
    const bool wasPin = pinFiredThisHold_;
    confirmArmed_ = false;
    pinFiredThisHold_ = false;
    if (armed && !wasPin) {
      handleSelection();
      return;
    }
  }

  // Hold the highlight until the requested preview has rendered. Confirm/Back above
  // stay responsive; only up/down is gated so input can't outrun the SD preview load.
  if (navLocked_) return;

  const int listSize = static_cast<int>(fonts_.size());
  // Two preview panes sit above the list, so reserve both when sizing a page.
  const int pageItems =
      UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false, 2 * (previewHeight_ + kPaneGap));

  buttonNavigator_.onNextRelease([this, listSize] {
    selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, listSize);
    previewSelected();
  });

  buttonNavigator_.onPreviousRelease([this, listSize] {
    selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, listSize);
    previewSelected();
  });

  buttonNavigator_.onNextContinuous([this, listSize, pageItems] {
    selectedIndex_ = ButtonNavigator::nextPageIndex(selectedIndex_, listSize, pageItems);
    previewSelected();
  });

  buttonNavigator_.onPreviousContinuous([this, listSize, pageItems] {
    selectedIndex_ = ButtonNavigator::previousPageIndex(selectedIndex_, listSize, pageItems);
    previewSelected();
  });
}

void FontSelectionActivity::handleSelection() {
  const auto& font = fonts_[selectedIndex_];
  FontSelectionResult result;
  if (font.settingIndex < CrossPointSettings::BUILTIN_FONT_COUNT) {
    result.isBuiltin = true;
    result.builtinIndex = font.settingIndex;
  } else if (registry_) {
    const int sdIdx = font.settingIndex - CrossPointSettings::BUILTIN_FONT_COUNT;
    const auto& families = registry_->getFamilies();
    if (sdIdx < static_cast<int>(families.size())) {
      result.isBuiltin = false;
      result.sdFamilyName = families[sdIdx].name;
    }
  }
  setResult(ActivityResult{std::move(result)});
  finish();
}

int FontSelectionActivity::loadPaneFontId(int index) {
  if (index < 0 || index >= static_cast<int>(fonts_.size())) return 0;
  const auto& font = fonts_[index];
  const uint8_t size = SETTINGS.getReaderFontSize();
  if (font.isBuiltin) {
    // Always resident in flash — no load, no swap of the resident SD family.
    return CrossPointSettings::computeBuiltinFontId(font.settingIndex, size);
  }
  // SD font: make it resident (swaps out any other SD family), returning its fontId.
  // Fast path inside loadFamilyForPreview when it is already the resident family.
  const int id = sdFontSystem.loadFamilyForPreview(font.name.c_str(), size, renderer);
  didLoadPreview_ = true;  // onExit() must restore the user's actual selection
  return id;
}

void FontSelectionActivity::renderPreviewPane(int top, int height, int fontId, const char* label) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int left = metrics.previewPadding;
  const int width = renderer.getScreenWidth() - (metrics.previewPadding * 2);
  if (width <= 0 || height <= 0) return;

  // Caption at the bottom of the pane: the font's name (`label`), so each pane is
  // identifiable. kPanePad (slim) is used for the vertical spacing so panes stay short.
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

void FontSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FONT_FAMILY));

  // Two stacked panes: top = Selected (current committed font), bottom = Preview
  // (highlighted font). Each pane is previewHeight_ tall.
  const int selectedTop = afterHeader_;
  const int previewTop = selectedTop + previewHeight_ + kPaneGap;
  const int listTop = previewTop + previewHeight_ + kPaneGap;
  const int listHeight = usableHeight_ - 2 * (previewHeight_ + kPaneGap);

  const int selectedIdx = currentSelectionIndex();
  const int fontsSize = static_cast<int>(fonts_.size());
  const char* selectedName = (selectedIdx >= 0 && selectedIdx < fontsSize) ? fonts_[selectedIdx].name.c_str() : "";
  const char* previewName =
      (previewFontIndex_ >= 0 && previewFontIndex_ < fontsSize) ? fonts_[previewFontIndex_].name.c_str() : "";

  // Top (Selected): load the committed font FIRST — its pixels land in the framebuffer
  // before the Preview load below can swap out the resident SD family, so only one SD
  // family is ever resident at a time. Caption is the font's own name.
  const int selectedFontId = loadPaneFontId(selectedIdx);
  renderPreviewPane(selectedTop, previewHeight_, selectedFontId, selectedName);

  // Bottom (Preview): load the highlighted font second (may swap out the Selected SD font,
  // now already drawn). Leaves the highlighted font resident for the next preview.
  const int previewFontId = loadPaneFontId(previewFontIndex_);
  renderPreviewPane(previewTop, previewHeight_, previewFontId, previewName);

  // Separator between the preview panes and the list. pageWidth-1: the rightmost valid
  // pixel is width-1; passing pageWidth draws one past the edge and spams "Outside range"
  // after the orientation rotate.
  renderer.drawLine(0, listTop - kPaneGap / 2, pageWidth - 1, listTop - kPaneGap / 2);

  const int currentFontIndex = selectedIdx;
  GUI.drawList(
      renderer, Rect{0, listTop, pageWidth, listHeight}, static_cast<int>(fonts_.size()), selectedIndex_,
      [this](int index) -> std::string {
        // Mark pinned fonts (they sort to the top) with a leading bullet.
        const auto& f = fonts_[index];
        return f.pinned ? ("* " + f.name) : f.name;
      },
      nullptr, nullptr,
      [currentFontIndex](int index) -> std::string {
        // Preview is live (highlight = previewed), so only tag the committed selection.
        return index == currentFontIndex ? tr(STR_SELECTED) : "";
      },
      true);

  // Preview is live (follows the highlight), so Confirm always commits.
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();

  // The requested preview is now on screen — accept up/down again.
  navLocked_ = false;
}
