#pragma once
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "fontIds.h"

// Shown on a manual power-button sleep gesture from the reader when a KOReader sync is
// due. Offers three outcomes:
//   Back  -> Cancel  (ActivityResult::isCancelled = true)  -> abort sleep, stay reading
//   Left  -> Skip    (MenuResult{action = ACTION_SKIP})    -> sleep now, no sync
//   Right -> Sync    (MenuResult{action = ACTION_SYNC})    -> sync then sleep
// Modeled on ConfirmationActivity's wrapped-text layout; kept separate because that
// component is a fixed two-button (Cancel/Confirm) dialog used by many callers.
class SleepSyncPromptActivity final : public Activity {
 public:
  static constexpr int ACTION_SKIP = 0;
  static constexpr int ACTION_SYNC = 1;

  SleepSyncPromptActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string heading,
                          std::string body);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&& lock) override;

 private:
  void finishWith(int action);

  std::string heading;
  std::string body;

  static constexpr int margin = 20;
  static constexpr int spacing = 30;
  static constexpr int fontId = UI_10_FONT_ID;

  std::vector<std::string> headingLines;
  std::vector<std::string> bodyLines;
  int startY = 0;
  int lineHeight = 0;
};
