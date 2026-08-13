#pragma once

#include <atomic>
#include <string>
#include <vector>

// One installed StarDict dictionary discovered on the SD card.
struct DictionaryEntry {
  std::string name;      // folder name, e.g. "dict-en-en"
  std::string stem;      // file stem (".idx"/".ifo" base), e.g. "dict-data"
  std::string basePath;  // <root>/<name>/<stem>, e.g. "/.dictionaries/dict-en-en/dict-data"
  // Which family the long-press dictionary switch keeps this entry in: true when the
  // .ifo's sametypesequence starts with 'm' (plain-text definitions), false for every
  // other value including an absent one. Read once at discover() time — see the cost
  // note there. A bool rather than the type string so an entry gains one byte instead
  // of a fourth heap allocation; the settings screen reads its own DictInfo for display
  // (DictionarySelectActivity.cpp:402).
  bool typeIsM = false;
};

// Discovers installed dictionaries on the SD card. Mirrors SdCardFontRegistry:
// discover() scans the card and populates entries_; the settings UI enumerates
// them on both device and web. The *active* selection is not stored here — it
// lives in dictionary.bin (see Dictionary::readDictPath/saveGlobalDictPath).
class DictionaryRegistry {
 public:
  // Scan the SD card, populate entries_ (sorted by folder name). Returns true if any found.
  bool discover();

  const std::vector<DictionaryEntry>& getEntries() const { return entries_; }
  int count() const { return static_cast<int>(entries_.size()); }
  const std::string& root() const { return root_; }

  // Index of the entry whose basePath == path, or -1 if not found / path empty.
  int indexOf(const std::string& basePath) const;

  // Next index after `current` in a list of `count` items, wrapping at the end.
  // Returns -1 for an empty list. Any out-of-range `current` — including the -1
  // indexOf() returns when the configured dictionary isn't among the installed
  // entries — starts the cycle at 0. Static and stateless so it is host-testable
  // without stubbing the SD-backed discovery half of this class.
  static int nextIndex(int current, int count) {
    if (count <= 0) return -1;
    if (current < 0 || current >= count) return 0;
    return (current + 1) % count;
  }

  // Next index after `current` whose group matches current's, wrapping at the end.
  // Grouping is the type-'m' partition described on DictionaryEntry::typeIsM: an 'm'
  // dictionary only ever cycles to another 'm', and a non-'m' only to another non-'m'.
  //
  // Returns -1 when the group has no other member, which is what makes a long press on
  // the only dictionary of its type do nothing rather than pointlessly re-running the
  // same lookup against the same dictionary — `current` is never a candidate for itself.
  // Out-of-range `current` (including the -1 from indexOf() for a configured dictionary
  // that is not installed) has no group to match, so it starts the cycle at 0, matching
  // nextIndex's contract above.
  //
  // Reads the group flag through a plain function pointer rather than off entries_, so
  // it stays static and stateless and the host test can drive it without stubbing the
  // SD-backed discovery half of this class — same reason nextIndex is split out. The
  // accessor also means the instance wrapper below shares this scan instead of carrying
  // a second copy of it. Function pointer + ctx rather than std::function for the usual
  // reason (see DictLookupCallbacks in Dictionary.h): no heap, no vtable, no bloat.
  static int nextIndexInGroup(int current, int count, bool (*groupOf)(const void* ctx, int index), const void* ctx) {
    if (count <= 0 || groupOf == nullptr) return -1;
    if (current < 0 || current >= count) return 0;
    const bool wanted = groupOf(ctx, current);
    for (int i = 1; i < count; i++) {
      const int candidate = (current + i) % count;
      if (groupOf(ctx, candidate) == wanted) return candidate;
    }
    return -1;  // sole member of its group
  }

  // nextIndexInGroup over the installed entries. No SD access — safe from an input path.
  int nextEntryIndexInGroup(int current) const;

  // Mark the registry as needing a re-scan. Thread-safe (callable from the web task).
  void markDirty() { dirty_.store(true, std::memory_order_release); }

  // Re-scan if marked dirty, then clear the flag. Mirrors SdCardFontSystem::refreshIfDirty().
  void refreshIfDirty() {
    if (dirty_.exchange(false, std::memory_order_acquire)) discover();
  }

 private:
  std::vector<DictionaryEntry> entries_;  // sorted alphabetically by name
  std::string root_;                      // active dictionary root dir on the SD card
  std::atomic<bool> dirty_{false};
};

// Global dictionary registry instance (defined in main.cpp).
extern DictionaryRegistry dictionaryRegistry;
