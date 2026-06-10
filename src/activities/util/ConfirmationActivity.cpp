#include "ConfirmationActivity.h"

#include <I18n.h>

#include <algorithm>

#include "HalDisplay.h"
#include "components/UITheme.h"

ConfirmationActivity::ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const std::string& heading, const std::string& body)
    : Activity("Confirmation", renderer, mappedInput), heading(heading), body(body) {}

void ConfirmationActivity::onEnter() {
  Activity::onEnter();

  lineHeight = renderer.getLineHeight(fontId);
  const int maxWidth = renderer.getScreenWidth() - (margin * 2);

  // Wrap heading + body to full text (no 1-line ellipsis). Line budget = screen
  // height minus top/bottom margins, the heading/body gap, and the bottom button-
  // hint strip — so the wrapped block can't ride under the Cancel/Confirm hints.
  const int buttonHintsHeight = UITheme::getInstance().getMetrics().buttonHintsHeight;
  const int available = renderer.getScreenHeight() - (margin * 2) - buttonHintsHeight - spacing;
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

  startY = std::max(margin, (renderer.getScreenHeight() - totalHeight) / 2);

  requestUpdate(true);
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

  // Draw UI Elements
  const auto labels = mappedInput.mapLabels("", "", I18N.get(StrId::STR_CANCEL), I18N.get(StrId::STR_CONFIRM));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

void ConfirmationActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    ActivityResult res;
    res.isCancelled = false;
    setResult(std::move(res));
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    ActivityResult res;
    res.isCancelled = true;
    setResult(std::move(res));
    finish();
    return;
  }
}