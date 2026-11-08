#pragma once
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "activities/TouchActionBar.h"
#include "fontIds.h"

// Three-answer prompt for "do X, with an optional extra step first":
//   Back / back swipe / [Cancel]   -> ActivityResult::isCancelled = true -> do nothing
//   Left / [Skip]                  -> MenuResult{ACTION_SKIP}            -> do X only
//   Right or Confirm / [primary]   -> MenuResult{ACTION_PRIMARY}         -> extra step, then X
// Bracketed answers are the on-screen boxes on touch boards. Plain yes/no questions
// use ConfirmationActivity instead.
class ChoicePromptActivity final : public Activity {
 public:
  static constexpr int ACTION_SKIP = 0;
  static constexpr int ACTION_PRIMARY = 1;

  ChoicePromptActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string heading, std::string body,
                       std::string primaryLabel);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&& lock) override;

 private:
  void finishWith(int action);
  void cancel();

  std::string heading;
  std::string body;
  std::string primaryLabel;

  static constexpr int margin = 20;
  static constexpr int spacing = 30;
  static constexpr int fontId = UI_10_FONT_ID;

  std::vector<std::string> headingLines;
  std::vector<std::string> bodyLines;
  int startY = 0;
  int lineHeight = 0;
  // Set once Confirm goes down on this screen, so the release of the press that
  // opened the prompt is not taken as an answer.
  bool confirmArmed = false;

  // On-screen answers for touch boards, which draw no button hints (see TouchActionBar).
  TouchActionBar actionBar_;
};
