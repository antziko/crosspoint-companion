#include "ChoicePromptActivity.h"

#include <I18n.h>

#include <algorithm>

#include "HalDisplay.h"
#include "components/UITheme.h"

ChoicePromptActivity::ChoicePromptActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string heading,
                                           std::string body, std::string primaryLabel)
    : Activity("ChoicePrompt", renderer, mappedInput),
      heading(std::move(heading)),
      body(std::move(body)),
      primaryLabel(std::move(primaryLabel)) {}

void ChoicePromptActivity::onEnter() {
  Activity::onEnter();

  lineHeight = renderer.getLineHeight(fontId);
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int maxWidth = safe.width - (margin * 2);

  // Line budget = the hint-safe area minus top/bottom margins and the heading/body gap,
  // so the wrapped text cannot ride under the hints in any orientation.
  actionBar_.layout(renderer, mappedInput.hasTouch(), fontId, 3);
  const int available = safe.height - (margin * 2) - actionBar_.reservedHeight() - spacing;
  const int maxLines = std::max(1, available / lineHeight);

  if (!heading.empty()) {
    headingLines = renderer.wrappedText(fontId, heading.c_str(), maxWidth, maxLines, EpdFontFamily::BOLD);
  }
  if (!body.empty()) {
    const int bodyBudget = std::max(1, maxLines - static_cast<int>(headingLines.size()));
    bodyLines = renderer.wrappedText(fontId, body.c_str(), maxWidth, bodyBudget, EpdFontFamily::REGULAR);
  }

  int totalHeight = static_cast<int>(headingLines.size() + bodyLines.size()) * lineHeight;
  if (!headingLines.empty() && !bodyLines.empty()) totalHeight += spacing;

  startY = safe.y + std::max(margin, (safe.height - totalHeight) / 2);

  requestUpdate(true);
}

void ChoicePromptActivity::render(RenderLock&& lock) {
  renderer.clearScreen();

  const Rect area = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  int currentY = startY;
  for (const auto& line : headingLines) {
    UITheme::drawCenteredText(renderer, area, fontId, currentY, line.c_str(), true, EpdFontFamily::BOLD);
    currentY += lineHeight;
  }
  if (!headingLines.empty() && !bodyLines.empty()) currentY += spacing;
  for (const auto& line : bodyLines) {
    UITheme::drawCenteredText(renderer, area, fontId, currentY, line.c_str(), true, EpdFontFamily::REGULAR);
    currentY += lineHeight;
  }

  const char* cancelLabel = I18N.get(StrId::STR_CANCEL);
  const char* skipLabel = I18N.get(StrId::STR_SKIP);
  const auto labels = mappedInput.mapLabels(cancelLabel, primaryLabel.c_str(), skipLabel, primaryLabel.c_str());
  if (actionBar_.active()) {
    const char* barLabels[] = {cancelLabel, skipLabel, primaryLabel.c_str()};
    actionBar_.draw(renderer, fontId, barLabels, /*primaryIndex=*/2);
  }
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

void ChoicePromptActivity::finishWith(int action) {
  setResult(MenuResult{action});
  finish();
}

void ChoicePromptActivity::cancel() {
  ActivityResult res;
  res.isCancelled = true;
  setResult(std::move(res));
  finish();
}

void ChoicePromptActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    cancel();
    return;
  }
  int tapX = 0;
  int tapY = 0;
  if (mappedInput.wasScreenTapped(tapX, tapY)) {
    switch (actionBar_.hitAt(tapX, tapY)) {
      case 0:
        cancel();
        return;
      case 1:
        finishWith(ACTION_SKIP);
        return;
      case 2:
        finishWith(ACTION_PRIMARY);
        return;
      default:
        break;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    finishWith(ACTION_SKIP);
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) confirmArmed = true;
  if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
      (confirmArmed && mappedInput.wasReleased(MappedInputManager::Button::Confirm))) {
    finishWith(ACTION_PRIMARY);
    return;
  }
}
