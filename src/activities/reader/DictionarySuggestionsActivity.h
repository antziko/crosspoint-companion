#pragma once
#include <string>
#include <vector>

#include "../Activity.h"
#include "activities/ListTouchTarget.h"

class DictionarySuggestionsActivity final : public Activity {
 public:
  explicit DictionarySuggestionsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                         std::vector<std::string> suggestions)
      : Activity("DictionarySuggestions", renderer, mappedInput), suggestions(std::move(suggestions)) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::vector<std::string> suggestions;
  int selectedIndex = 0;

  // Rows the last render drew, so a tap can pick one (see ListTouchTarget).
  ListTouchTarget listTouch_;
};
