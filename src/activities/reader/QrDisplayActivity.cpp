#include "QrDisplayActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/QrUtils.h"

void QrDisplayActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void QrDisplayActivity::onExit() { Activity::onExit(); }

void QrDisplayActivity::loop() {
  int x = 0;
  int y = 0;
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
    finish();
    return;
  }
}

void QrDisplayActivity::render(RenderLock&&) {
  renderer.clearScreen();
  auto metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const auto pageWidth = safe.width;

  GUI.drawHeader(renderer, UITheme::getInstance().getSafeHeaderRect(renderer), tr(STR_DISPLAY_QR), nullptr);

  const int availableWidth = pageWidth - 40;
  const int availableHeight =
      safe.height - metrics.topPadding - metrics.headerHeight - metrics.verticalSpacing * 2 - 40;
  const int startY = safe.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  const Rect qrBounds(safe.x + 20, startY, availableWidth, availableHeight);
  QrUtils::drawQrCode(renderer, qrBounds, textPayload);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
