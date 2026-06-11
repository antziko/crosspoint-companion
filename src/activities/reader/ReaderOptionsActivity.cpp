#include "ReaderOptionsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <cstring>
#include <variant>

#include "../settings/FontSelectionActivity.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReaderSettingsIO.h"
#include "SdCardFontSystem.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

// Item index constants
enum ItemIndex : int {
  FONT_FAMILY = 0,
  FONT_SIZE = 1,
  LINE_SPACING = 2,
  PARA_ALIGNMENT = 3,
  HYPHENATION = 4,
  EXTRA_SPACING = 5,
  MIN_SESSION = 6,
};

// Ordered cycle of valid per-book min-session values: 0xFF = use global, then indices
// into CrossPointSettings::MIN_SESSION_SECONDS (0 = always, 15s, 30s, 1m, 2m, 5m).
static constexpr uint8_t MIN_SESSION_CYCLE[] = {
    CrossPointSettings::ReaderOverride::MIN_SESSION_USE_GLOBAL, 0, 1, 2, 3, 4, 5};
static constexpr int MIN_SESSION_CYCLE_COUNT = static_cast<int>(sizeof(MIN_SESSION_CYCLE));

// Formats a MIN_SESSION_SECONDS index as a short duration label ("Always", "15s", "2 min").
static void formatMinSession(uint8_t idx, char* buf, size_t len) {
  constexpr size_t kCount = sizeof(CrossPointSettings::MIN_SESSION_SECONDS) / sizeof(uint16_t);
  const uint16_t s = (idx < kCount) ? CrossPointSettings::MIN_SESSION_SECONDS[idx] : 0;
  if (s == 0) {
    snprintf(buf, len, "%s", tr(STR_ALWAYS));
  } else if (s < 60) {
    snprintf(buf, len, "%us", static_cast<unsigned>(s));
  } else {
    snprintf(buf, len, "%u min", static_cast<unsigned>(s / 60));
  }
}

}  // namespace

ReaderOptionsActivity::ReaderOptionsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             std::string bookCachePath,
                                             const CrossPointSettings::ReaderOverride& initialOverride,
                                             bool showMinSession)
    : Activity("ReaderOptions", renderer, mappedInput),
      cachePath(std::move(bookCachePath)),
      localOverride(initialOverride),
      showMinSession(showMinSession) {}

void ReaderOptionsActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  requestUpdate();
}

void ReaderOptionsActivity::onExit() { Activity::onExit(); }

void ReaderOptionsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (selectedIndex == FONT_FAMILY) {
      // Font family drills into the full picker (built-in + SD fonts), matching
      // the global Reader Font Family screen, rather than cycling built-ins only.
      openFontFamilyPicker();
    } else {
      cycleCurrentItem();
      requestUpdate();
    }
    return;
  }

  buttonNavigator.onNextRelease([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, itemCount());
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, itemCount());
    requestUpdate();
  });
}

void ReaderOptionsActivity::openFontFamilyPicker() {
  // Reuse the global font picker; it returns a FontSelectionResult that we apply
  // to this book's override (the reader reloads the SD font + reflows on exit).
  startActivityForResult(
      std::make_unique<FontSelectionActivity>(renderer, mappedInput, &sdFontSystem.registry(),
                                              localOverride.fontFamily, std::string(localOverride.sdFontFamilyName)),
      [this](const ActivityResult& result) {
        if (std::holds_alternative<FontSelectionResult>(result.data)) {
          const auto& sel = std::get<FontSelectionResult>(result.data);
          if (sel.isBuiltin) {
            localOverride.fontFamily = sel.builtinIndex;
            localOverride.sdFontFamilyName[0] = '\0';
          } else {
            strncpy(localOverride.sdFontFamilyName, sel.sdFamilyName.c_str(),
                    sizeof(localOverride.sdFontFamilyName) - 1);
            localOverride.sdFontFamilyName[sizeof(localOverride.sdFontFamilyName) - 1] = '\0';
          }
          persistAndApply();
        }
        requestUpdate();
      });
}

void ReaderOptionsActivity::cycleCurrentItem() {
  switch (selectedIndex) {
    case FONT_SIZE:
      localOverride.fontSize =
          (localOverride.fontSize + 1) % static_cast<uint8_t>(CrossPointSettings::FONT_SIZE_COUNT);
      break;
    case LINE_SPACING:
      localOverride.lineSpacing =
          (localOverride.lineSpacing + 1) % static_cast<uint8_t>(CrossPointSettings::LINE_COMPRESSION_COUNT);
      break;
    case PARA_ALIGNMENT:
      localOverride.paragraphAlignment =
          (localOverride.paragraphAlignment + 1) %
          static_cast<uint8_t>(CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT);
      break;
    case HYPHENATION:
      localOverride.hyphenationEnabled = localOverride.hyphenationEnabled ? 0 : 1;
      break;
    case EXTRA_SPACING:
      localOverride.extraParagraphSpacing = localOverride.extraParagraphSpacing ? 0 : 1;
      break;
    case MIN_SESSION: {
      // Find current position in cycle table and advance by one.
      int pos = 0;
      for (int i = 0; i < MIN_SESSION_CYCLE_COUNT; i++) {
        if (MIN_SESSION_CYCLE[i] == localOverride.minSessionMinutes) {
          pos = i;
          break;
        }
      }
      localOverride.minSessionMinutes = MIN_SESSION_CYCLE[(pos + 1) % MIN_SESSION_CYCLE_COUNT];
      break;
    }
    default:
      return;
  }
  persistAndApply();
}

void ReaderOptionsActivity::persistAndApply() {
  localOverride.active = true;
  SETTINGS.setReaderOverride(localOverride);
  if (!ReaderSettingsIO::write(cachePath, localOverride)) {
    LOG_ERR("RO", "Failed to persist per-book reader settings");
  }
}

// static
const char* ReaderOptionsActivity::getItemName(const int index) {
  switch (index) {
    case FONT_FAMILY:
      return tr(STR_FONT_FAMILY);
    case FONT_SIZE:
      return tr(STR_FONT_SIZE);
    case LINE_SPACING:
      return tr(STR_LINE_SPACING);
    case PARA_ALIGNMENT:
      return tr(STR_PARA_ALIGNMENT);
    case HYPHENATION:
      return tr(STR_HYPHENATION);
    case EXTRA_SPACING:
      return tr(STR_EXTRA_SPACING);
    case MIN_SESSION:
      return tr(STR_MIN_SESSION_FOR_STATS);
    default:
      return "";
  }
}

std::string ReaderOptionsActivity::getItemValue(const int index) const {
  switch (index) {
    case FONT_FAMILY: {
      if (localOverride.sdFontFamilyName[0] != '\0') {
        return std::string(localOverride.sdFontFamilyName);
      }
      const StrId labels[] = {StrId::STR_NOTO_SERIF, StrId::STR_NOTO_SANS};
      const uint8_t family = localOverride.fontFamily < CrossPointSettings::BUILTIN_FONT_COUNT
                                 ? localOverride.fontFamily
                                 : 0;
      return I18N.get(labels[family]);
    }
    case FONT_SIZE: {
      const StrId labels[] = {StrId::STR_SMALL, StrId::STR_MEDIUM, StrId::STR_LARGE, StrId::STR_X_LARGE};
      const uint8_t sz =
          localOverride.fontSize < CrossPointSettings::FONT_SIZE_COUNT ? localOverride.fontSize : 0;
      return I18N.get(labels[sz]);
    }
    case LINE_SPACING: {
      const StrId labels[] = {StrId::STR_TIGHT, StrId::STR_NORMAL, StrId::STR_WIDE};
      const uint8_t sp =
          localOverride.lineSpacing < CrossPointSettings::LINE_COMPRESSION_COUNT ? localOverride.lineSpacing : 0;
      return I18N.get(labels[sp]);
    }
    case PARA_ALIGNMENT: {
      const StrId labels[] = {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER,
                               StrId::STR_ALIGN_RIGHT, StrId::STR_BOOK_S_STYLE};
      const uint8_t al = localOverride.paragraphAlignment < CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT
                             ? localOverride.paragraphAlignment
                             : 0;
      return I18N.get(labels[al]);
    }
    case HYPHENATION:
      return localOverride.hyphenationEnabled ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    case EXTRA_SPACING:
      return localOverride.extraParagraphSpacing ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    case MIN_SESSION: {
      const uint8_t v = localOverride.minSessionMinutes;
      if (v == CrossPointSettings::ReaderOverride::MIN_SESSION_USE_GLOBAL) {
        char inner[24];
        formatMinSession(SETTINGS.minSessionMinutes, inner, sizeof(inner));
        char buf[48];
        snprintf(buf, sizeof(buf), "%s (%s)", tr(STR_DEFAULT_VALUE), inner);
        return std::string(buf);
      }
      char buf[24];
      formatMinSession(v, buf, sizeof(buf));
      return std::string(buf);
    }
    default:
      return "";
  }
}

void ReaderOptionsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_READER_OPTIONS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, itemCount(), selectedIndex,
      [](int index) { return std::string(getItemName(index)); }, nullptr, nullptr,
      [this](int index) -> std::string { return getItemValue(index); }, true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_TOGGLE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
