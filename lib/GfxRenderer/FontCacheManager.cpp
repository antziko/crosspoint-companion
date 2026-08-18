#include "FontCacheManager.h"

#include <FontDecompressor.h>
#include <Logging.h>
#include <SdCardFont.h>
#include <esp_heap_caps.h>

#include <cstring>

FontCacheManager::FontCacheManager(const std::map<int, EpdFontFamily>& fontMap,
                                   const std::map<int, SdCardFont*>& sdCardFonts)
    : fontMap_(fontMap), sdCardFonts_(sdCardFonts) {}

void FontCacheManager::setFontDecompressor(FontDecompressor* d) { fontDecompressor_ = d; }

void FontCacheManager::clearCache() {
  if (fontDecompressor_) fontDecompressor_->clearCache();
  for (auto& [id, font] : sdCardFonts_) {
    font->clearCache();
  }
}

void FontCacheManager::releaseCache() {
  if (fontDecompressor_) fontDecompressor_->clearCache();
  for (auto& [id, font] : sdCardFonts_) {
    font->releaseCache();
  }
}

int FontCacheManager::prewarmCache(int fontId, const char* utf8Text, uint8_t styleMask) {
  // SD card font prewarm path: prewarm all requested styles in one call
  auto it = sdCardFonts_.find(fontId);
  if (it != sdCardFonts_.end()) {
    int missed = it->second->prewarm(utf8Text, styleMask);
    if (missed > 0) {
      LOG_DBG("FCM", "prewarmCache(SD): %d glyph(s) not found (styleMask=0x%02X)", missed, styleMask);
    }
    return missed;
  }

  // Standard compressed font prewarm path: loop over all requested styles
  if (!fontDecompressor_ || fontMap_.count(fontId) == 0) return -1;

  // Worst case across the requested styles: one style failing to prepare is enough to put the
  // draw path back on the per-glyph hot-group fallback, so a caller checking "did this work"
  // must not see a later success mask an earlier failure. -1 (not attempted) outranks a count.
  int worst = -1;
  bool attempted = false;
  for (uint8_t i = 0; i < 4; i++) {
    if (!(styleMask & (1 << i))) continue;
    auto style = static_cast<EpdFontFamily::Style>(i);
    const EpdFontData* data = fontMap_.at(fontId).getData(style);
    if (!data || !data->groups) continue;
    int missed = fontDecompressor_->prewarmCache(data, utf8Text);
    if (missed > 0) {
      LOG_DBG("FCM", "prewarmCache: %d glyph(s) not cached for style %d", missed, i);
    }
    if (missed < 0) return -1;  // no free page slot: nothing downstream will be cached
    worst = attempted && worst > missed ? worst : missed;
    attempted = true;
  }
  return attempted ? worst : -1;
}

void FontCacheManager::logStats(const char* label) {
  if (fontDecompressor_) fontDecompressor_->logStats(label);
  for (auto& [id, font] : sdCardFonts_) {
    font->logStats(label);
  }
}

void FontCacheManager::resetStats() {
  if (fontDecompressor_) fontDecompressor_->resetStats();
  for (auto& [id, font] : sdCardFonts_) {
    font->resetStats();
  }
}

// Later scan slots hold short furniture strings (status bar, header, page number), so they
// get a small reserve -- and only when the heap can still afford one. See recordText().
constexpr size_t kLateSlotReserve = 256;
constexpr size_t kLateSlotHeadroom = 512;

bool FontCacheManager::isScanning() const { return scanMode_ == ScanMode::Scanning; }

void FontCacheManager::recordText(const char* text, int fontId, EpdFontFamily::Style style) {
  if (!text || *text == '\0') return;

  ScanEntry* entry = nullptr;
  for (auto& e : scanEntries_) {
    if (e.used && e.fontId == fontId) {
      entry = &e;
      break;
    }
    if (!e.used) {
      e.used = true;
      e.fontId = fontId;
      e.text.clear();
      // Slot 0 was reserved at scope entry, behind the heap gate there. Later slots hold
      // short furniture strings (status bar, headers) and are claimed mid-scan, when the
      // heap is in a worse state than it was at scope entry -- so re-check before
      // reserving. std::string growth goes through the global operator new, which abort()s
      // rather than returning null; an unguarded reserve here is the same crash the scope
      // gate was added to prevent. Skipping the reserve is not a failure, it just lets the
      // string grow on demand.
      if (&e != &scanEntries_[0] &&
          heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) >= kLateSlotReserve + kLateSlotHeadroom) {
        e.text.reserve(kLateSlotReserve);
      }
      entry = &e;
      break;
    }
  }
  // Every slot taken by another font: not batched. The string still gets its glyphs via the
  // per-string prewarm on the real draw pass, just without sharing this pass's SD trip.
  if (!entry) return;

  entry->text += text;
  entry->styleMask |= static_cast<uint8_t>(1u << (static_cast<uint8_t>(style) & 0x03));
}

void FontCacheManager::resetScanEntries() {
  for (auto& e : scanEntries_) {
    e.used = false;
    e.fontId = 0;
    e.styleMask = 0;
    e.text.clear();
    e.text.shrink_to_fit();
  }
}

// --- PrewarmScope implementation ---

FontCacheManager::PrewarmScope::PrewarmScope(FontCacheManager& manager) : manager_(&manager) {
  manager_->clearCache();
  manager_->resetStats();
  manager_->resetScanEntries();

  // Prewarm is a fragmentation/perf optimization, not a correctness requirement:
  // it batches glyph caching for one page. Without it, glyphs still cache lazily
  // per-draw. The scan buffer below grows via std::string, whose allocation goes
  // through the global operator new — which abort()s on OOM (no exceptions on
  // ESP32-C3). On a starved/fragmented heap the reserve() alone has crashed the
  // device (failed new of 2049B vs largest free block 2036B). Skip scanning when
  // the largest free block can't safely hold the buffer; the scope then stays
  // inert because recordText() is gated on isScanning(). largest-free-block, not
  // free-total, is what the contiguous string allocation actually needs.
  constexpr size_t kScanReserve = 2048;
  constexpr size_t kHeadroom = 1024;  // leave room for the rest of the render path
  if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < kScanReserve + kHeadroom) {
    manager_->scanMode_ = ScanMode::None;  // inert: no scan, no prewarm allocation
    LOG_DBG("FCM", "Prewarm skipped: low heap (largest block < %u)", (unsigned)(kScanReserve + kHeadroom));
    return;
  }
  manager_->scanMode_ = ScanMode::Scanning;
  // Pre-size slot 0's buffer -- the page body lands there, and this is the allocation the
  // gate above just priced. Left unclaimed (used=false) so recordText assigns it to the
  // first font id it sees; its clear() keeps the capacity reserved here.
  manager_->scanEntries_[0].text.reserve(kScanReserve);  // avoid fragmentation from repeated concat
}

void FontCacheManager::PrewarmScope::endScanAndPrewarm() {
  manager_->scanMode_ = ScanMode::None;

  // One prewarm per font id that actually appeared. styleMask carries the styles seen for
  // that id; an entry that recorded text but no style bit defaults to regular.
  for (auto& e : manager_->scanEntries_) {
    if (!e.used || e.text.empty()) continue;
    manager_->prewarmCache(e.fontId, e.text.c_str(), e.styleMask != 0 ? e.styleMask : 1);
  }
  manager_->resetScanEntries();
}

FontCacheManager::PrewarmScope::~PrewarmScope() {
  if (active_) {
    endScanAndPrewarm();  // no-op if already called (the scan entries are released there)
    manager_->clearCache();
  }
}

FontCacheManager::PrewarmScope::PrewarmScope(PrewarmScope&& other) noexcept
    : manager_(other.manager_), active_(other.active_) {
  other.active_ = false;
}

FontCacheManager::PrewarmScope FontCacheManager::createPrewarmScope() { return PrewarmScope(*this); }
