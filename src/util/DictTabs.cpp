#include "DictTabs.h"

#include <Logging.h>
#include <Memory.h>

#include "util/Dictionary.h"
#include "util/DictionaryRegistry.h"

namespace DictTabs {

namespace {

struct CachedSlot {
  uint32_t nameHash;
  uint8_t slot;
};
constexpr int kCacheSize = 16;
CachedSlot g_cache[kCacheSize] = {};
int g_cacheCount = 0;
int g_cacheNext = 0;

// Members of registryIdx's group, at most kMaxMembers.
constexpr int kMaxMembers = 16;
int groupOf(const int registryIdx, int* members) {
  const auto& entries = dictionaryRegistry.getEntries();
  if (registryIdx < 0 || registryIdx >= static_cast<int>(entries.size())) return 0;
  const bool st = entries[registryIdx].nameIsSt;
  int n = 0;
  for (int i = 0; i < static_cast<int>(entries.size()) && n < kMaxMembers; i++) {
    if (entries[i].nameIsSt == st) members[n++] = i;
  }
  return n;
}

}  // namespace

uint8_t slotOf(const int registryIdx) {
  const auto& entries = dictionaryRegistry.getEntries();
  if (registryIdx < 0 || registryIdx >= static_cast<int>(entries.size())) return 0;
  const uint32_t hash = DictionaryRegistry::nameHash(entries[registryIdx].name.c_str());
  for (int i = 0; i < g_cacheCount; i++) {
    if (g_cache[i].nameHash == hash) return g_cache[i].slot;
  }
  // DictInfo is ~620 bytes: heap, not the 256-byte stack budget. Once per dictionary per boot.
  auto info = makeUniqueNoThrow<DictInfo>();
  if (!info) {
    LOG_ERR("DTAB", "OOM: DictInfo");
    return 0;
  }
  const uint8_t slot =
      Dictionary::readInfoInto(entries[registryIdx].basePath.c_str(), *info) ? slotFromTag(info->matchaSlot) : 0;
  g_cache[g_cacheNext] = {hash, slot};
  g_cacheNext = (g_cacheNext + 1) % kCacheSize;
  if (g_cacheCount < kCacheSize) g_cacheCount++;
  return slot;
}

uint8_t installedSlots(const int registryIdx) {
  int members[kMaxMembers];
  const int n = groupOf(registryIdx, members);
  uint8_t slots = 0;
  for (int i = 0; i < n; i++) slots |= slotOf(members[i]);
  return slots;
}

int collect(const int activeIdx, const uint8_t hits, Tab out[kMaxTabs]) {
  int members[kMaxMembers];
  const int n = groupOf(activeIdx, members);
  uint8_t slots[kMaxMembers];
  for (int i = 0; i < n; i++) slots[i] = slotOf(members[i]);
  return order(slots, members, n, hits, activeIdx, out);
}

}  // namespace DictTabs
