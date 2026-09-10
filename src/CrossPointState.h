#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>

class CrossPointState : public PersistableStore<CrossPointState> {
  // Private constructor for singleton (PersistableStore provides getInstance()).
  CrossPointState() = default;

  friend class PersistableStore<CrossPointState>;

 public:
  // Sleep-wallpaper shuffle-bag ("deck"): every image is shown once, in random
  // order, before any repeat. A persistent bitset records which images have been
  // shown in the current cycle; when the cycle is exhausted (or the folder size
  // changes) a fresh cycle starts. Bounded so the state stays small: images beyond
  // SLEEP_DECK_MAX in a single folder are simply never picked.
  // (Supersedes upstream's SLEEP_RECENT_COUNT recent-list approach, now unused.)
  static constexpr uint16_t SLEEP_DECK_MAX = 512;  // max images tracked per cycle

  std::string openEpubPath;
  // Path of the wallpaper shown when entering the last sleep, when it was a random
  // pick from the /sleep folder. Persisted so the on-wake review prompt knows which
  // image to offer keep/remove for. Empty for non-folder sleep screens (blank, cover,
  // quick-resume, /sleep.bmp). Cleared once the prompt has been handled.
  std::string lastSleepImagePath;
  uint8_t sleepDeckShown[SLEEP_DECK_MAX / 8] = {};  // bit i set => image i shown this cycle (64 bytes)
  uint16_t sleepDeckSize = 0;                       // folder size the current cycle was built for
  uint16_t sleepDeckShownCount = 0;                 // images shown so far this cycle
  uint8_t readerActivityLoadCount = 0;
  bool lastSleepFromReader = false;
  bool showBootScreen = true;
  // Orientation the reader is currently using. Runtime only — NOT serialized.
  // Inside an open EPUB: the book's saved orientation (or global default if none).
  // Everywhere else: mirrors SETTINGS.orientation (global default). 0 == PORTRAIT.
  uint8_t activeOrientation = 0;

  // One-shot orientation request from the control-center orientation tile. The panel
  // cannot rotate the renderer itself (that would crop the sheet and the screen under
  // it), and it cannot just write activeOrientation either, because the readers need to
  // reflow. So it parks the request here and the resumed activity adopts it; a rotation
  // made inside a reader is per-book and never goes through this. ActivityManager clears
  // it after the resume, so a request never leaks into the next screen.
  // Runtime only — NOT serialized.
  static constexpr uint8_t NO_ORIENTATION_REQUEST = 0xFF;
  uint8_t pendingOrientation = NO_ORIENTATION_REQUEST;

  // Set by an activity to ask the main loop to enter a *manual* deep sleep (fromTimeout=false)
  // on its next iteration. Used by the reader's "sync before sleep" flow to defer the sleep
  // gesture across a confirmation prompt / sync activity. Runtime only — NOT serialized.
  bool requestManualSleep = false;

  // Sleep deck helpers (see SLEEP_DECK_MAX above).
  bool isSleepShown(uint16_t idx) const;  // already shown this cycle?
  void markSleepShown(uint16_t idx);      // record idx as shown this cycle
  void resetSleepDeck(uint16_t size);     // begin a fresh cycle for `size` images

  // PersistableStore hooks (getInstance/saveToFile/loadFromFile come from the CRTP base).
  static const char* getFilePath() { return "/.crosspoint/state.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);
};

// Helper macro to access state
#define APP_STATE CrossPointState::getInstance()
