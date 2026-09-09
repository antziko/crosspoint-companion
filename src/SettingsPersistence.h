#pragma once

#include <BoardConfig.h>
#include <stdint.h>

#include "CrossPointSettings.h"

// Flash-resident description of every setting that CrossPointSettings persists to
// settings.json.
//
// Why this exists rather than reusing getSettingsList(): toJson()/fromJson() used to walk
// the full SettingInfo list, which is built on the heap on every call (74 entries x 72 B,
// plus ~40 per-entry std::vector<StrId> label lists). That put a multi-kilobyte contiguous
// allocation on the save path, and under -fno-exceptions a failed operator new calls
// abort() rather than returning null -- so saving settings at low heap rebooted the device.
// It was reached in the field from WifiSelectionActivity, which saves while the radio is up
// and free heap is at its lowest (crash_report.txt: size=8640 freeAtFail=4068
// largestBlock=2036 kind=throwing(fatal)).
//
// Persisting only ever needs the JSON key, the member to read/write, and the range to
// validate a loaded value against -- none of the labels, categories or option lists that
// make SettingInfo large. Holding just those four things in a constexpr table puts the
// whole description in flash, so the save/load path allocates nothing at all and cannot be
// re-broken by future growth of the settings list.
//
// Every persisted setting is a uint8_t member of CrossPointSettings. SettingInfo::String
// has no call sites, so no persisted setting is string-backed; if one is ever added it
// needs a second table here, and verifySettingsPersistenceTable() will flag it.
//
// KEEPING THIS IN SYNC: the bounds are stated here and in SettingsList.h. Prefer an
// existing _COUNT constant or lastIndex() over a literal so the bound tracks its enum
// automatically. verifySettingsPersistenceTable() cross-checks both tables at boot in
// LOG_LEVEL >= 2 builds -- see SettingsPersistence.cpp.

// Last valid index of a constexpr lookup table, so bounds for index-style settings track
// the array they index instead of repeating its length as a literal.
template <typename T, size_t N>
constexpr uint8_t lastIndex(const T (&)[N]) {
  return static_cast<uint8_t>(N - 1);
}

struct PersistedU8 {
  const char* key;                    // JSON key in settings.json
  uint8_t CrossPointSettings::* ptr;  // the uint8_t member it maps to
  uint8_t lo;                         // lowest valid value
  uint8_t hi;                         // highest valid value
  // How an out-of-range value loaded from disk is handled, matching what the SettingInfo
  // loop did per SettingType: VALUE sliders clamp into [lo, hi]; TOGGLE and ENUM fall back
  // to the field's struct-initializer default, which is what preserves sentinel values
  // (e.g. minSessionMinutes' MIN_SESSION_USE_GLOBAL) instead of folding them to a bound.
  bool clampToRange;
};

namespace persisted {
using S = CrossPointSettings;

constexpr PersistedU8 toggle(const char* key, uint8_t S::* ptr) { return {key, ptr, 0, 1, false}; }
constexpr PersistedU8 enumerated(const char* key, uint8_t S::* ptr, uint8_t hi) { return {key, ptr, 0, hi, false}; }
constexpr PersistedU8 ranged(const char* key, uint8_t S::* ptr, uint8_t lo, uint8_t hi) {
  return {key, ptr, lo, hi, true};
}
}  // namespace persisted

// Listed in the same order as SettingsList.h's getSettingsList() so the two can be diffed
// side by side.
inline constexpr PersistedU8 kPersistedSettings[] = {
    // --- Display: sleep screen ---
    persisted::enumerated("sleepScreen", &CrossPointSettings::sleepScreen,
                          CrossPointSettings::SLEEP_SCREEN_MODE_COUNT - 1),
    persisted::toggle("reviewSleepImageOnWake", &CrossPointSettings::reviewSleepImageOnWake),
    persisted::enumerated("sleepScreenCoverMode", &CrossPointSettings::sleepScreenCoverMode,
                          CrossPointSettings::SLEEP_SCREEN_COVER_MODE_COUNT - 1),
    persisted::enumerated("sleepScreenCoverFilter", &CrossPointSettings::sleepScreenCoverFilter,
                          CrossPointSettings::SLEEP_SCREEN_COVER_FILTER_COUNT - 1),
    persisted::enumerated("quickResumeSleepScreen", &CrossPointSettings::quickResumeSleepScreen,
                          CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN_COUNT - 1),

    // --- Display: e-ink ---
    persisted::enumerated("imageDither", &CrossPointSettings::imageDither, CrossPointSettings::IMAGE_DITHER_COUNT - 1),
    persisted::enumerated("refreshFrequency", &CrossPointSettings::refreshFrequency,
                          CrossPointSettings::REFRESH_FREQUENCY_COUNT - 1),
    persisted::enumerated("refreshAction", &CrossPointSettings::refreshAction,
                          CrossPointSettings::REFRESH_ACTION_COUNT - 1),
    persisted::enumerated("refreshScreenMode", &CrossPointSettings::refreshScreenMode,
                          CrossPointSettings::REFRESH_SCREEN_MODE_COUNT - 1),
    persisted::toggle("fadingFix", &CrossPointSettings::fadingFix),

    // --- Display ---
    persisted::enumerated("hideBatteryPercentage", &CrossPointSettings::hideBatteryPercentage,
                          CrossPointSettings::HIDE_BATTERY_PERCENTAGE_COUNT - 1),
    // UI_THEME has no _COUNT member; VEGA is its highest value.
    persisted::enumerated("uiTheme", &CrossPointSettings::uiTheme, CrossPointSettings::VEGA),
    persisted::enumerated("displayOrientation", &CrossPointSettings::displayOrientation,
                          CrossPointSettings::ORIENTATION_COUNT - 1),

    // --- Reader ---
    persisted::enumerated("fontFamily", &CrossPointSettings::fontFamily, CrossPointSettings::FONT_FAMILY_COUNT - 1),
    // Dictionary font family adds "Same as book" (DICT_FONT_MATCH_READER) above the built-ins.
    persisted::enumerated("dictionaryFontFamily", &CrossPointSettings::dictionaryFontFamily,
                          CrossPointSettings::DICT_FONT_MATCH_READER),
    persisted::enumerated("dictionaryFontSize", &CrossPointSettings::dictionaryFontSize,
                          CrossPointSettings::FONT_SIZE_COUNT - 1),
    persisted::enumerated("lineSpacing", &CrossPointSettings::lineSpacing,
                          CrossPointSettings::LINE_COMPRESSION_COUNT - 1),
    persisted::ranged("screenMargin", &CrossPointSettings::screenMargin, CrossPointSettings::SCREEN_MARGIN_MIN,
                      CrossPointSettings::SCREEN_MARGIN_MAX),
    persisted::enumerated("paragraphAlignment", &CrossPointSettings::paragraphAlignment,
                          CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT - 1),
    persisted::toggle("embeddedStyle", &CrossPointSettings::embeddedStyle),
    persisted::toggle("focusReadingEnabled", &CrossPointSettings::focusReadingEnabled),
    persisted::toggle("hyphenationEnabled", &CrossPointSettings::hyphenationEnabled),
    persisted::enumerated("orientation", &CrossPointSettings::orientation, CrossPointSettings::ORIENTATION_COUNT - 1),
    persisted::toggle("extraParagraphSpacing", &CrossPointSettings::extraParagraphSpacing),
    persisted::enumerated("textAntiAliasing", &CrossPointSettings::textAntiAliasing,
                          CrossPointSettings::TEXT_AA_COUNT - 1),
    persisted::enumerated("imageRendering", &CrossPointSettings::imageRendering,
                          CrossPointSettings::IMAGE_RENDERING_COUNT - 1),
    persisted::enumerated("quoteHighlightStyle", &CrossPointSettings::quoteHighlightStyle,
                          CrossPointSettings::QUOTE_STYLE_COUNT - 1),
    persisted::toggle("lookupUnderline", &CrossPointSettings::lookupUnderline),
    persisted::toggle("screenInverted", &CrossPointSettings::screenInverted),

    // --- Reader: dictionary ---
    // HIST_CAP_UNLIMITED is the sentinel one step above HIST_CAP_MAX and is a valid value.
    persisted::ranged("lookupHistoryCap", &CrossPointSettings::lookupHistoryCap, CrossPointSettings::HIST_CAP_MIN,
                      CrossPointSettings::HIST_CAP_UNLIMITED),
    persisted::ranged("flashcardRecountMins", &CrossPointSettings::flashcardRecountMins,
                      CrossPointSettings::FC_RECOUNT_MIN, CrossPointSettings::FC_RECOUNT_MAX),
    persisted::enumerated("flashcardInlineMinutesIdx", &CrossPointSettings::flashcardInlineMinutesIdx,
                          lastIndex(CrossPointSettings::FC_INLINE_MINUTES)),
    persisted::enumerated("flashcardInlineCardsIdx", &CrossPointSettings::flashcardInlineCardsIdx,
                          lastIndex(CrossPointSettings::FC_INLINE_CARDS)),
    persisted::enumerated("holdConfirmAction", &CrossPointSettings::holdConfirmAction,
                          CrossPointSettings::HOLD_CONFIRM_DICTIONARY),
    persisted::toggle("dictFallbackGroup", &CrossPointSettings::dictFallbackGroup),

    // --- Reader: stats ---
    persisted::enumerated("minSessionMinutes", &CrossPointSettings::minSessionMinutes,
                          lastIndex(CrossPointSettings::MIN_SESSION_SECONDS)),
    persisted::enumerated("pageIdleCapSeconds", &CrossPointSettings::pageIdleCapSeconds,
                          lastIndex(CrossPointSettings::PAGE_IDLE_CAP_SECONDS)),
    persisted::enumerated("progressSaveIntervalIdx", &CrossPointSettings::progressSaveIntervalIdx,
                          lastIndex(CrossPointSettings::PROGRESS_SAVE_PAGES)),

    // --- Buttons ---
    persisted::toggle("frontButtonFollowOrientation", &CrossPointSettings::frontButtonFollowOrientation),
    persisted::enumerated("longPressButtonBehavior", &CrossPointSettings::longPressButtonBehavior,
                          CrossPointSettings::LONG_PRESS_BUTTON_BEHAVIOR_COUNT - 1),
    persisted::enumerated("sideButtonLayout", &CrossPointSettings::sideButtonLayout,
                          CrossPointSettings::SIDE_BUTTON_LAYOUT_COUNT - 1),
    persisted::enumerated("touchReaderControls", &CrossPointSettings::touchReaderControls,
                          CrossPointSettings::TOUCH_READER_CONTROLS_COUNT - 1),
    persisted::toggle("swapSideButtonsCW", &CrossPointSettings::swapSideButtonsCW),
    persisted::enumerated("sideLongPressButtonBehavior", &CrossPointSettings::sideLongPressButtonBehavior,
                          CrossPointSettings::LONG_PRESS_BUTTON_BEHAVIOR_COUNT - 1),
// The Confirm action is offered only on touch boards (SettingsList.h), so the accepted
// range has to track that or the drift check fires. A settings.json carrying Confirm and
// loaded on a button board falls back to the default, which is what should happen: that
// board reaches Confirm through its own key.
#if FREEINK_CAP_TOUCH
    persisted::enumerated("shortPwrBtn", &CrossPointSettings::shortPwrBtn, CrossPointSettings::SHORT_PWRBTN_COUNT - 1),
#else
    persisted::enumerated("shortPwrBtn", &CrossPointSettings::shortPwrBtn, CrossPointSettings::SHORT_PWRBTN::FOOTNOTES),
#endif
    // Legacy key, widened from a toggle to a 3-value enum: a stored 0/1 still
    // means Off/Tap, so old files migrate without a conversion step.
    persisted::enumerated("tapForReaderMenu", &CrossPointSettings::showReaderMenu,
                          CrossPointSettings::SHOW_READER_MENU_COUNT - 1),
    persisted::toggle("pwrBtnFootnoteBack", &CrossPointSettings::pwrBtnFootnoteBack),
    persisted::toggle("backShortToFileBrowser", &CrossPointSettings::backShortToFileBrowser),

    // --- Power ---
    persisted::ranged("sleepTimeoutMinutes", &CrossPointSettings::sleepTimeoutMinutes,
                      CrossPointSettings::MIN_SLEEP_TIMEOUT_MINUTES, CrossPointSettings::MAX_SLEEP_TIMEOUT_MINUTES),

    // --- Frontlight (quick panel state; no Settings-screen rows) ---
    persisted::ranged("frontlightBrightness", &CrossPointSettings::frontlightBrightness, 0, 100),
    persisted::ranged("frontlightWarmth", &CrossPointSettings::frontlightWarmth, 0, 100),
    persisted::toggle("frontlightOn", &CrossPointSettings::frontlightOn),
    persisted::toggle("frontlightRestoreOnWake", &CrossPointSettings::frontlightRestoreOnWake),

    // --- Sync ---
    persisted::toggle("syncPromptOnSleep", &CrossPointSettings::syncPromptOnSleep),
    persisted::toggle("syncPromptOnOpen", &CrossPointSettings::syncPromptOnOpen),
    persisted::enumerated("syncPromptMinutesIdx", &CrossPointSettings::syncPromptMinutesIdx,
                          lastIndex(CrossPointSettings::SYNC_PROMPT_MINUTES)),

    // --- Library ---
    persisted::toggle("showHiddenFiles", &CrossPointSettings::showHiddenFiles),
    persisted::enumerated("recentBooksView", &CrossPointSettings::recentBooksView,
                          CrossPointSettings::RECENT_VIEW_COUNT - 1),
    persisted::toggle("removeReadBooksFromRecents", &CrossPointSettings::removeReadBooksFromRecents),
    persisted::toggle("moveFinishedToReadFolder", &CrossPointSettings::moveFinishedToReadFolder),
    persisted::toggle("sdCardLogging", &CrossPointSettings::sdCardLogging),
    persisted::enumerated("opdsFilenameFormat", &CrossPointSettings::opdsFilenameFormat, 2),

    // --- Status bar ---
    persisted::toggle("statusBarChapterPageCount", &CrossPointSettings::statusBarChapterPageCount),
    persisted::toggle("statusBarBookProgressPercentage", &CrossPointSettings::statusBarBookProgressPercentage),
    persisted::enumerated("statusBarProgressBar", &CrossPointSettings::statusBarProgressBar,
                          CrossPointSettings::STATUS_BAR_PROGRESS_BAR_COUNT - 1),
    persisted::enumerated("statusBarProgressBarThickness", &CrossPointSettings::statusBarProgressBarThickness,
                          CrossPointSettings::STATUS_BAR_PROGRESS_BAR_THICKNESS_COUNT - 1),
    persisted::enumerated("statusBarTitle", &CrossPointSettings::statusBarTitle,
                          CrossPointSettings::STATUS_BAR_TITLE_COUNT - 1),
    persisted::toggle("statusBarBattery", &CrossPointSettings::statusBarBattery),
    persisted::enumerated("xtcStatusBarMode", &CrossPointSettings::xtcStatusBarMode,
                          CrossPointSettings::XTC_STATUS_BAR_MODE_COUNT - 1),
    persisted::toggle("statusBarClock", &CrossPointSettings::statusBarClock),
    // Quarter-hour UTC offset: 0..104 covers UTC-12:00 .. UTC+14:00 in 15-minute steps.
    persisted::ranged("clockUtcOffsetQ", &CrossPointSettings::clockUtcOffsetQ, 0, 104),
    persisted::enumerated("clockFormat", &CrossPointSettings::clockFormat, 1),
    persisted::toggle("clockHasBeenSynced", &CrossPointSettings::clockHasBeenSynced),
    persisted::toggle("statusBarDate", &CrossPointSettings::statusBarDate),
    persisted::enumerated("dateFormat", &CrossPointSettings::dateFormat, 3),

    // --- Home top bar ---
    persisted::toggle("homeTopBarClock", &CrossPointSettings::homeTopBarClock),
    persisted::toggle("homeTopBarDate", &CrossPointSettings::homeTopBarDate),
    persisted::enumerated("homeTopBarDateFormat", &CrossPointSettings::homeTopBarDateFormat, 3),
    persisted::toggle("topBarOtherScreens", &CrossPointSettings::topBarOtherScreens),

    // --- Touch ---
    persisted::enumerated("tiltPageTurn", &CrossPointSettings::tiltPageTurn,
                          CrossPointSettings::TILT_PAGE_TURN_COUNT - 1),
};

// Cross-checks kPersistedSettings against the real getSettingsList() and logs any drift.
// Defined only in LOG_LEVEL >= 2 builds; call once from setup(), where free heap is ~200 KB
// and building the full list is free.
void verifySettingsPersistenceTable();
