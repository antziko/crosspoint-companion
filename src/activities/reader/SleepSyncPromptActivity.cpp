#include "SleepSyncPromptActivity.h"

#include <I18n.h>

#include <algorithm>

#include "HalDisplay.h"
#include "components/UITheme.h"

SleepSyncPromptActivity::SleepSyncPromptActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                 std::string heading, std::string body)
    : Activity("SleepSyncPrompt", renderer, mappedInput), heading(std::move(heading)), body(std::move(body)) {}

void SleepSyncPromptActivity::onEnter() {
  Activity::onEnter();

  lineHeight = renderer.getLineHeight(fontId);
  const int maxWidth = renderer.getScreenWidth() - (margin * 2);

  // Line budget = screen minus top/bottom margins, the heading/body gap, and the bottom
  // button-hint strip, so the wrapped text can't ride under the hints.
  const int buttonHintsHeight = UITheme::getInstance().getMetrics().buttonHintsHeight;
  const int available = renderer.getScreenHeight() - (margin * 2) - buttonHintsHeight - spacing;
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

  startY = std::max(margin, (renderer.getScreenHeight() - totalHeight) / 2);

  requestUpdate(true);
}

void SleepSyncPromptActivity::render(RenderLock&& lock) {
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

  // Back = Cancel (stay awake), Left = Skip (sleep, no sync), Right = Sync.
  const auto labels =
      mappedInput.mapLabels(I18N.get(StrId::STR_CANCEL), "", I18N.get(StrId::STR_SKIP), I18N.get(StrId::STR_SYNC));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

void SleepSyncPromptActivity::finishWith(int action) {
  setResult(MenuResult{action});
  finish();
}

void SleepSyncPromptActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult res;
    res.isCancelled = true;
    setResult(std::move(res));
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    finishWith(ACTION_SKIP);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    finishWith(ACTION_SYNC);
    return;
  }
}
