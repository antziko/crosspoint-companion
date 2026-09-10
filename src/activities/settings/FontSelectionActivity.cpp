#include "FontSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Hold Confirm at least this long to pin/unpin (vs a tap, which commits the font).
constexpr unsigned long kPinHoldMs = 600;
// Gap between the panes and between the panes and the list.
constexpr int kPaneGap = 2;
// Combined compare-pane region as a % of the usable area (two stacked panes share it).
constexpr int kPanesPercent = 40;
}  // namespace

FontSelectionActivity::FontSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             const SdCardFontRegistry* registry, uint8_t currentBuiltinFamily,
                                             std::string currentSdFamilyName)
    : Activity("FontSelect", renderer, mappedInput),
      registry_(registry),
      currentBuiltinFamily_(currentBuiltinFamily),
      currentSdFamilyName_(std::move(currentSdFamilyName)) {}

void FontSelectionActivity::onEnter() {
  Activity::onEnter();

  // Cache layout dims so loop() (page sizing) and render() agree without recompute.
  const auto& metrics = UITheme::getInstance().getMetrics();
  afterHeader_ = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int bottomReserved = metrics.buttonHintsHeight + metrics.verticalSpacing;
  usableHeight_ = renderer.getScreenHeight() - afterHeader_ - bottomReserved;
  panesHeight_ = usableHeight_ * kPanesPercent / 100;

  pane_.build(registry_, currentBuiltinFamily_, currentSdFamilyName_.c_str());

  requestUpdate();
}

void FontSelectionActivity::onExit() {
  // Restore the user's actual resident SD font if a preview swapped it out.
  pane_.restore(renderer);
  Activity::onExit();
}

void FontSelectionActivity::handleSelection() {
  const auto& font = pane_.highlighted();
  FontSelectionResult result;
  if (font.isBuiltin) {
    result.isBuiltin = true;
    result.builtinIndex = font.settingIndex;
  } else {
    result.isBuiltin = false;
    result.sdFamilyName = font.name;
  }
  setResult(ActivityResult{std::move(result)});
  finish();
}

void FontSelectionActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    // Preview never mutated SETTINGS, so there is nothing to restore — just exit.
    finish();
    return;
  }

  // Confirm: a tap commits the highlighted font (preview is live); a hold pins/unpins it. Arm only
  // on a press seen *inside* this activity, so the release of the press that opened the screen is
  // ignored (otherwise it commits + exits on the first loop).
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) confirmArmed_ = true;
  if (confirmArmed_ && mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
    if (!pinFiredThisHold_ && mappedInput.getHeldTime() > kPinHoldMs) {
      pane_.togglePinSelected();
      pinFiredThisHold_ = true;
      requestUpdate();
    }
  }
  // A tap picks a row and commits it, the touch counterpart of the Confirm release
  // below. Never the pin hold: a tap carries no hold time.
  int tapX = 0;
  int tapY = 0;
  if (mappedInput.wasScreenTapped(tapX, tapY) && pane_.selectAtPoint(renderer, tapX, tapY)) {
    handleSelection();
    return;
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

  // Hold the highlight until the requested preview has rendered. Confirm/Back above stay responsive;
  // only up/down is gated so input can't outrun the SD preview load.
  if (pane_.navLocked()) return;

  // Two compare panes sit above the list, so reserve their region when sizing a page.
  const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false, panesHeight_ + kPaneGap);

  buttonNavigator_.onNextRelease([this] {
    pane_.moveNext();
    requestUpdate();
  });
  buttonNavigator_.onPreviousRelease([this] {
    pane_.movePrevious();
    requestUpdate();
  });
  buttonNavigator_.onNextContinuous([this, pageItems] {
    pane_.movePageNext(pageItems);
    requestUpdate();
  });
  buttonNavigator_.onPreviousContinuous([this, pageItems] {
    pane_.movePagePrevious(pageItems);
    requestUpdate();
  });
}

void FontSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FONT_FAMILY));

  pane_.renderPanes(renderer, afterHeader_, panesHeight_);

  const int listTop = afterHeader_ + panesHeight_ + kPaneGap;
  const int listHeight = usableHeight_ - panesHeight_ - kPaneGap;

  // Separator between the panes and the list. pageWidth-1: the rightmost valid pixel is width-1.
  renderer.drawLine(0, listTop - kPaneGap / 2, pageWidth - 1, listTop - kPaneGap / 2);

  pane_.renderList(renderer, listTop, listHeight);

  // Preview is live (follows the highlight), so Confirm always commits.
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
