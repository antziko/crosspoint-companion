#include "SyncScopeSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "activities/ActivityResult.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int MENU_ITEM_COUNT = 6;

constexpr SyncScope kScopes[MENU_ITEM_COUNT] = {SyncScope::All,   SyncScope::Progress, SyncScope::Bookmarks,
                                                SyncScope::Stats, SyncScope::Dict,     SyncScope::Flashcards};
constexpr StrId kLabels[MENU_ITEM_COUNT] = {StrId::STR_SYNC_SCOPE_ALL,       StrId::STR_SYNC_SCOPE_PROGRESS,
                                            StrId::STR_SYNC_SCOPE_BOOKMARKS, StrId::STR_SYNC_SCOPE_STATS,
                                            StrId::STR_SYNC_SCOPE_DICT,      StrId::STR_SYNC_SCOPE_FLASHCARDS};
constexpr StrId kDescs[MENU_ITEM_COUNT] = {StrId::STR_SYNC_SCOPE_ALL_DESC,       StrId::STR_SYNC_SCOPE_PROGRESS_DESC,
                                           StrId::STR_SYNC_SCOPE_BOOKMARKS_DESC, StrId::STR_SYNC_SCOPE_STATS_DESC,
                                           StrId::STR_SYNC_SCOPE_DICT_DESC,      StrId::STR_SYNC_SCOPE_FLASHCARDS_DESC};
constexpr UIIcon kIcons[MENU_ITEM_COUNT] = {UIIcon::Transfer, UIIcon::Book,    UIIcon::BookmarkRibbon,
                                            UIIcon::Chart,    UIIcon::Library, UIIcon::Recent};
}  // namespace

void SyncScopeSelectionActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  requestUpdate();
}

void SyncScopeSelectionActivity::onExit() { Activity::onExit(); }

void SyncScopeSelectionActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  // A tap on a row selects and activates it in one go, like the FUI list screens.
  int tapX = 0;
  int tapY = 0;
  const int tappedRow = mappedInput.wasScreenTapped(tapX, tapY) ? listTouch_.indexAt(renderer, tapX, tapY) : -1;
  if (tappedRow >= 0) selectedIndex = tappedRow;

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) || tappedRow >= 0) {
    setResult(SyncScopeResult{kScopes[selectedIndex]});
    finish();
    return;
  }

  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, MENU_ITEM_COUNT);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, MENU_ITEM_COUNT);
    requestUpdate();
  });
}

void SyncScopeSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_KOREADER_SYNC));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;

  listTouch_.record(Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(MENU_ITEM_COUNT), selectedIndex,
                    /*hasSubtitle=*/true);
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(MENU_ITEM_COUNT), selectedIndex,
      [](int index) { return std::string(I18N.get(kLabels[index])); },
      [](int index) { return std::string(I18N.get(kDescs[index])); }, [](int index) { return kIcons[index]; });

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
