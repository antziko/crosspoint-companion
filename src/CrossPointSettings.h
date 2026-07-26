#pragma once
#include <ArduinoJson.h>
#include <HalStorage.h>
#include <PersistableStore.h>

#include <cstdint>
#include <iosfwd>

class CrossPointSettings : public PersistableStore<CrossPointSettings> {
 private:
  // Private constructor for singleton (PersistableStore provides getInstance()).
  CrossPointSettings() = default;

  friend class PersistableStore<CrossPointSettings>;

 public:
  enum SLEEP_SCREEN_MODE {
    DARK = 0,
    LIGHT = 1,
    CUSTOM = 2,
    COVER = 3,
    COVER_CUSTOM = 4,
    BLANK = 5,
    QUICK_RESUME = 6,
    SLEEP_SCREEN_MODE_COUNT
  };
  enum SLEEP_SCREEN_COVER_MODE { FIT = 0, CROP = 1, SLEEP_SCREEN_COVER_MODE_COUNT };
  enum SLEEP_SCREEN_COVER_FILTER {
    NO_FILTER = 0,
    BLACK_AND_WHITE = 1,
    INVERTED_BLACK_AND_WHITE = 2,
    SLEEP_SCREEN_COVER_FILTER_COUNT
  };

  // Status bar enum - legacy
  enum STATUS_BAR_MODE {
    NONE = 0,
    NO_PROGRESS = 1,
    FULL = 2,
    BOOK_PROGRESS_BAR = 3,
    ONLY_BOOK_PROGRESS_BAR = 4,
    CHAPTER_PROGRESS_BAR = 5,
    STATUS_BAR_MODE_COUNT
  };
  enum STATUS_BAR_PROGRESS_BAR {
    BOOK_PROGRESS = 0,
    CHAPTER_PROGRESS = 1,
    HIDE_PROGRESS = 2,
    STATUS_BAR_PROGRESS_BAR_COUNT
  };
  enum STATUS_BAR_PROGRESS_BAR_THICKNESS {
    PROGRESS_BAR_THIN = 0,
    PROGRESS_BAR_NORMAL = 1,
    PROGRESS_BAR_THICK = 2,
    STATUS_BAR_PROGRESS_BAR_THICKNESS_COUNT
  };
  enum STATUS_BAR_TITLE { BOOK_TITLE = 0, CHAPTER_TITLE = 1, HIDE_TITLE = 2, STATUS_BAR_TITLE_COUNT };
  enum XTC_STATUS_BAR_MODE {
    XTC_STATUS_BAR_HIDE = 0,
    XTC_STATUS_BAR_BOTTOM = 1,
    XTC_STATUS_BAR_TOP = 2,
    XTC_STATUS_BAR_MODE_COUNT
  };

  enum STATUS_BAR_CLOCK_MODE { STATUS_BAR_CLOCK_HIDE = 0, STATUS_BAR_CLOCK_RIGHT = 1, STATUS_BAR_CLOCK_LEFT = 2 };

  enum ORIENTATION {
    PORTRAIT = 0,       // 480x800 logical coordinates (current default)
    LANDSCAPE_CW = 1,   // 800x480 logical coordinates, rotated 180° (swap top/bottom)
    INVERTED = 2,       // 480x800 logical coordinates, inverted
    LANDSCAPE_CCW = 3,  // 800x480 logical coordinates, native panel orientation
    ORIENTATION_COUNT
  };

  // Front button layout options (legacy)
  // Default: Back, Confirm, Left, Right
  // Swapped: Left, Right, Back, Confirm
  enum FRONT_BUTTON_LAYOUT {
    BACK_CONFIRM_LEFT_RIGHT = 0,
    LEFT_RIGHT_BACK_CONFIRM = 1,
    LEFT_BACK_CONFIRM_RIGHT = 2,
    BACK_CONFIRM_RIGHT_LEFT = 3,
    FRONT_BUTTON_LAYOUT_COUNT
  };

  // Front button hardware identifiers (for remapping)
  enum FRONT_BUTTON_HARDWARE {
    FRONT_HW_BACK = 0,
    FRONT_HW_CONFIRM = 1,
    FRONT_HW_LEFT = 2,
    FRONT_HW_RIGHT = 3,
    FRONT_BUTTON_HARDWARE_COUNT
  };

  // Side button layout options
  // Default: Up = Previous, Down = Next
  enum SIDE_BUTTON_LAYOUT { PREV_NEXT = 0, NEXT_PREV = 1, SIDE_BUTTONS_DISABLED = 2, SIDE_BUTTON_LAYOUT_COUNT };

  // Font family options (built-in fonts only; SD card fonts use sdFontFamilyName)
  enum FONT_FAMILY { NOTOSERIF = 0, NOTOSANS = 1, FONT_FAMILY_COUNT };
  static constexpr uint8_t LEGACY_OPENDYSLEXIC = 2;
  static constexpr uint8_t BUILTIN_FONT_COUNT = FONT_FAMILY_COUNT;
  // The reader font size is a point size (see fontPointSize), NOT an enum slot.
  // This enum survives only for the dictionary/definition viewer font size
  // (dictionaryFontSize), which still uses discrete Small/Medium/Large/XL slots.
  enum FONT_SIZE { SMALL = 0, MEDIUM = 1, LARGE = 2, EXTRA_LARGE = 3, FONT_SIZE_COUNT };
  // Legacy 1.4-and-earlier files stored the reader size as a 0..3 SMALL/MEDIUM/
  // LARGE/EXTRA_LARGE slot; fromJson()/ReaderSettingsIO fold that range up to the
  // point size it meant (see LEGACY_FONT_SIZE_MAX).
  static constexpr uint8_t LEGACY_FONT_SIZE_MAX = 3;
  static constexpr uint8_t DEFAULT_FONT_POINT_SIZE = 14;
  enum LINE_COMPRESSION { TIGHT = 0, NORMAL = 1, WIDE = 2, LINE_COMPRESSION_COUNT };
  enum PARAGRAPH_ALIGNMENT {
    JUSTIFIED = 0,
    LEFT_ALIGN = 1,
    CENTER_ALIGN = 2,
    RIGHT_ALIGN = 3,
    BOOK_STYLE = 4,
    PARAGRAPH_ALIGNMENT_COUNT
  };

  // Auto-sleep timeout options (in minutes)
  enum SLEEP_TIMEOUT {
    SLEEP_1_MIN = 0,
    SLEEP_5_MIN = 1,
    SLEEP_10_MIN = 2,
    SLEEP_15_MIN = 3,
    SLEEP_30_MIN = 4,
    SLEEP_TIMEOUT_COUNT
  };

  // E-ink refresh frequency (pages between full refreshes)
  enum REFRESH_FREQUENCY {
    REFRESH_1 = 0,
    REFRESH_5 = 1,
    REFRESH_10 = 2,
    REFRESH_15 = 3,
    REFRESH_30 = 4,
    REFRESH_FREQUENCY_COUNT
  };

  // Short power button press actions
  enum SHORT_PWRBTN { IGNORE = 0, SLEEP = 1, PAGE_TURN = 2, FORCE_REFRESH = 3, FOOTNOTES = 4, SHORT_PWRBTN_COUNT };

  // Manual "Refresh Screen" (power-button FORCE_REFRESH) clear mode. Drives the
  // whole-panel ghost clear in main.cpp. FAST is grayscale-safe (X4 default);
  // HALF/FULL give a stronger ghost clear but firm the e-ink particles too hard
  // for the following grayscale pass, washing AA/image pages whitish on X4.
  enum REFRESH_SCREEN_MODE { RSM_FAST = 0, RSM_HALF = 1, RSM_FULL = 2, REFRESH_SCREEN_MODE_COUNT };

  // Hide battery percentage
  enum HIDE_BATTERY_PERCENTAGE { HIDE_NEVER = 0, HIDE_READER = 1, HIDE_ALWAYS = 2, HIDE_BATTERY_PERCENTAGE_COUNT };

  // Page turn button long press behavior
  enum LONG_PRESS_BUTTON_BEHAVIOR {
    OFF = 0,
    CHAPTER_SKIP = 1,
    ORIENTATION_CHANGE = 2,
    // Hold left front button = toggle bookmark; hold right = sync progress.
    BOOKMARK_AND_SYNC = 3,
    LONG_PRESS_BUTTON_BEHAVIOR_COUNT
  };

  // UI Theme
  enum UI_THEME { CLASSIC = 0, LYRA = 1, LYRA_3_COVERS = 2, ROUNDEDRAFF = 3, VEGA = 4 };

  // Image rendering in EPUB reader
  enum IMAGE_RENDERING { IMAGES_DISPLAY = 0, IMAGES_PLACEHOLDER = 1, IMAGES_SUPPRESS = 2, IMAGE_RENDERING_COUNT };

  // 1-bit halftone dither algorithm (X3). Applies to all images: EPUB images,
  // sleep wallpaper, BMP viewer. Both options are stateless ordered dithers.
  enum IMAGE_DITHER { DITHER_BLUE_NOISE = 0, DITHER_BAYER = 1, DITHER_ERROR_DIFFUSION = 2, IMAGE_DITHER_COUNT };

  enum TILT_PAGE_TURN { TILT_OFF = 0, TILT_NORMAL = 1, TILT_NVERTED = 2, TILT_PAGE_TURN_COUNT };

  // Text rendering on X4. Off: solid-black text + 1-bit images (fast, no two-stage
  // flash). Antialiased: grey (AA) text + 4-level grey images. Sharp: true-black
  // text + 4-level grey images (grayscale pass runs only on pages with an image).
  enum TEXT_AA { TEXT_AA_OFF = 0, TEXT_AA_ANTIALIASED = 1, TEXT_AA_SHARP = 2, TEXT_AA_COUNT };
  enum TOUCH_READER_CONTROLS { TOUCH_READER_OFF = 0, TOUCH_READER_ON = 1, TOUCH_READER_CONTROLS_COUNT };

  enum QUICK_RESUME_SLEEP_SCREEN {
    QUICK_RESUME_NEVER = 0,
    QUICK_RESUME_AFTER_TIMEOUT = 1,
    QUICK_RESUME_SLEEP_SCREEN_COUNT
  };

  // Dictionary-lookup flashcards (FlashcardReviewActivity). Card front style and
  // review-session card selection order. Values map to FlashcardDeck render /
  // FlashcardDeck::SessionScope.
  enum FLASHCARD_CARD_STYLE { FLASHCARD_STYLE_CLOZE = 0, FLASHCARD_STYLE_WORD_CONTEXT = 1, FLASHCARD_CARD_STYLE_COUNT };
  enum FLASHCARD_SESSION_SCOPE {
    FLASHCARD_SCOPE_DUE_FIRST = 0,
    FLASHCARD_SCOPE_ALL_SHUFFLED = 1,
    FLASHCARD_SESSION_SCOPE_COUNT
  };

  // Per-book reader settings override.
  // When active, SETTINGS.getReaderFontId(), getReaderLineCompression(), and the
  // three getReader*() accessors below return values from this struct instead of
  // the global members.  Never serialised to settings.json.
  struct ReaderOverride {
    bool active = false;
    uint8_t fontFamily = NOTOSERIF;
    // Per-book reader font size, an actual point size (matches CrossPointSettings::
    // fontPointSize). Persisted per-book by ReaderSettingsIO, which folds the legacy
    // 0..3 slot up on read.
    uint8_t fontPointSize = DEFAULT_FONT_POINT_SIZE;
    uint8_t lineSpacing = NORMAL;
    uint8_t paragraphAlignment = JUSTIFIED;
    uint8_t hyphenationEnabled = 0;
    uint8_t extraParagraphSpacing = 1;
    char sdFontFamilyName[32] = "";
    // Per-book min-session threshold. 0xFF = inherit the global setting; otherwise an
    // index into CrossPointSettings::MIN_SESSION_SECONDS (0 = always commit). The field
    // name is kept for the settings/cache key, but it now holds an index, not minutes.
    static constexpr uint8_t MIN_SESSION_USE_GLOBAL = 0xFF;
    uint8_t minSessionMinutes = MIN_SESSION_USE_GLOBAL;
  };

  // Sleep screen settings
  uint8_t sleepScreen = DARK;
  // When set, after a random /sleep-folder wallpaper was shown, the next wake offers
  // a keep/remove prompt for that image before landing on Home/Reader. Off by default.
  uint8_t reviewSleepImageOnWake = 0;
  // Sleep screen cover mode settings
  uint8_t sleepScreenCoverMode = FIT;
  // Sleep screen cover filter
  uint8_t sleepScreenCoverFilter = NO_FILTER;
  // Status bar settings (statusBar retained for migration only)
  uint8_t statusBar = FULL;
  uint8_t statusBarChapterPageCount = 1;
  uint8_t statusBarBookProgressPercentage = 1;
  uint8_t statusBarProgressBar = HIDE_PROGRESS;
  uint8_t statusBarProgressBarThickness = PROGRESS_BAR_NORMAL;
  uint8_t statusBarTitle = CHAPTER_TITLE;
  uint8_t statusBarBattery = 1;
  uint8_t xtcStatusBarMode = XTC_STATUS_BAR_HIDE;
  // Clock display in status bar (needs a clock: X3 DS3231 RTC, or X4 NTP over WiFi)
  uint8_t statusBarClock = 0;
  // Date display in status bar (needs a clock: X3 DS3231 RTC, or X4 NTP over WiFi)
  uint8_t statusBarDate = 0;
  // Date format: 0="30 Jun", 1="Mon, 30 Jun", 2="30/06", 3="Mon, 30/06"
  uint8_t dateFormat = 0;
  // Home top bar display — independent from reader status bar (needs a clock: X3 RTC or X4 NTP)
  uint8_t homeTopBarClock = 0;
  uint8_t homeTopBarDate = 0;
  // Home top bar date format: same codes as dateFormat
  uint8_t homeTopBarDateFormat = 0;
  // Clock UTC offset in quarter-hour steps, biased by 48 so it fits in uint8_t.
  // Value 48 = UTC+0, 0 = UTC-12:00, 104 = UTC+14:00.
  // Quarter-hour granularity supports oddball zones like Nepal (+5:45) and Chatham (+12:45).
  uint8_t clockUtcOffsetQ = 48;
  // Clock display format: 0 = 24-hour, 1 = 12-hour
  uint8_t clockFormat = 0;
  // Set once an NTP sync succeeds. Used to skip re-syncing on every WiFi connect.
  // Resetting to 0 (e.g. via the web UI) forces a re-sync on next WiFi connect.
  uint8_t clockHasBeenSynced = 0;
  // Text rendering settings
  uint8_t extraParagraphSpacing = 1;
  uint8_t textAntiAliasing = TEXT_AA_ANTIALIASED;  // TEXT_AA enum (0=Off,1=Antialiased,2=Sharp)
  // Short power button click behaviour
  uint8_t shortPwrBtn = IGNORE;
  // EPUB reading orientation settings
  // 0 = portrait (default), 1 = landscape clockwise, 2 = inverted, 3 = landscape counter-clockwise
  uint8_t orientation = PORTRAIT;
  // Non-reader UI orientation (Home/Browser/Stats/etc.) — independent of the reader's orientation
  uint8_t displayOrientation = PORTRAIT;
  // Button layouts (front layout retained for migration only)
  uint8_t frontButtonLayout = BACK_CONFIRM_LEFT_RIGHT;
  uint8_t sideButtonLayout = PREV_NEXT;
  uint8_t frontButtonFollowOrientation = 0;
  // Front button remap (logical -> hardware)
  // Used by MappedInputManager to translate logical buttons into physical front buttons.
  uint8_t frontButtonBack = FRONT_HW_BACK;
  uint8_t frontButtonConfirm = FRONT_HW_CONFIRM;
  uint8_t frontButtonLeft = FRONT_HW_LEFT;
  uint8_t frontButtonRight = FRONT_HW_RIGHT;
  // LandscapeCW front button override (logical -> hardware). Default = factory
  // order so behavior is unchanged until the user configures the CW remap. Used
  // as-is in CW; the "Orient front buttons" swap does not apply in CW (only in
  // PortraitInverted / LandscapeCounterClockwise).
  uint8_t frontButtonBackCW = FRONT_HW_BACK;
  uint8_t frontButtonConfirmCW = FRONT_HW_CONFIRM;
  uint8_t frontButtonLeftCW = FRONT_HW_LEFT;
  uint8_t frontButtonRightCW = FRONT_HW_RIGHT;
  // LandscapeCW: swap the two side buttons (reader page-turn + menu Up/Down). Off by default.
  uint8_t swapSideButtonsCW = 0;
  // Reader font settings
  uint8_t fontFamily = NOTOSERIF;
  // Point size of the reader font. Only sizes the active family actually ships
  // are selectable; SdCardFontSystem::ensureLoaded() snaps this to the nearest
  // available size (and persists the snap) whenever the family changes.
  uint8_t fontPointSize = DEFAULT_FONT_POINT_SIZE;
  uint8_t lineSpacing = NORMAL;
  uint8_t paragraphAlignment = JUSTIFIED;
  // Definition viewer font (built-in fonts only).
  uint8_t dictionaryFontFamily = NOTOSERIF;
  uint8_t dictionaryFontSize = MEDIUM;
  // Dictionary-lookup flashcards: card front style + review session card order.
  // Both are chosen on the FlashcardReviewActivity overview (not in SettingsList)
  // and persisted here as the remembered defaults. Default style = word+context.
  uint8_t flashcardCardStyle = FLASHCARD_STYLE_WORD_CONTEXT;
  uint8_t flashcardSessionScope = FLASHCARD_SCOPE_DUE_FIRST;
  // Auto-sleep timeout setting (default 10 minutes). Legacy sleepTimeout enum values are migration-only.
  uint8_t sleepTimeoutMinutes = 10;
  // When on, a manual power-button sleep from the reader offers to KOReader-sync first
  // (never on auto-sleep timeout). Off by default — opt-in.
  uint8_t syncPromptOnSleep = 0;
  // When on, opening (or waking into) a book offers to KOReader-sync first if enough reading
  // has accrued since the last sync. Shares the syncPromptMinutesIdx threshold. Off by default.
  uint8_t syncPromptOnOpen = 0;
  // Only show that prompt once this much reading time has accrued since the last successful
  // sync of the open book. Stored as an index into SYNC_PROMPT_MINUTES (default index 1 = 5 min).
  uint8_t syncPromptMinutesIdx = 1;
  // E-ink refresh frequency (default 15 pages)
  uint8_t refreshFrequency = REFRESH_15;
  // Manual "Refresh Screen" clear mode (default FAST: grayscale-safe everywhere)
  uint8_t refreshScreenMode = RSM_FAST;
  uint8_t hyphenationEnabled = 0;

  // Reader screen margin settings
  // LOCAL(feat-dictionary): margin bounds from #2605; screenMargin default unchanged (== MIN)
  static constexpr uint8_t SCREEN_MARGIN_MIN = 5;
  static constexpr uint8_t SCREEN_MARGIN_MAX = 40;
  static constexpr uint8_t SCREEN_MARGIN_STEP = 5;
  uint8_t screenMargin = SCREEN_MARGIN_MIN;
  // Hide battery percentage
  uint8_t hideBatteryPercentage = HIDE_NEVER;
  // Long-press behavior for FRONT page-turn buttons (Left/Right)
  uint8_t longPressButtonBehavior = OFF;
  // Long-press behavior for SIDE page-turn buttons (PageBack/PageForward)
  uint8_t sideLongPressButtonBehavior = OFF;
  // UI Theme
  uint8_t uiTheme = LYRA;
  // Sunlight fading compensation
  uint8_t fadingFix = 0;
  // Power button return from footnotes (1 = enabled, 0 = disabled)
  uint8_t pwrBtnFootnoteBack = 1;
  // Use book's embedded CSS styles for EPUB rendering (1 = enabled, 0 = disabled)
  uint8_t embeddedStyle = 1;
  // Focus Reading - emphasizes the first part of words with bold
  uint8_t focusReadingEnabled = 0;
  // SD card font family name (empty = use built-in fontFamily)
  char sdFontFamilyName[32] = "";
  // Pinned fonts in the Font Family picker: newline-separated set of font keys
  // (built-in = "@b<index>", SD = "@s<familyName>"). Pinned fonts sort first.
  // 256 bytes covers ~8 typical SD names; appends are bounds-checked.
  char pinnedFonts[256] = "";
  // Show hidden files/directories (starting with '.') in the file browser (0 = hidden, 1 = show)
  uint8_t showHiddenFiles = 0;
  // Remove a book from the Recent Books list when its End-of-Book screen is reached (0 = off, 1 = on)
  uint8_t removeReadBooksFromRecents = 0;
  // Move epub to /Read/ folder on SD card when finished (0 = disabled, 1 = enabled)
  uint8_t moveFinishedToReadFolder = 0;
  // On-disk filename format for OPDS downloads. See OpdsFilenameFormat in util/OpdsFilename.h
  // (0=Author-Title, 1=Title-Author, 2=Title). Default 1 preserves this branch's long-standing
  // "Title - Author.epub" download convention. Edited from the OPDS server list + web UI;
  // category-less SettingInfo::Enum keeps it off the on-device Settings screen.
  uint8_t opdsFilenameFormat = 1;
  // Write diagnostic logs to /opds_debug.txt on the SD card (0 = off, 1 = on). Off by
  // default: a diagnostic, and avoids SD wear. Drives SdDebugLog's master switch.
  uint8_t sdCardLogging = 0;
  // Minimum session duration before reading time is committed to stats, as an index
  // into MIN_SESSION_SECONDS (0 = always commit, the default). Sessions whose effective
  // (idle-capped) time is shorter are discarded. Field name kept for the settings key,
  // but it holds an index, not minutes.
  uint8_t minSessionMinutes = 0;
  // Index -> seconds for minSessionMinutes (global) and ReaderOverride::minSessionMinutes.
  // Index 0 = always (no minimum). Ladder: Off/always, 15s, 30s, 1m, 2m, 5m.
  static constexpr uint16_t MIN_SESSION_SECONDS[] = {0, 15, 30, 60, 120, 300};
  // Idle-page cap for reading stats. A page held longer than PAGE_IDLE_THRESHOLD_SECONDS
  // (60s) is treated as idle/AFK and contributes only the capped seconds to recorded
  // reading time instead of full wall-clock. Stored as an index into PAGE_IDLE_CAP_SECONDS;
  // 0 = Off (no cap, full wall-clock — the legacy behaviour).
  uint8_t pageIdleCapSeconds = 0;
  // Index -> seconds mapping for pageIdleCapSeconds. Index 0 = Off.
  static constexpr uint16_t PAGE_IDLE_CAP_SECONDS[] = {0, 15, 30, 45, 60};
  // A page dwell beyond this is considered idle and gets capped (when the cap is enabled).
  static constexpr uint32_t PAGE_IDLE_THRESHOLD_SECONDS = 60;
  // Dictionary/highlight word-select marker placement by current-page dwell. When enabled, the
  // initial word-select marker starts near the top of the page for a short dwell, the middle for
  // a medium dwell, and lower for a long dwell (eyes assumed to have moved down the page). Dwell is
  // idle-adjusted via the pageIdleCapSeconds cap above. 0 = off (always centre, legacy behaviour).
  uint8_t dictMarkerDwellEnabled = 0;
  // Dwell thresholds, stored as indices into the seconds tables below: dwell <= T1 -> top,
  // dwell <= T2 -> middle, otherwise bottom.
  uint8_t dictMarkerT1Idx = 1;  // default 5s
  uint8_t dictMarkerT2Idx = 2;  // default 12s
  static constexpr uint16_t DICT_MARKER_T1_SECONDS[] = {3, 5, 8, 10};
  static constexpr uint16_t DICT_MARKER_T2_SECONDS[] = {7, 9, 12, 15};
  // Reading-progress save debounce: write /progress.bin only every N page turns (plus a flush on
  // reader exit / sleep) to cut SD wear. Stored as an index into PROGRESS_SAVE_PAGES. Index 0 = 1
  // (save every turn, the safest legacy behaviour — no progress loss on hard power-off).
  uint8_t progressSaveIntervalIdx = 0;
  static constexpr uint16_t PROGRESS_SAVE_PAGES[] = {1, 5, 10, 15, 30};
  // Short press Back goes to file browser instead of home (0 = disabled, 1 = enabled)
  uint8_t backShortToFileBrowser = 0;
  // Image rendering mode in EPUB reader
  uint8_t imageRendering = IMAGES_DISPLAY;
  // 1-bit halftone dither algorithm for all images (X3): blue noise vs Bayer
  uint8_t imageDither = DITHER_BLUE_NOISE;
  // Lookup history entry cap (direct value)
  static constexpr uint8_t HIST_CAP_MIN = 25;
  static constexpr uint8_t HIST_CAP_MAX = 225;  // highest finite cap
  static constexpr uint8_t HIST_CAP_STEP = 25;
  static constexpr uint8_t HIST_CAP_DEFAULT = 100;
  // Sentinel one step above HIST_CAP_MAX: history grows without eviction. Only
  // RAM-safe because the history-list UI pages a fixed window (never materializes
  // the whole file). See LookupHistory / LookedUpWordsActivity.
  static constexpr uint8_t HIST_CAP_UNLIMITED = HIST_CAP_MAX + HIST_CAP_STEP;  // 250
  uint8_t lookupHistoryCap = HIST_CAP_DEFAULT;
  // Action triggered by holding Confirm in the reader.
  // OFF: no action (default). BOOKMARK: add bookmark @ BOOKMARK_HOLD_MS (400ms).
  // DICTIONARY: open word-select @ Dictionary::LONG_PRESS_MS (600ms, requires per-book dictionary).
  enum HOLD_CONFIRM_ACTION : uint8_t { HOLD_CONFIRM_OFF = 0, HOLD_CONFIRM_BOOKMARK = 1, HOLD_CONFIRM_DICTIONARY = 2 };
  uint8_t holdConfirmAction = HOLD_CONFIRM_OFF;
  // Tilt-based page turning (X3 only — requires QMI8658 IMU)
  uint8_t tiltPageTurn = TILT_OFF;
  // Touch screen reader zones/gestures on boards with a touch controller.
  uint8_t touchReaderControls = TOUCH_READER_ON;
  // Language setting (Language enum index, default 0 = EN)
  uint8_t language = 0;
  // Quick Resume: keep current content visible with moon icon instead of showing a static sleep screen.
  uint8_t quickResumeSleepScreen = QUICK_RESUME_NEVER;

  static constexpr uint8_t MIN_SLEEP_TIMEOUT_MINUTES = 1;
  static constexpr uint8_t SLEEP_TIMEOUT_NEVER_MINUTES = 31;
  static constexpr uint8_t MAX_SLEEP_TIMEOUT_MINUTES = SLEEP_TIMEOUT_NEVER_MINUTES;

  // Reading-minutes-since-last-sync thresholds for the "sync before sleep" prompt.
  // syncPromptMinutesIdx indexes this table; e.g. index 0 = prompt after 3 min of reading.
  static constexpr uint8_t SYNC_PROMPT_MINUTES[] = {3, 5, 10, 15, 20, 25, 30};

  // Callback to resolve SD card font IDs. Set by SdCardFontSystem::begin().
  // Returns font ID or 0 if not found.
  using SdFontIdResolver = int (*)(void* ctx, const char* familyName, uint8_t fontSize);
  SdFontIdResolver sdFontIdResolver = nullptr;
  void* sdFontResolverCtx = nullptr;

  uint16_t getPowerButtonDuration() const {
    return (shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP) ? 10 : 400;
  }
  int getReaderFontId() const;
  // Reader font point size, honoring the per-book override when active.
  uint8_t getReaderFontSize() const;
  // SD-card font family name honoring the per-book override when active (empty
  // string means "use a built-in font"). Returned pointer is owned by settings.
  const char* getReaderSdFontFamilyName() const;
  int getDefinitionFontId() const;
  float getDefinitionLineCompression() const;

  // Pinned-font set helpers (see pinnedFonts). `key` is "@b<index>" / "@s<name>".
  bool isFontPinned(const char* key) const;
  // Add/remove `key` from the pinned set in place. Caller persists via saveToFile().
  // No-op if already in the desired state or (on add) the buffer is full.
  void setFontPinned(const char* key, bool pinned);
  int getLookupHistoryCapValue() const { return lookupHistoryCap; }
  bool isLookupHistoryUnlimited() const { return lookupHistoryCap >= HIST_CAP_UNLIMITED; }

  // Drop the SD font selection and fall back to the built-in family. The reader
  // point size comes back into BUILTIN_READER_POINT_SIZES with it, since that is
  // the only set a built-in family ships — otherwise the settings UI would keep
  // offering a size nothing renders at. Both fields are persisted in one write.
  void clearSdFontFamily();

  // Per-book override control.
  void setReaderOverride(const ReaderOverride& ov);
  void clearReaderOverride();
  const ReaderOverride& getReaderOverride() const { return readerOverride; }

  // Per-book aware reader accessors.
  // When an override is active these return override values; otherwise globals.
  uint8_t getReaderParagraphAlignment() const;
  uint8_t getReaderHyphenationEnabled() const;
  uint8_t getReaderExtraParagraphSpacing() const;

  // PersistableStore hooks (getInstance/loadFromFile come from the CRTP base).
  static const char* getFilePath() { return "/.crosspoint/settings.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);
  // Shadows PersistableStore::saveToFile() to apply the SD-debug logging toggle
  // live on every persist before delegating to the base save.
  bool saveToFile() const;

  static void validateFrontButtonMapping(CrossPointSettings& settings);
  static uint8_t sleepTimeoutEnumToMinutes(uint8_t legacyValue);

 private:
  // In-memory per-book override. Never persisted to settings.json.
  ReaderOverride readerOverride;

  // Shared computation helpers used by both global and override code paths.
  static float computeLineCompression(uint8_t family, uint8_t lineSpacing, const char* sdFontName);

 public:
  // Pure helper: built-in family + size enum -> fontId. Public so the font
  // picker can resolve a preview fontId without mutating the active setting.
  static int computeBuiltinFontId(uint8_t family, uint8_t size);
  float getReaderLineCompression() const;
  unsigned long getSleepTimeoutMs() const;
  int getRefreshFrequency() const;
};

// Helper macro to access settings
#define SETTINGS CrossPointSettings::getInstance()
