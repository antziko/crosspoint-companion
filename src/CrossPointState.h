#pragma once
#include <cstdint>
#include <string>

class CrossPointState {
  // Static instance
  static CrossPointState instance;

 public:
  // Sleep-wallpaper shuffle-bag ("deck"): every image is shown once, in random
  // order, before any repeat. A persistent bitset records which images have been
  // shown in the current cycle; when the cycle is exhausted (or the folder size
  // changes) a fresh cycle starts. Bounded so the state stays small: images beyond
  // SLEEP_DECK_MAX in a single folder are simply never picked.
  static constexpr uint16_t SLEEP_DECK_MAX = 512;  // max images tracked per cycle

  std::string openEpubPath;
  uint8_t sleepDeckShown[SLEEP_DECK_MAX / 8] = {};  // bit i set => image i shown this cycle (64 bytes)
  uint16_t sleepDeckSize = 0;                        // folder size the current cycle was built for
  uint16_t sleepDeckShownCount = 0;                  // images shown so far this cycle
  uint8_t readerActivityLoadCount = 0;
  bool lastSleepFromReader = false;
  bool showBootScreen = true;
  // Orientation the reader is currently using. Runtime only — NOT serialized.
  // Inside an open EPUB: the book's saved orientation (or global default if none).
  // Everywhere else: mirrors SETTINGS.orientation (global default). 0 == PORTRAIT.
  uint8_t activeOrientation = 0;

  // Sleep deck helpers (see SLEEP_DECK_MAX above).
  bool isSleepShown(uint16_t idx) const;   // already shown this cycle?
  void markSleepShown(uint16_t idx);       // record idx as shown this cycle
  void resetSleepDeck(uint16_t size);      // begin a fresh cycle for `size` images
  ~CrossPointState() = default;

  // Get singleton instance
  static CrossPointState& getInstance() { return instance; }

  bool saveToFile() const;

  bool loadFromFile();

 private:
  bool loadFromBinaryFile();
};

// Helper macro to access settings
#define APP_STATE CrossPointState::getInstance()
