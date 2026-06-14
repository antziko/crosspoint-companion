#include "HighlightActionActivity.h"

#include <I18n.h>

#include <algorithm>

#include "HalDisplay.h"
#include "components/UITheme.h"

HighlightActionActivity::HighlightActionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                 std::string quoteText)
    : Activity("HighlightAction", renderer, mappedInput), quoteText(std::move(quoteText)) {}

void HighlightActionActivity::onEnter() {
  Activity::onEnter();

  // Back is still held from the launching hold-Back gesture; swallow its release once.
  swallowBackRelease = mappedInput.isPressed(MappedInputManager::Button::Back);

  lineHeight = renderer.getLineHeight(fontId);
  const int maxWidth = renderer.getScreenWidth() - (margin * 2);

  // Line budget = screen minus top/bottom margins, the heading/body gap, and the bottom
  // button-hint strip, so the wrapped quote can't ride under the hints.
  const int buttonHintsHeight = UITheme::getInstance().getMetrics().buttonHintsHeight;
  const int available = renderer.getScreenHeight() - (margin * 2) - buttonHintsHeight - spacing;
  const int maxLines = std::max(1, available / lineHeight);

  headingLines = renderer.wrappedText(fontId, tr(STR_HIGHLIGHT_EXISTS), maxWidth, maxLines, EpdFontFamily::BOLD);

  if (!quoteText.empty()) {
    const int bodyBudget = std::max(1, maxLines - static_cast<int>(headingLines.size()));
    bodyLines = renderer.wrappedText(fontId, quoteText.c_str(), maxWidth, bodyBudget, EpdFontFamily::REGULAR);
  }

  int totalHeight = static_cast<int>(headingLines.size() + bodyLines.size()) * lineHeight;
  if (!headingLines.empty() && !bodyLines.empty()) totalHeight += spacing;

  startY = std::max(margin, (renderer.getScreenHeight() - totalHeight) / 2);

  requestUpdate(true);
}

void HighlightActionActivity::render(RenderLock&& lock) {
  renderer.clearScreen();

  int currentY = startY;
  for (const auto& line : headingLines) {
    renderer.drawCenteredText(fontId, currentY, line.c_str(), true, EpdFontFamily::BOLD);
    currentY += lineHeight;
  }
  if (!headingLines.empty() && !bodyLines.empty()) currentY += spacing;
  for (const auto& line : bodyLines) {
    renderer.drawCenteredText(fontId, currentY, line.c_str(), true, EpdFontFamily::REGULAR);
    currentY += lineHeight;
  }

  // Back = Cancel, Left = Delete, Right = Add new highlight.
  const auto labels = mappedInput.mapLabels(I18N.get(StrId::STR_CANCEL), "", I18N.get(StrId::STR_DELETE),
                                            I18N.get(StrId::STR_ADD_HIGHLIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

void HighlightActionActivity::finishWith(int action) {
  setResult(MenuResult{action});
  finish();
}

void HighlightActionActivity::loop() {
  // Consume the launching hold's Back release without cancelling.
  if (swallowBackRelease) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Back)) swallowBackRelease = false;
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult res;
    res.isCancelled = true;
    setResult(std::move(res));
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    finishWith(ACTION_DELETE);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    finishWith(ACTION_ADD_NEW);
    return;
  }
}
