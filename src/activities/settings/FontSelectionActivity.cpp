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
}  // namespace

FontSelectionActivity::FontSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             const SdCardFontRegistry* registry, uint8_t currentBuiltinFamily,
                                             std::string currentSdFamilyName)
    : Activity("FontSelect", renderer, mappedInput),
      registry_(registry),
      currentBuiltinFamily_(currentBuiltinFamily),
      currentSdFamilyName_(std::move(currentSdFamilyName)) {}

int FontSelectionActivity::currentSelectionIndex() const {
  if (!currentSdFamilyName_.empty() && registry_) {
    const auto& families = registry_->getFamilies();
    for (int i = 0; i < static_cast<int>(families.size()); i++) {
      if (families[i].name == currentSdFamilyName_) {
        return CrossPointSettings::BUILTIN_FONT_COUNT + i;
      }
    }
  }
  return currentBuiltinFamily_ < CrossPointSettings::BUILTIN_FONT_COUNT ? currentBuiltinFamily_ : 0;
}

void FontSelectionActivity::onEnter() {
  Activity::onEnter();

  // Cache layout dims so loop() (page sizing) and render() agree without recompute.
  const auto& metrics = UITheme::getInstance().getMetrics();
  afterHeader_ = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  bottomReserved_ = metrics.buttonHintsHeight + metrics.verticalSpacing;
  usableHeight_ = renderer.getScreenHeight() - afterHeader_ - bottomReserved_;
  previewHeight_ = usableHeight_ * metrics.previewHeightPercent / 100;

  // Build combined font list: built-in + SD card fonts
  fonts_.clear();
  fonts_.reserve(CrossPointSettings::BUILTIN_FONT_COUNT + (registry_ ? registry_->getFamilyCount() : 0));

  fonts_.push_back({I18N.get(StrId::STR_NOTO_SERIF), true, 0});
  fonts_.push_back({I18N.get(StrId::STR_NOTO_SANS), true, 1});

  if (registry_) {
    const auto& families = registry_->getFamilies();
    for (int i = 0; i < static_cast<int>(families.size()); i++) {
      fonts_.push_back({families[i].name, false, static_cast<uint8_t>(CrossPointSettings::BUILTIN_FONT_COUNT + i)});
    }
  }

  selectedIndex_ = currentSelectionIndex();
  previewFontIndex_ = selectedIndex_;

  requestUpdate();
}

void FontSelectionActivity::onExit() { Activity::onExit(); }

void FontSelectionActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    // Preview never mutated SETTINGS, so there is nothing to restore — just exit.
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (selectedIndex_ == previewFontIndex_) {
      handleSelection();  // second press on the previewed font commits it
    } else {
      previewFontIndex_ = selectedIndex_;  // first press previews the highlighted font
      requestUpdate();
    }
    return;
  }

  const int listSize = static_cast<int>(fonts_.size());
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageItems =
      UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false, previewHeight_ + metrics.verticalSpacing);

  buttonNavigator_.onNextRelease([this, listSize] {
    selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, listSize);
    requestUpdate();
  });

  buttonNavigator_.onPreviousRelease([this, listSize] {
    selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, listSize);
    requestUpdate();
  });

  buttonNavigator_.onNextContinuous([this, listSize, pageItems] {
    selectedIndex_ = ButtonNavigator::nextPageIndex(selectedIndex_, listSize, pageItems);
    requestUpdate();
  });

  buttonNavigator_.onPreviousContinuous([this, listSize, pageItems] {
    selectedIndex_ = ButtonNavigator::previousPageIndex(selectedIndex_, listSize, pageItems);
    requestUpdate();
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

int FontSelectionActivity::getFontIdForPreview(int index) const {
  if (index < 0 || index >= static_cast<int>(fonts_.size())) return 0;
  const auto& font = fonts_[index];
  const uint8_t size = SETTINGS.getReaderFontSize();
  if (font.isBuiltin) {
    return CrossPointSettings::computeBuiltinFontId(font.settingIndex, size);
  }
  // SD font: resolve without touching SETTINGS; 0 if not currently loaded.
  return sdFontSystem.resolveFontId(font.name.c_str(), size);
}

void FontSelectionActivity::renderPreviewPane(int top, int height, int fontId, const char* fontName) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int left = metrics.previewPadding;
  const int width = renderer.getScreenWidth() - (metrics.previewPadding * 2);
  if (width <= 0 || height <= 0) return;

  const int labelFontId = UI_10_FONT_ID;
  const int labelH = renderer.getTextHeight(labelFontId);
  const int labelGap = 4;
  const int labelReserved = labelH + labelGap + metrics.previewPadding;

  char labelBuf[128];
  snprintf(labelBuf, sizeof(labelBuf), "%s \"%s\"", tr(STR_PREVIEW), fontName ? fontName : "");
  const int labelY = top + height - metrics.previewPadding - labelH;
  renderer.drawText(labelFontId, left, labelY, labelBuf);

  if (fontId == 0) return;

  const int lineH = renderer.getTextHeight(fontId);
  if (lineH <= 0) return;

  const int innerHeight = height - metrics.previewPadding - labelReserved;
  const int maxLines = std::max(1, innerHeight / (lineH + 2));

  const char* previewText = I18N.get(StrId::STR_FONT_PREVIEW_TEXT);
  if (auto* fcm = renderer.getFontCacheManager()) {
    char prewarmBuf[256];
    snprintf(prewarmBuf, sizeof(prewarmBuf), "%s %s", previewText, ELLIPSIS_UTF8);
    fcm->prewarmCache(fontId, prewarmBuf, 0x01);
  }

  const auto lines = renderer.wrappedText(fontId, previewText, width, maxLines);

  int y = top + metrics.previewPadding;
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

  const int previewTop = afterHeader_;
  const int listTop = previewTop + previewHeight_ + metrics.verticalSpacing;
  const int listHeight = usableHeight_ - previewHeight_ - metrics.verticalSpacing;

  // Preview pane renders the highlighted font without committing it.
  const int previewFontId = getFontIdForPreview(previewFontIndex_);
  const char* previewFontName = (previewFontIndex_ >= 0 && previewFontIndex_ < static_cast<int>(fonts_.size()))
                                    ? fonts_[previewFontIndex_].name.c_str()
                                    : nullptr;
  renderPreviewPane(previewTop, previewHeight_, previewFontId, previewFontName);

  renderer.drawLine(0, listTop - metrics.verticalSpacing / 2, pageWidth, listTop - metrics.verticalSpacing / 2);

  const int currentFontIndex = currentSelectionIndex();
  GUI.drawList(
      renderer, Rect{0, listTop, pageWidth, listHeight}, static_cast<int>(fonts_.size()), selectedIndex_,
      [this](int index) { return fonts_[index].name; }, nullptr, nullptr,
      [this, currentFontIndex](int index) -> std::string {
        if (index == previewFontIndex_ && index != currentFontIndex) return tr(STR_PREVIEW);
        if (index == currentFontIndex) return tr(STR_SELECTED);
        return "";
      },
      true);

  const bool onPreviewed = selectedIndex_ == previewFontIndex_;
  const char* confirmLabel = onPreviewed ? tr(STR_SELECT) : tr(STR_PREVIEW);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
