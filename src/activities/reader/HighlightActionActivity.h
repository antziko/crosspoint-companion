#pragma once
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "fontIds.h"

// Shown when the user triggers "highlight" (hold-Back) on a reader page that already
// has a highlight. Displays the existing highlight's text and offers three outcomes:
//   Back  -> Cancel    (ActivityResult::isCancelled = true)
//   Left  -> Delete    (MenuResult{action = ACTION_DELETE})
//   Right -> Add new   (MenuResult{action = ACTION_ADD_NEW})
// Modeled on ConfirmationActivity's wrapped-text layout; kept separate because that
// component is a fixed two-button (Cancel/Confirm) dialog used by ~12 callers.
class HighlightActionActivity final : public Activity {
 public:
  static constexpr int ACTION_DELETE = 0;
  static constexpr int ACTION_ADD_NEW = 1;

  HighlightActionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string quoteText);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&& lock) override;

 private:
  void finishWith(int action);

  std::string quoteText;

  static constexpr int margin = 20;
  static constexpr int spacing = 30;
  static constexpr int fontId = UI_10_FONT_ID;

  std::vector<std::string> headingLines;
  std::vector<std::string> bodyLines;
  int startY = 0;
  int lineHeight = 0;

  // Launched mid hold-Back: swallow the release that ends that launching hold so it
  // doesn't instantly cancel the dialog (mirrors DictionaryWordSelectActivity).
  bool swallowBackRelease = false;
};
