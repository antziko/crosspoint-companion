#include "SettingsListActivity.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SettingsActivity.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "components/icons/listIcons.h"

namespace fui = freeink::ui;

namespace {
// Row icons, indexed by SettingsSection.
constexpr const freeink::Icon* SECTION_ICONS[SETTINGS_SECTION_COUNT] = {
    &icon_settings_32, &icon_smartphone_32, &icon_book_open_32, &icon_pointer_32, &icon_library_32, &icon_wifi_32,
};
}  // namespace

SettingsListActivity::SettingsListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const SettingsSection openSection)
    : UiListActivity("Settings", renderer, mappedInput), pendingSection(openSection) {}

void SettingsListActivity::onEnter() {
  UiListActivity::onEnter();
  rebuildRows();
}

void SettingsListActivity::onExit() {
  UiListActivity::onExit();
  UITheme::getInstance().reload();  // Re-apply theme in case it was changed
}

void SettingsListActivity::loop() {
  if (pendingSection != SettingsSection::End) {
    const SettingsSection section = pendingSection;
    pendingSection = SettingsSection::End;
    openSection(section);
    return;
  }
  UiListActivity::loop();
}

// Rebuilds row labels; call when entering and after a section returns (the
// UI language may have changed).
void SettingsListActivity::rebuildRows() {
  for (size_t i = 0; i < SETTINGS_SECTION_COUNT; ++i) {
    auto& item = rowItems_[i];
    item.label = I18N.get(SETTINGS_SECTION_TITLES[i]);
    item.icon = fui::bitmapFromIcon(*SECTION_ICONS[i]);
    item.actionValue = static_cast<int16_t>(i);
  }
}

void SettingsListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the header band, clear of the button hints in any orientation.
  screen.setContentMargin(safeContentInsets(renderer, metrics.topPadding + metrics.headerHeight));
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  syncListViewport(screen, props);
  screen.list(props);
}

void SettingsListActivity::activateIndex(const int index) {
  // The section screen replaces this one; a lingering flash would gray an
  // unrelated element on return.
  app.clearTapFlash();
  openSection(static_cast<SettingsSection>(index));
  // Tap-first: a tapped row is not a cursor position (see SettingsActivity::activateIndex).
  if (mappedInput.hasTouch()) nav.selected = 0;
}

void SettingsListActivity::openSection(const SettingsSection section) {
  nav.selected = static_cast<int>(section);
  auto activity = makeUniqueNoThrow<SettingsActivity>(renderer, mappedInput, section);
  if (!activity) {
    LOG_ERR("SETTINGS", "OOM: SettingsActivity");
    return;
  }
  startActivityForResult(std::move(activity), [this](const ActivityResult&) {
    rebuildRows();
    // A theme change inside the section must restyle this screen too.
    resetUi();
    requestUpdate();
  });
}

void SettingsListActivity::onBackButton() {
  SETTINGS.saveToFile();
  onGoHome();
}

void SettingsListActivity::drawChrome() {
  // Version rides in the header's trailing label slot: the footer position
  // conflicts with button hints on non-touch devices.
  GUI.drawHeader(renderer, UITheme::getInstance().getSafeHeaderRect(renderer), tr(STR_SETTINGS_TITLE),
                 CROSSPOINT_VERSION);
}
