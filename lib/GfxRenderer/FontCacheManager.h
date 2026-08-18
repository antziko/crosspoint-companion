#pragma once

#include <EpdFontFamily.h>

#include <cstdint>
#include <map>
#include <string>

class FontDecompressor;
class SdCardFont;

class FontCacheManager {
 public:
  FontCacheManager(const std::map<int, EpdFontFamily>& fontMap, const std::map<int, SdCardFont*>& sdCardFonts);

  void setFontDecompressor(FontDecompressor* d);

  void clearCache();
  // clearCache() that actually gives the heap back: the SD fonts' mini arenas are retained
  // across pages by default, so clearCache() frees them only under its own heap floor. Use
  // this before a heap-critical operation that will not render book text first — see the
  // KOSync TLS handshake.
  void releaseCache();
  // Returns 0 when every requested glyph was prepared, >0 for the number that were not, and
  // -1 when the prewarm could not be attempted at all (unknown font, no decompressor, no free
  // page slot). Callers that only want the side effect can ignore it; callers that need to
  // know whether the on-demand fallback will be taken at draw time must not — see the IPA
  // path in DictionaryDefinitionActivity, where the fallback costs an 11KB contiguous
  // allocation per glyph and silently drops the glyph when it fails.
  int prewarmCache(int fontId, const char* utf8Text, uint8_t styleMask = 0x0F);
  void logStats(const char* label = "render");
  void resetStats();

  // Scan-mode API: called by GfxRenderer::drawText() during scan pass
  bool isScanning() const;
  void recordText(const char* text, int fontId, EpdFontFamily::Style style);

  // The FontDecompressor pointer, needed by GfxRenderer::getGlyphBitmap()
  FontDecompressor* getDecompressor() const { return fontDecompressor_; }

  // RAII scope for two-pass prewarm pattern
  class PrewarmScope {
   public:
    explicit PrewarmScope(FontCacheManager& manager);
    ~PrewarmScope();
    void endScanAndPrewarm();
    PrewarmScope(PrewarmScope&& other) noexcept;
    PrewarmScope& operator=(PrewarmScope&&) = delete;
    PrewarmScope(const PrewarmScope&) = delete;
    PrewarmScope& operator=(const PrewarmScope&) = delete;

   private:
    FontCacheManager* manager_;
    bool active_ = true;
  };
  PrewarmScope createPrewarmScope();

 private:
  const std::map<int, EpdFontFamily>& fontMap_;
  const std::map<int, SdCardFont*>& sdCardFonts_;
  FontDecompressor* fontDecompressor_ = nullptr;

  enum class ScanMode : uint8_t { None, Scanning };
  ScanMode scanMode_ = ScanMode::None;

  // Per-font scan accumulators. A page mixes font ids -- the reader font, the UI font, and
  // the SD fallback that any CJK-bearing string resolves to -- and prewarming only the
  // first-recorded id left the other fonts to fault in glyph by glyph during the real draw
  // pass. Fixed-size: a render pass touches a handful of ids, and anything past the cap is
  // simply not batched (it falls back to the per-string prewarm in GfxRenderer).
  static constexpr uint8_t MAX_SCAN_FONTS = 4;
  struct ScanEntry {
    // Occupancy is tracked separately rather than with a sentinel id: SD font ids come from
    // SdCardFontManager::computeFontId, an FNV hash cast to int, so roughly half of them are
    // negative and no id value can mean "free". A `fontId < 0` sentinel silently never
    // latched for exactly the SD fallback fonts this batching exists to serve.
    bool used = false;
    int fontId = 0;
    std::string text;
    uint8_t styleMask = 0;
  };
  ScanEntry scanEntries_[MAX_SCAN_FONTS];
  void resetScanEntries();
};
