#include "ConfirmationActivity.h"

#include <I18n.h>

#include <algorithm>

#include "HalDisplay.h"
#include "components/UITheme.h"

ConfirmationActivity::ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const std::string& heading, const std::string& body,
                                           const std::string& cancelLabel, const std::string& confirmLabel)
    : Activity("Confirmation", renderer, mappedInput),
      heading(heading),
      body(body),
      cancelLabel(cancelLabel),
      confirmLabel(confirmLabel) {}

void ConfirmationActivity::onEnter() {
  Activity::onEnter();

  lineHeight = renderer.getLineHeight(fontId);
  const int maxWidth = renderer.getScreenWidth() - (margin * 2);

  // Touch boards hide the physical button-hint strip (BaseTheme::drawButtonHints
  // returns early there), so the prompt draws its own Cancel / Confirm pair. On the
  // X4 Pro, which wires no front buttons at all, that pair is the only way to answer.
  touchButtons = mappedInput.hasTouch();
  const int touchButtonHeight = touchButtons ? lineHeight + touchButtonPaddingY * 2 : 0;

  // Wrap heading + body to full text (no 1-line ellipsis). Line budget = screen
  // height minus top/bottom margins, the heading/body gap, and whichever bottom
  // strip is in use — so the wrapped block can't ride under the Cancel/Confirm
  // hints or the on-screen buttons.
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int bottomReserved = metrics.buttonHintsHeight + touchButtonHeight;
  const int available = renderer.getScreenHeight() - (margin * 2) - bottomReserved - spacing;
  const int maxLines = std::max(1, available / lineHeight);

  if (!heading.empty()) {
    headingLines = renderer.wrappedText(fontId, heading.c_str(), maxWidth, maxLines, EpdFontFamily::BOLD);
  }
  if (!body.empty()) {
    // Body shares the budget with whatever the heading already consumed.
    const int bodyBudget = std::max(1, maxLines - static_cast<int>(headingLines.size()));
    bodyLines = renderer.wrappedText(fontId, body.c_str(), maxWidth, bodyBudget, EpdFontFamily::REGULAR);
  }

  int totalHeight = static_cast<int>(headingLines.size() + bodyLines.size()) * lineHeight;
  if (!headingLines.empty() && !bodyLines.empty()) totalHeight += spacing;

  // Centre in the band above the on-screen buttons, so the text block cannot slide under
  // them. Without them (button boards) this is the plain screen centre, as before.
  const int layoutHeight = renderer.getScreenHeight() - (touchButtons ? touchButtonHeight + spacing : 0);
  startY = std::max(margin, (layoutHeight - totalHeight) / 2);

  if (touchButtons) {
    // Two equal boxes side by side across the content width, bottom-anchored — the same
    // arrangement addDialogCancelOk() gives the FUI dialogs.
    const int screenWidth = renderer.getScreenWidth();
    const int inset = std::max(margin, metrics.contentSidePadding);
    const int width = std::max(1, (screenWidth - inset * 2 - spacing) / 2);
    const int y = renderer.getScreenHeight() - margin - touchButtonHeight;
    cancelButton = {inset, y, width, touchButtonHeight};
    confirmButton = {screenWidth - inset - width, y, width, touchButtonHeight};
  }

  requestUpdate(true);
}

void ConfirmationActivity::drawTouchButton(const TouchButton& button, const char* label, const bool primary) const {
  const int radius = UITheme::getInstance().getMetrics().controlRadius;
  if (primary) {
    // Filled box with inverted text: the affirmative answer reads as the primary action.
    if (radius > 0) {
      renderer.fillRoundedRect(button.x, button.y, button.w, button.h, radius, Color::Black);
    } else {
      renderer.fillRect(button.x, button.y, button.w, button.h, true);
    }
  } else if (radius > 0) {
    renderer.drawRoundedRect(button.x, button.y, button.w, button.h, 1, radius, true);
  } else {
    renderer.drawRect(button.x, button.y, button.w, button.h);
  }

  const int textWidth = renderer.getTextWidth(fontId, label);
  const int textX = button.x + (button.w - textWidth) / 2;
  const int textY = button.y + (button.h - lineHeight) / 2;
  renderer.drawText(fontId, textX, textY, label, !primary);
}

bool ConfirmationActivity::hitTouchButton(const TouchButton& button, const int x, const int y) {
  return button.w > 0 && x >= button.x && x < button.x + button.w && y >= button.y && y < button.y + button.h;
}

void ConfirmationActivity::finishWith(const bool cancelled) {
  ActivityResult res;
  res.isCancelled = cancelled;
  setResult(std::move(res));
  finish();
}

void ConfirmationActivity::render(RenderLock&& lock) {
  renderer.clearScreen();

  int currentY = startY;

  // Draw heading (wrapped, centered)
  for (const auto& line : headingLines) {
    renderer.drawCenteredText(fontId, currentY, line.c_str(), true, EpdFontFamily::BOLD);
    currentY += lineHeight;
  }

  // Gap between heading and body
  if (!headingLines.empty() && !bodyLines.empty()) currentY += spacing;

  // Draw body (wrapped, centered)
  for (const auto& line : bodyLines) {
    renderer.drawCenteredText(fontId, currentY, line.c_str(), true, EpdFontFamily::REGULAR);
    currentY += lineHeight;
  }

  // Draw UI Elements (label overrides fall back to Cancel / Confirm)
  const char* cancel = cancelLabel.empty() ? I18N.get(StrId::STR_CANCEL) : cancelLabel.c_str();
  const char* confirm = confirmLabel.empty() ? I18N.get(StrId::STR_CONFIRM) : confirmLabel.c_str();
  if (touchButtons) {
    drawTouchButton(cancelButton, cancel, false);
    drawTouchButton(confirmButton, confirm, true);
  }
  const auto labels = mappedInput.mapLabels("", "", cancel, confirm);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

void ConfirmationActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    finishWith(false);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    finishWith(true);
    return;
  }

  // Back — the front button, or the left-edge back gesture on touch boards — answers
  // no. Without it a board with no Left button has no way out of the prompt at all.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finishWith(true);
    return;
  }

  if (touchButtons) {
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasScreenTapped(tx, ty)) {
      if (hitTouchButton(confirmButton, tx, ty)) {
        finishWith(false);
      } else if (hitTouchButton(cancelButton, tx, ty)) {
        finishWith(true);
      }
    }
  }
}
