#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

// Which language group a dictionary belongs to, from its folder name (DictionaryRegistry::langForName).
// Switching and fallback stay inside one group, and a book shows only its own language's group.
enum class DictLang : uint8_t { Other = 0, Chinese = 1, Japanese = 2 };

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
  // Language group, from the folder name at discover() time (langForName).
  DictLang lang = DictLang::Other;
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

  // Language group of a dictionary folder: "st-" or "zh-" prefix is Chinese, a "ja-" prefix or a
  // "-ja-" anywhere (jmdict-ja-en) is Japanese, anything else is the English / other group.
  // Folder names, not the .ifo's optional lang=, for the reason nameIsSt gives above.
  static DictLang langForName(const char* name) {
    if (!name) return DictLang::Other;
    if (nameIsStGroup(name) || strncasecmp(name, "zh-", 3) == 0) return DictLang::Chinese;
    if (strncasecmp(name, "ja-", 3) == 0) return DictLang::Japanese;
    for (const char* p = name; p[0] && p[1] && p[2] && p[3]; p++) {
      if (p[0] == '-' && (p[1] | 0x20) == 'j' && (p[2] | 0x20) == 'a' && p[3] == '-') return DictLang::Japanese;
    }
    return DictLang::Other;
  }

  // The folder component of a dictionary base path ("<root>/<folder>/<stem>"), or "" when the
  // path has no folder. Shared by langForBasePath and the dictionary picker's labels.
  static std::string_view folderOfBasePath(std::string_view basePath) {
    const size_t slash = basePath.rfind('/');
    if (slash == std::string_view::npos || slash == 0) return {};
    const size_t start = basePath.rfind('/', slash - 1);
    const size_t from = start == std::string_view::npos ? 0 : start + 1;
    return basePath.substr(from, slash - from);
  }

  // langForName for a dictionary base path: the folder decides, never the stem.
  static DictLang langForBasePath(std::string_view basePath) {
    const std::string_view folder = folderOfBasePath(basePath);
    char name[64];
    const size_t len = std::min(folder.size(), sizeof(name) - 1);
    memcpy(name, folder.data(), len);
    name[len] = '\0';
    return langForName(name);
  }

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
  // The production grouping is the language partition (langForName): a Chinese dictionary only
  // ever cycles to another Chinese one, and so on.
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
  static int nextIndexInGroup(int current, int count, int (*groupOf)(const void* ctx, int index), const void* ctx) {
    if (count <= 0 || groupOf == nullptr) return -1;
    if (current < 0 || current >= count) return 0;
    const int wanted = groupOf(ctx, current);
    for (int i = 1; i < count; i++) {
      const int candidate = (current + i) % count;
      if (groupOf(ctx, candidate) == wanted) return candidate;
    }
    return -1;  // sole member of its group
  }

  // nextIndexInGroup over the installed entries. No SD access — safe from an input path.
  int nextEntryIndexInGroup(int current) const;

  // First installed dictionary of `lang`, or -1. No SD access.
  int firstIndexInLang(DictLang lang) const {
    for (size_t i = 0; i < entries_.size(); i++) {
      if (entries_[i].lang == lang) return static_cast<int>(i);
    }
    return -1;
  }

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

// Tell the dictionary layer which language group the open book reads in (Dictionary::setBookLang)
// and resolve that group's default: the remembered choice for the language, else the global
// dictionary when it is in the group, else the group's first installed dictionary. A group with
// nothing installed leaves the book unfiltered on the global dictionary. Pass
// Dictionary::kNoBookLang when no book is open. UI task only.
void applyBookDictLang(uint8_t lang);

// The default dictionary of language group `lang` as applyBookDictLang resolves it, or empty when
// that group has nothing installed. Reads the SD card; UI task only.
std::string resolveLangDictPath(uint8_t lang);
