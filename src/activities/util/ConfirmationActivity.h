#pragma once
#include <functional>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "fontIds.h"

class ConfirmationActivity : public Activity {
 private:
  // Input data
  std::string heading;
  std::string body;

  // Optional button-label overrides (empty -> defaults: Cancel / Confirm).
  std::string cancelLabel;   // Left button
  std::string confirmLabel;  // Right button

  const int margin = 20;
  const int spacing = 30;
  const int fontId = UI_10_FONT_ID;
  // Vertical padding inside an on-screen button box, above and below the label.
  static constexpr int touchButtonPaddingY = 12;

  // On-screen Cancel / Confirm pair, laid out in onEnter() and hit-tested in loop().
  // Drawn only where the physical button-hint strip is hidden (touch boards): the
  // X4 Pro wires no front buttons at all, so a tap target is the only way to answer
  // the prompt there.
  struct TouchButton {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
  };
  bool touchButtons = false;
  TouchButton cancelButton;
  TouchButton confirmButton;

  void drawTouchButton(const TouchButton& button, const char* label, bool primary) const;
  static bool hitTouchButton(const TouchButton& button, int x, int y);
  void finishWith(bool cancelled);

  std::vector<std::string> headingLines;
  std::vector<std::string> bodyLines;
  int startY = 0;
  int lineHeight = 0;

 public:
  ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& heading,
                       const std::string& body, const std::string& cancelLabel = "",
                       const std::string& confirmLabel = "");

  void onEnter() override;
  void loop() override;
  void render(RenderLock&& lock) override;
};