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