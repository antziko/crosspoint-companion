#include "CrossPointState.h"

#include <algorithm>
#include <cstring>

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

void CrossPointState::toJson(JsonDocument& doc) const {
  doc["openEpubPath"] = openEpubPath;
  doc["lastSleepImagePath"] = lastSleepImagePath;
  JsonArray deckArr = doc["sleepDeckShown"].to<JsonArray>();
  for (size_t i = 0; i < sizeof(sleepDeckShown); i++) deckArr.add(sleepDeckShown[i]);
  doc["sleepDeckSize"] = sleepDeckSize;
  doc["sleepDeckShownCount"] = sleepDeckShownCount;
  doc["readerActivityLoadCount"] = readerActivityLoadCount;
  doc["lastSleepFromReader"] = lastSleepFromReader;
  doc["showBootScreen"] = showBootScreen;
}

bool CrossPointState::fromJson(JsonVariantConst doc) {
  openEpubPath = doc["openEpubPath"] | "";
  lastSleepImagePath = doc["lastSleepImagePath"] | "";
  // Sleep shuffle-bag deck. Absent keys (old state.json) leave the deck cleared,
  // so the next wake just starts a fresh cycle — no migration needed.
  memset(sleepDeckShown, 0, sizeof(sleepDeckShown));
  JsonArrayConst deckArr = doc["sleepDeckShown"];
  if (!deckArr.isNull()) {
    const size_t n = std::min(deckArr.size(), sizeof(sleepDeckShown));
    for (size_t i = 0; i < n; i++) sleepDeckShown[i] = deckArr[i] | static_cast<uint8_t>(0);
  }
  sleepDeckSize = doc["sleepDeckSize"] | static_cast<uint16_t>(0);
  sleepDeckShownCount = doc["sleepDeckShownCount"] | static_cast<uint16_t>(0);
  readerActivityLoadCount = doc["readerActivityLoadCount"] | static_cast<uint8_t>(0);
  lastSleepFromReader = doc["lastSleepFromReader"] | false;
  showBootScreen = doc["showBootScreen"] | true;
  return true;
}
