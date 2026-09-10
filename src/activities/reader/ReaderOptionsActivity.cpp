#include "ReaderOptionsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <variant>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReaderFontSizes.h"
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
  SCREEN_MARGIN = 6,
  WORD_SELECT_BUTTONS = 7,
  // MIN_SESSION must stay last: itemCount() hides it by trimming the count by one.
  MIN_SESSION = 8,
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

// Height for the enlarged preview pane: claim the space a compact list (capped to a few
// visible rows, scrolls for the rest) doesn't need, so the sample text dominates the screen.
// Clamped to [1/3, 3/4] of the content area so both panes stay usable.
int enlargedPreviewHeight(int contentHeight, int listRowHeight, int verticalSpacing, int rows) {
  constexpr int kMaxVisibleRows = 5;
  const int visible = std::max(std::min(rows, kMaxVisibleRows), 1);
  const int h = contentHeight - visible * listRowHeight - verticalSpacing;
  return std::clamp(h, contentHeight / 3, contentHeight * 3 / 4);
}

}  // namespace

ReaderOptionsActivity::ReaderOptionsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             std::string bookCachePath,
                                             const CrossPointSettings::ReaderOverride& initialOverride,
                                             bool showMinSession, std::string sampleText)
    : Activity("ReaderOptions", renderer, mappedInput),
      cachePath(std::move(bookCachePath)),
      localOverride(initialOverride),
      showMinSession(showMinSession),
      sampleText(std::move(sampleText)) {}

void ReaderOptionsActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  fullRedraw_ = true;
  requestUpdate();
}

void ReaderOptionsActivity::onExit() { Activity::onExit(); }

void ReaderOptionsActivity::loop() {
  // While the embedded font list is open it owns all input.
  if (fontListOpen_) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      cancelInlineFont();
      return;
    }
    // Arm on a press seen inside the list so the release of the opening press is ignored.
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) fontConfirmArmed_ = true;
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (fontConfirmArmed_) {
        fontConfirmArmed_ = false;
        commitInlineFont();
        return;
      }
    }
    // A tap picks a font and commits it in one go, matching the Confirm release above.
    int tapX = 0;
    int tapY = 0;
    if (mappedInput.wasScreenTapped(tapX, tapY) && fontPane_.selectAtPoint(renderer, tapX, tapY)) {
      commitInlineFont();
      return;
    }
    // Gate up/down until render() has loaded the requested preview font (clears the nav-lock),
    // so held/rapid input can't outrun the slow SD load.
    if (fontPane_.navLocked()) return;
    buttonNavigator.onNextRelease([this] {
      fontPane_.moveNext();
      requestUpdate();
    });
    buttonNavigator.onPreviousRelease([this] {
      fontPane_.movePrevious();
      requestUpdate();
    });
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  // A tap on a row selects and activates it in one go, like the FUI list screens.
  // A tap is never a hold, so it cannot reach the long-press branch above.
  int tapX = 0;
  int tapY = 0;
  const int tappedRow = mappedInput.wasScreenTapped(tapX, tapY) ? listTouch_.indexAt(renderer, tapX, tapY) : -1;
  if (tappedRow >= 0) selectedIndex = tappedRow;

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) || tappedRow >= 0) {
    if (selectedIndex == FONT_FAMILY) {
      openInlineFontList();
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

void ReaderOptionsActivity::openInlineFontList() {
  // Save the current font so Back can restore it; build the pane's list around it.
  savedFontFamily_ = localOverride.fontFamily;
  strncpy(savedSdFontFamilyName_, localOverride.sdFontFamilyName, sizeof(savedSdFontFamilyName_) - 1);
  savedSdFontFamilyName_[sizeof(savedSdFontFamilyName_) - 1] = '\0';
  fontPane_.build(&sdFontSystem.registry(), localOverride.fontFamily, localOverride.sdFontFamilyName);
  fontConfirmArmed_ = false;
  fontListOpen_ = true;
  fullRedraw_ = true;
  requestUpdate();
}

void ReaderOptionsActivity::applyHighlightedFont() {
  const auto& hl = fontPane_.highlighted();
  if (hl.isBuiltin) {
    localOverride.fontFamily = hl.settingIndex;
    localOverride.sdFontFamilyName[0] = '\0';
  } else {
    strncpy(localOverride.sdFontFamilyName, hl.name.c_str(), sizeof(localOverride.sdFontFamilyName) - 1);
    localOverride.sdFontFamilyName[sizeof(localOverride.sdFontFamilyName) - 1] = '\0';
  }
  // Live-apply so getReaderFontId() resolves to the highlighted font for the preview.
  SETTINGS.setReaderOverride(localOverride);
}

void ReaderOptionsActivity::commitInlineFont() {
  applyHighlightedFont();  // ensure localOverride holds the highlighted font
  fontPane_.commitHighlighted();
  persistAndApply();
  fontListOpen_ = false;
  fullRedraw_ = true;
  requestUpdate();
}

void ReaderOptionsActivity::cancelInlineFont() {
  // Restore the pre-picker font and reload the user's actual resident SD font.
  localOverride.fontFamily = savedFontFamily_;
  strncpy(localOverride.sdFontFamilyName, savedSdFontFamilyName_, sizeof(localOverride.sdFontFamilyName) - 1);
  localOverride.sdFontFamilyName[sizeof(localOverride.sdFontFamilyName) - 1] = '\0';
  SETTINGS.setReaderOverride(localOverride);
  fontPane_.restore(renderer);
  fontListOpen_ = false;
  fullRedraw_ = true;
  requestUpdate();
}

void ReaderOptionsActivity::cycleCurrentItem() {
  switch (selectedIndex) {
    case FONT_SIZE: {
      // Cycle through the point sizes the active (per-book) family actually ships.
      // readerFontPointSizes() never returns empty, so the modulo is safe.
      const std::vector<uint8_t> sizes = readerFontPointSizes(&sdFontSystem.registry(), localOverride.sdFontFamilyName);
      const uint8_t current = snapToNearestPointSize(sizes, localOverride.fontPointSize);
      int idx = 0;
      for (int i = 0; i < static_cast<int>(sizes.size()); i++) {
        if (sizes[i] == current) {
          idx = i;
          break;
        }
      }
      localOverride.fontPointSize = sizes[(idx + 1) % sizes.size()];
      break;
    }
    case LINE_SPACING:
      localOverride.lineSpacing =
          (localOverride.lineSpacing + 1) % static_cast<uint8_t>(CrossPointSettings::LINE_COMPRESSION_COUNT);
      break;
    case PARA_ALIGNMENT:
      localOverride.paragraphAlignment =
          (localOverride.paragraphAlignment + 1) % static_cast<uint8_t>(CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT);
      break;
    case HYPHENATION:
      localOverride.hyphenationEnabled = localOverride.hyphenationEnabled ? 0 : 1;
      break;
    case EXTRA_SPACING:
      localOverride.extraParagraphSpacing = localOverride.extraParagraphSpacing ? 0 : 1;
      break;
    case SCREEN_MARGIN: {
      // Step through [MIN, MAX] and wrap; clamp guards a stale/out-of-range stored value.
      const int cur = std::clamp<int>(localOverride.screenMargin, CrossPointSettings::SCREEN_MARGIN_MIN,
                                      CrossPointSettings::SCREEN_MARGIN_MAX);
      int next = cur + CrossPointSettings::SCREEN_MARGIN_STEP;
      if (next > CrossPointSettings::SCREEN_MARGIN_MAX) next = CrossPointSettings::SCREEN_MARGIN_MIN;
      localOverride.screenMargin = static_cast<uint8_t>(next);
      break;
    }
    case WORD_SELECT_BUTTONS:
      localOverride.swapWordSelectAxes = localOverride.swapWordSelectAxes ? 0 : 1;
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
  // Most items cycled here feed the preview. MIN_SESSION (stats-only) and
  // WORD_SELECT_BUTTONS (input mapping) appear nowhere in PreviewKey, so they keep the
  // cheap list-only repaint.
  if (selectedIndex != MIN_SESSION && selectedIndex != WORD_SELECT_BUTTONS) fullRedraw_ = true;
  // A size change must reload the resident SD font at the new size, or getReaderFontId()
  // keeps resolving the old-size id and the live preview never reflows (SD fonts load one
  // size at a time; built-ins are always resident so this is a no-op for them).
  if (selectedIndex == FONT_SIZE) sdFontSystem.ensureLoaded(renderer);
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
    case SCREEN_MARGIN:
      return tr(STR_SCREEN_MARGIN);
    case WORD_SELECT_BUTTONS:
      return tr(STR_WORD_SELECT_BUTTONS);
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
      const uint8_t family =
          localOverride.fontFamily < CrossPointSettings::BUILTIN_FONT_COUNT ? localOverride.fontFamily : 0;
      return I18N.get(labels[family]);
    }
    case FONT_SIZE: {
      // "pt" is the typographic unit symbol — deliberately not translated (matches
      // the reader Text Settings size list).
      char buf[12];
      snprintf(buf, sizeof(buf), "%u pt", localOverride.fontPointSize);
      return std::string(buf);
    }
    case LINE_SPACING: {
      const StrId labels[] = {StrId::STR_TIGHT, StrId::STR_NORMAL, StrId::STR_WIDE};
      const uint8_t sp =
          localOverride.lineSpacing < CrossPointSettings::LINE_COMPRESSION_COUNT ? localOverride.lineSpacing : 0;
      return I18N.get(labels[sp]);
    }
    case PARA_ALIGNMENT: {
      const StrId labels[] = {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT,
                              StrId::STR_BOOK_S_STYLE};
      const uint8_t al = localOverride.paragraphAlignment < CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT
                             ? localOverride.paragraphAlignment
                             : 0;
      return I18N.get(labels[al]);
    }
    case HYPHENATION:
      return localOverride.hyphenationEnabled ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    case EXTRA_SPACING:
      return localOverride.extraParagraphSpacing ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    case SCREEN_MARGIN:
      return std::to_string(localOverride.screenMargin);
    case WORD_SELECT_BUTTONS:
      // Names the pair that steps word by word; the other pair moves between rows.
      return localOverride.swapWordSelectAxes ? tr(STR_WORD_SELECT_SIDE) : tr(STR_WORD_SELECT_FRONT);
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
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Embedded font-family picker: the font list replaces the settings list, and the book-text
  // preview above shows the highlighted font (no separate screen, no two-pane comparison).
  if (fontListOpen_) {
    renderer.clearScreen();
    // Whatever the picker leaves on screen is not the settings view, so the settings view
    // must repaint in full once the picker closes.
    fullRedraw_ = true;
    // Load the highlighted font (resident SD swap) + clear the nav-lock, then live-apply it so
    // the preview renders in that font.
    fontPane_.loadHighlightedFontId(renderer);
    applyHighlightedFont();

    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FONT_FAMILY));
    const int fcTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int fcHeight = pageHeight - fcTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
    int fcListTop = fcTop;
    int fcListHeight = fcHeight;
    if (metrics.previewHeightPercent > 0) {
      const int previewHeight =
          enlargedPreviewHeight(fcHeight, metrics.listRowHeight, metrics.verticalSpacing, fontPane_.size());
      const std::string familyName = getItemValue(FONT_FAMILY);
      const std::string sizeName = getItemValue(FONT_SIZE);
      textsettings::renderPreview(renderer, previewLayout_, metrics.previewPadding, metrics.verticalSpacing, fcTop,
                                  previewHeight, familyName.c_str(), sizeName.c_str(), sampleText.c_str(),
                                  /*showLabel=*/false);
      fcListTop = fcTop + previewHeight + metrics.verticalSpacing;
      fcListHeight = fcHeight - previewHeight - metrics.verticalSpacing;
    }
    fontPane_.renderList(renderer, fcListTop, fcListHeight);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  // Live preview pane above the list (same shared component as the global Text Settings).
  // previewHeightPercent == 0 disables it; then the list uses the full content area.
  // Geometry is resolved before any drawing because the list band is also what a
  // preview-preserving redraw clears.
  int listTop = contentTop;
  int listHeight = contentHeight;
  int previewHeight = 0;
  if (metrics.previewHeightPercent > 0) {
    previewHeight = enlargedPreviewHeight(contentHeight, metrics.listRowHeight, metrics.verticalSpacing, itemCount());
    listTop = contentTop + previewHeight + metrics.verticalSpacing;
    listHeight = contentHeight - previewHeight - metrics.verticalSpacing;
  }

  // Moving the highlight up/down changes nothing outside the list band, but re-rendering the
  // preview is by far the most expensive thing on this screen: on a CJK page it is ~165 glyph
  // draws, and every one that misses the SD font cache costs ~3.7ms. So repaint the preview,
  // header and hints only when they can actually differ, and otherwise clear just the list band
  // and let the framebuffer keep the rest.
  //
  // This relies on nothing else painting the framebuffer between our renders. True today:
  // ActivityManager never clears it, this activity pushes no sub-activities (the font picker is
  // inline), loop() takes no touch input, and deep sleep resets the chip rather than resuming.
  // Adding a sub-screen or a touch handler here means setting fullRedraw_ alongside it.
  if (fullRedraw_) {
    renderer.clearScreen();
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_READER_OPTIONS));

    if (metrics.previewHeightPercent > 0) {
      // familyName/sizeName reuse the list's own value formatting for the font rows.
      const std::string familyName = getItemValue(FONT_FAMILY);
      const std::string sizeName = getItemValue(FONT_SIZE);
      textsettings::renderPreview(renderer, previewLayout_, metrics.previewPadding, metrics.verticalSpacing, contentTop,
                                  previewHeight, familyName.c_str(), sizeName.c_str(), sampleText.c_str(),
                                  /*showLabel=*/false);
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_TOGGLE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    fullRedraw_ = false;
  } else {
    renderer.clearRect(0, listTop, pageWidth, listHeight);
  }

  listTouch_.record(Rect{0, listTop, pageWidth, listHeight}, itemCount(), selectedIndex);
  GUI.drawList(
      renderer, Rect{0, listTop, pageWidth, listHeight}, itemCount(), selectedIndex,
      [](int index) { return std::string(getItemName(index)); }, nullptr, nullptr,
      [this](int index) -> std::string { return getItemValue(index); }, true);

  renderer.displayBuffer();
}
