#include "FullScreenMessageActivity.h"

#include <GfxRenderer.h>

#include <algorithm>

#include "fontIds.h"

void FullScreenMessageActivity::onEnter() {
  Activity::onEnter();

  constexpr int margin = 20;
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int maxWidth = renderer.getScreenWidth() - (margin * 2);
  // Wrap to full text (no clip off-screen). No button hints here, so the block
  // may use nearly the whole screen; overflow ellipsizes the last visible line.
  const int maxLines = std::max(1, (renderer.getScreenHeight() - margin * 2) / lineHeight);
  const auto lines = renderer.wrappedText(UI_10_FONT_ID, text.c_str(), maxWidth, maxLines, style);

  int top = (renderer.getScreenHeight() - static_cast<int>(lines.size()) * lineHeight) / 2;
  top = std::max(top, margin);

  renderer.clearScreen();
  for (const auto& line : lines) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, line.c_str(), true, style);
    top += lineHeight;
  }
  renderer.displayBuffer(refreshMode);
}
