#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Per-dictionary tabs on the definition screen (Words / Names / Grammar), after matcha-reader's
// lookup panel. A tab is a StarDict dictionary converted from one of matcha's dictionaries
// (its .ifo carries `matchaslot=`), offered only when matcha's own index in /dictionaries/<lang>
// says the word is in it. Switching tabs is an ordinary dictionary switch.
namespace DictTabs {

// The DictIndex::DICT_* bits, repeated so this header stays free of lib/Dict.
constexpr uint8_t kWords = 1;
constexpr uint8_t kGrammar = 2;
constexpr uint8_t kNames = 4;
constexpr int kMaxTabs = 3;

struct Tab {
  uint8_t slot;
  int registryIdx;
};

// `matchaslot=` value -> slot bit; 0 for none. Accepts the pre-rename matcha names too.
inline uint8_t slotFromTag(const char* tag) {
  if (!tag || !*tag) return 0;
  if (std::strcmp(tag, "vocab") == 0 || std::strcmp(tag, "jmdict") == 0) return kWords;
  if (std::strcmp(tag, "names") == 0 || std::strcmp(tag, "jmnedict") == 0) return kNames;
  if (std::strcmp(tag, "grammar") == 0) return kGrammar;
  return 0;
}

// Tabs for a group of `n` dictionaries (slot and registry index each): Words, Names, Grammar,
// the first member per slot that `hits` names. The active dictionary always holds its own slot's
// tab. Returns the count, or 0 when the active one is untagged or fewer than two tabs remain --
// one tab is just the footer's dictionary name.
inline int order(const uint8_t* slots, const int* registryIdx, const int n, const uint8_t hits, const int activeIdx,
                 Tab out[kMaxTabs]) {
  uint8_t activeSlot = 0;
  for (int i = 0; i < n; i++) {
    if (registryIdx[i] == activeIdx) activeSlot = slots[i];
  }
  if (activeSlot == 0) return 0;
  static constexpr uint8_t kOrder[kMaxTabs] = {kWords, kNames, kGrammar};
  int count = 0;
  for (const uint8_t slot : kOrder) {
    if (!(slot & (hits | activeSlot))) continue;
    int pick = -1;
    for (int i = 0; i < n; i++) {
      if (slots[i] != slot) continue;
      if (registryIdx[i] == activeIdx) {
        pick = i;
        break;
      }
      if (pick < 0) pick = i;
    }
    if (pick >= 0) out[count++] = {slot, registryIdx[pick]};
  }
  return count >= 2 ? count : 0;
}

// --- Registry glue (DictTabs.cpp) ---------------------------------------------------------
// Slot of registry entry `registryIdx`, read from its .ifo once per boot and cached. Called from
// the lookup task (installedSlots) and from the definition screen's loop, never at the same time:
// the screen only asks once its lookup has finished.
uint8_t slotOf(int registryIdx);
// OR of the slots of every dictionary in registryIdx's st-/other group.
uint8_t installedSlots(int registryIdx);
// The tabs for the active dictionary's group, `hits` from the lookup that found the word.
int collect(int activeIdx, uint8_t hits, Tab out[kMaxTabs]);

}  // namespace DictTabs
