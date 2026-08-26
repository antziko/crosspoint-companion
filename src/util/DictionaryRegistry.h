#pragma once

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

// One installed StarDict dictionary discovered on the SD card.
struct DictionaryEntry {
  std::string name;      // folder name, e.g. "dict-en-en"
  std::string stem;      // file stem (".idx"/".ifo" base), e.g. "dict-data"
  std::string basePath;  // <root>/<name>/<stem>, e.g. "/.dictionaries/dict-en-en/dict-data"
  // Which family the long-press dictionary switch keeps this entry in — see
  // nameIsStGroup() below for the rule. Derived from `name` at discover() time and cached
  // as one byte so the input path never re-derives it.
  //
  // This used to come from the .ifo's sametypesequence. It does not any more: that field is
  // OPTIONAL in the StarDict spec, so an absent or unexpected value silently filed a
  // dictionary in the wrong group with nothing on screen to show it had happened. A folder
  // name is visible in the settings list and cannot be misread.
  bool nameIsSt = false;
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

  // Stable 32-bit identity for a dictionary, hashed from its FOLDER NAME.
  //
  // Used by FlashcardDeck to remember which dictionary a card was saved from, so the card's
  // back face is looked up in that dictionary rather than whatever happens to be active. The
  // value travels between devices on the sync wire, which fixes three properties:
  //
  //   - Folder name, not basePath: the folder is what the user copies between devices; the
  //     root prefix is a local detail and can differ.
  //   - FNV-1a, not std::hash: std::hash is unspecified and may differ across libstdc++
  //     versions, so it cannot be a wire value. (Its use for epub cache paths is fine — those
  //     never leave the device.)
  //   - A hash, not an index into entries_: entries_ is sorted by name and renumbers whenever
  //     a dictionary is added or removed, which would silently repoint every existing card.
  //
  // Case-insensitive, matching nameIsStGroup() and discover()'s sort, so a folder copied from
  // a case-preserving filesystem still resolves. 0 is reserved for "unset": the empty name maps
  // to it by construction, and no real folder name is expected to (FNV-1a can only reach 0 if an
  // intermediate hash happens to equal the very next byte, which needs a value below 256 at that
  // step). A folder that did collide would simply read as "unrecorded" and fall back to the
  // active dictionary — a cosmetic miss, not corruption.
  static uint32_t nameHash(const char* name) {
    if (name == nullptr || name[0] == '\0') return 0;
    uint32_t h = 2166136261u;  // FNV-1a 32 offset basis
    for (const char* p = name; *p != '\0'; ++p) {
      h ^= static_cast<uint32_t>(static_cast<unsigned char>(tolower(static_cast<unsigned char>(*p))));
      h *= 16777619u;  // FNV prime
    }
    return h;
  }

  // Index of the entry whose folder name hashes to `hash`, or -1 when none does — which is the
  // normal case for a card synced from a device with a different dictionary set, or one whose
  // dictionary has since been deleted. Callers fall back to the active dictionary.
  int indexOfHash(uint32_t hash) const;

  // The entire grouping rule for the long-press switch: a folder name beginning "st-",
  // case-insensitively, is one group and everything else is the other.
  //
  // Case-insensitive because the rest of this class already is — discover() sorts through
  // tolower() (below) and skips .DS_Store through strcasecmp — and because a folder copied
  // from a case-preserving filesystem would otherwise land in the wrong group.
  //
  // Static and stateless so the host test can drive it without stubbing the SD-backed
  // discovery half of this class, the same reason nextIndex is split out.
  static bool nameIsStGroup(const char* name) { return name != nullptr && strncasecmp(name, "st-", 3) == 0; }

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
  // Grouping is the name partition described on nameIsStGroup above: an "st-" dictionary
  // only ever cycles to another "st-" one, and a non-"st-" only to another non-"st-".
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

  // nextIndexInGroup walked the other way, so a select mode can step both directions.
  // Identical contract, including the -1 for a sole group member and the start-at-0
  // fallback for an out-of-range `current`.
  static int prevIndexInGroup(int current, int count, bool (*groupOf)(const void* ctx, int index), const void* ctx) {
    if (count <= 0 || groupOf == nullptr) return -1;
    if (current < 0 || current >= count) return 0;
    const bool wanted = groupOf(ctx, current);
    for (int i = 1; i < count; i++) {
      const int candidate = ((current - i) % count + count) % count;
      if (groupOf(ctx, candidate) == wanted) return candidate;
    }
    return -1;  // sole member of its group
  }

  // nextIndexInGroup over the installed entries. No SD access — safe from an input path.
  int nextEntryIndexInGroup(int current) const;

  // prevIndexInGroup over the installed entries. Same guarantees as the next() half.
  int prevEntryIndexInGroup(int current) const;

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
