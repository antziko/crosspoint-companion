#include "CrossPointState.h"

#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>
#include <cstring>
#include <mutex>

namespace {
constexpr uint8_t STATE_FILE_VERSION = 4;
constexpr char STATE_FILE_BIN[] = "/.crosspoint/state.bin";
constexpr char STATE_FILE_JSON[] = "/.crosspoint/state.json";
constexpr char STATE_FILE_BAK[] = "/.crosspoint/state.bin.bak";
}  // namespace

CrossPointState CrossPointState::instance;

bool CrossPointState::isSleepShown(uint16_t idx) const {
  if (idx >= SLEEP_DECK_MAX) return false;
  return (sleepDeckShown[idx >> 3] >> (idx & 7)) & 1u;
}

void CrossPointState::markSleepShown(uint16_t idx) {
  if (idx >= SLEEP_DECK_MAX || isSleepShown(idx)) return;
  sleepDeckShown[idx >> 3] |= static_cast<uint8_t>(1u << (idx & 7));
  if (sleepDeckShownCount < UINT16_MAX) sleepDeckShownCount++;
}

void CrossPointState::resetSleepDeck(uint16_t size) {
  memset(sleepDeckShown, 0, sizeof(sleepDeckShown));
  sleepDeckSize = size;
  sleepDeckShownCount = 0;
}

bool CrossPointState::saveToFile() const {
  std::lock_guard<std::mutex> lock(_mutex);
  Storage.mkdir("/.crosspoint");
  return JsonSettingsIO::saveState(*this, STATE_FILE_JSON);
}

bool CrossPointState::loadFromFile() {
  // Try JSON first
  if (Storage.exists(STATE_FILE_JSON)) {
    String json = Storage.readFile(STATE_FILE_JSON);
    if (!json.isEmpty()) {
      std::lock_guard<std::mutex> lock(_mutex);
      return JsonSettingsIO::loadState(*this, json.c_str());
    }
  }

  // Fall back to binary migration
  if (Storage.exists(STATE_FILE_BIN)) {
    if (loadFromBinaryFile()) {
      if (saveToFile()) {
        Storage.rename(STATE_FILE_BIN, STATE_FILE_BAK);
        LOG_DBG("CPS", "Migrated state.bin to state.json");
        return true;
      } else {
        LOG_ERR("CPS", "Failed to save state during migration");
        return false;
      }
    }
  }

  return false;
}

bool CrossPointState::loadFromBinaryFile() {
  HalFile inputFile;
  if (!Storage.openFileForRead("CPS", STATE_FILE_BIN, inputFile)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(_mutex);

  uint8_t version;
  serialization::readPod(inputFile, version);
  if (version > STATE_FILE_VERSION) {
    LOG_ERR("CPS", "Deserialization failed: Unknown version %u", version);
    return false;
  }

  serialization::readString(inputFile, openEpubPath);
  if (version >= 2) {
    // Legacy single recent-image byte. The shuffle-bag deck replaces it; consume
    // the byte to keep parsing aligned, but don't seed anything (deck starts fresh).
    uint8_t legacyLastSleep = UINT8_MAX;
    serialization::readPod(inputFile, legacyLastSleep);
  }

  if (version >= 3) {
    serialization::readPod(inputFile, readerActivityLoadCount);
  }

  if (version >= 4) {
    serialization::readPod(inputFile, lastSleepFromReader);
  } else {
    lastSleepFromReader = false;
  }

  return true;
}
