#include "SdCardFontSystem.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include <iterator>

#include "CrossPointSettings.h"
#include "ReaderFontSizes.h"
#include "fontIds.h"

namespace {

// Point the GLOBAL reader font size at a size the active family actually ships,
// and persist it so the settings UI and the loaded font never disagree. Guarded
// by the value-change check (a no-op snap must not write SPIFFS) AND by the
// per-book override: while an override is active the loaded size is the override's,
// so persisting it would corrupt the user's global size — skip in that case.
void snapFontPointSizeTo(const uint8_t availablePointSize) {
  if (availablePointSize == 0 || availablePointSize == SETTINGS.fontPointSize) return;
  if (SETTINGS.getReaderOverride().active) return;
  LOG_DBG("SDFS", "Font size %u unavailable, snapping to %u", SETTINGS.fontPointSize, availablePointSize);
  SETTINGS.fontPointSize = availablePointSize;
  SETTINGS.saveToFile();
}

// Built-in UI fonts and their physical point sizes (at 150 DPI, matching the
// SD-font converter). Each is paired with a same-size SD fallback so CJK UI
// text matches the surrounding Latin. See SdCardFontSystem::setupUiFallbacks.
struct UiFontSize {
  int fontId;
  uint8_t pointSize;
};
constexpr UiFontSize kUiFontSizes[] = {
    {SMALL_FONT_ID, 8},
    {UI_10_FONT_ID, 10},
    {UI_12_FONT_ID, 12},
};

}  // namespace

void SdCardFontSystem::begin(GfxRenderer& renderer) {
  registry_.discover();

  // Register this system as the SD font ID resolver in settings.
  // Uses a static trampoline since CrossPointSettings stores a plain function pointer.
  SETTINGS.sdFontIdResolver = [](void* ctx, const char* familyName, uint8_t pointSize) -> int {
    return static_cast<SdCardFontSystem*>(ctx)->resolveFontId(familyName, pointSize);
  };
  SETTINGS.sdFontResolverCtx = this;

  // If user has a saved SD font selection, load it
  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    const auto* family = registry_.findFamily(SETTINGS.sdFontFamilyName);
    if (family) {
      if (manager_.loadFamily(*family, renderer, SETTINGS.fontPointSize)) {
        snapFontPointSizeTo(manager_.currentPointSize());
        setupUiFallbacks(renderer);
        LOG_DBG("SDFS", "Loaded SD card font family: %s", SETTINGS.sdFontFamilyName);
      } else {
        LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", SETTINGS.sdFontFamilyName);
        SETTINGS.clearSdFontFamily();
      }
    } else {
      LOG_DBG("SDFS", "SD font family not found on card: %s (clearing)", SETTINGS.sdFontFamilyName);
      SETTINGS.clearSdFontFamily();
    }
  }

  LOG_DBG("SDFS", "SD font system ready (%d families discovered)", registry_.getFamilyCount());
}

void SdCardFontSystem::ensureLoaded(GfxRenderer& renderer) {
  // If the web server (or another task) installed/deleted fonts, re-discover.
  // Track whether we just re-discovered so we can force a reload below even
  // when the wanted family/size still maps to the same point size — the file
  // contents on disk may have changed (e.g. user re-uploaded a new build).
  const bool registryWasDirty = registryDirty_.exchange(false, std::memory_order_acquire);
  if (registryWasDirty) {
    LOG_DBG("SDFS", "Registry dirty — re-discovering fonts");
    registry_.discover();
  }

  // Honor the per-book reader override's family AND size when active; otherwise the
  // global. The manager loads exactly one size, so the loaded size must track the
  // effective (override-aware) reader point size.
  const char* wantedFamily = SETTINGS.getReaderSdFontFamilyName();
  const uint8_t wantedPointSize = SETTINGS.getReaderFontSize();
  const std::string& currentFamily = manager_.currentFamilyName();

  // On load failure we only clear the *global* selection; when a per-book override
  // font fails we leave settings untouched (render falls back to a built-in font)
  // so a missing per-book font can't wipe the user's global font choice.
  const bool overrideActive = SETTINGS.getReaderOverride().active;
  const auto clearWantedFamily = [overrideActive]() {
    if (!overrideActive) {
      // Clears the global family, snaps the size back into the built-in set, and
      // persists both — so a missing font isn't re-loaded next boot (#2519) and the
      // size UI never offers a size nothing renders at.
      SETTINGS.clearSdFontFamily();
    }
  };

  if (wantedFamily[0] == '\0') {
    if (!currentFamily.empty()) {
      manager_.unloadAll(renderer);
    }
    // Back on a built-in family, which exists only at BUILTIN_READER_POINT_SIZES:
    // a size inherited from an SD family has to come back into that set.
    snapFontPointSizeTo(snapToNearestPointSize(BUILTIN_READER_POINT_SIZES, std::size(BUILTIN_READER_POINT_SIZES),
                                               SETTINGS.fontPointSize));
    return;
  }

  // Reload if family changed OR if the user-selected size maps to a
  // different file than what's currently loaded OR if the registry was
  // just rediscovered (file may have been replaced on disk).
  bool familyMatches = (currentFamily == wantedFamily);
  if (familyMatches) {
    const auto* family = registry_.findFamily(wantedFamily);
    if (!family) {
      LOG_DBG("SDFS", "SD font family disappeared: %s (clearing)", wantedFamily);
      manager_.unloadAll(renderer);
      clearWantedFamily();
      return;
    }
    const auto* selected = family->findNearestSize(wantedPointSize);
    const uint8_t wantedPt = selected ? selected->pointSize : 0;
    // Snap before the early return: the wanted size can already be loaded while
    // the setting still names a size this family does not ship.
    snapFontPointSizeTo(wantedPt);
    if (!registryWasDirty && wantedPt == manager_.currentPointSize()) return;
    LOG_DBG("SDFS", "Reloading %s: size %u -> %u%s", wantedFamily, manager_.currentPointSize(), wantedPt,
            registryWasDirty ? " [registry dirty]" : "");
  }

  if (!currentFamily.empty()) {
    manager_.unloadAll(renderer);
  }

  const auto* family = registry_.findFamily(wantedFamily);
  if (family) {
    if (manager_.loadFamily(*family, renderer, wantedPointSize)) {
      snapFontPointSizeTo(manager_.currentPointSize());
      setupUiFallbacks(renderer);
      LOG_DBG("SDFS", "Loaded SD font family: %s", wantedFamily);
    } else {
      LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", wantedFamily);
      clearWantedFamily();
    }
  } else {
    LOG_DBG("SDFS", "SD font family not found: %s (clearing)", wantedFamily);
    clearWantedFamily();
  }
}

void SdCardFontSystem::setupUiFallbacks(GfxRenderer& renderer) {
  const std::string& familyName = manager_.currentFamilyName();
  if (familyName.empty()) return;  // no SD family loaded — nothing to fall back to

  const auto* family = registry_.findFamily(familyName);
  if (!family) return;

  // Probe the already-loaded reader-size font before paying for the UI sizes:
  // resolveTextFontId only redirects on CJK codepoints, so a Latin-only family
  // can never act as a fallback and its UI sizes would be dead weight in RAM.
  const auto readerIt = renderer.getFontMap().find(manager_.getFontId(familyName));
  if (readerIt == renderer.getFontMap().end()) return;
  // One representative codepoint per script: Han, Hiragana, Katakana, Hangul.
  static constexpr uint32_t kCjkProbes[] = {0x4E00, 0x3042, 0x30A2, 0xAC00};
  bool hasCjk = false;
  for (const uint32_t cp : kCjkProbes) {
    if (readerIt->second.hasCodepoint(cp)) {
      hasCjk = true;
      break;
    }
  }
  if (!hasCjk) {
    LOG_DBG("SDFS", "%s has no CJK coverage - skipping UI fallback sizes", familyName.c_str());
    return;
  }

  for (const auto& ui : kUiFontSizes) {
    const int sdFontId = manager_.loadFamilyExtraSize(*family, renderer, ui.pointSize);
    if (sdFontId != 0) {
      renderer.setFallbackFont(ui.fontId, sdFontId);
    } else {
      LOG_DBG("SDFS", "No %u pt SD glyphs for UI fallback in %s", ui.pointSize, familyName.c_str());
    }
  }
}

int SdCardFontSystem::resolveFontId(const char* familyName, uint8_t pointSize) const {
  // Prefer an exactly-matching resident size. Normally there is only the reader-size
  // font and this is that font, but sizes can be loaded additively — the CJK UI
  // fallbacks, and the dictionary's own point size (see ensureFontSize) — and those
  // callers must get the size they asked for.
  const int exact = manager_.getFontIdAtSize(familyName, pointSize);
  if (exact != 0) return exact;
  // Not resident at that size: fall back to the reader-size font so text still renders.
  return manager_.getFontId(familyName);
}

int SdCardFontSystem::ensureFontSize(const char* familyName, const uint8_t pointSize, GfxRenderer& renderer) {
  if (!familyName || !*familyName) return 0;
  // Already resident (the common case: the wanted size IS the reader's size).
  const int existing = manager_.getFontIdAtSize(familyName, pointSize);
  if (existing != 0) return existing;
  // Only the currently loaded family can gain sizes — loading a second family would
  // unload the reader's (loadFamily unloads first).
  if (manager_.currentFamilyName() != familyName) return 0;

  const auto* family = registry_.findFamily(familyName);
  if (!family) return 0;

  // A second .cpfont costs its own resident interval / glyph-metadata tables on top of
  // the reader's (plus kern classes once something prewarms it), and this runs with the
  // reader activity still in memory. Decline rather than starve the render that follows;
  // the caller falls back to the reader-size font.
  constexpr size_t kMinFreeForExtraSize = 28 * 1024;
  const uint32_t freeBefore = ESP.getFreeHeap();
  if (freeBefore < kMinFreeForExtraSize) {
    LOG_DBG("SDFS", "Skipping %upt load of %s: free=%u", pointSize, familyName, (unsigned)freeBefore);
    return 0;
  }

  const int id = manager_.loadFamilyExtraSize(*family, renderer, pointSize);
  if (id == 0) {
    LOG_DBG("SDFS", "%s has no %upt file", familyName, pointSize);
    return 0;
  }
  LOG_DBG("SDFS", "Loaded %s at %upt for the dictionary: free %u -> %u", familyName, pointSize, (unsigned)freeBefore,
          (unsigned)ESP.getFreeHeap());
  return id;
}

int SdCardFontSystem::loadFamilyForPreview(const char* familyName, uint8_t pointSize, GfxRenderer& renderer) {
  if (!familyName || !*familyName) return 0;
  // Already resident (e.g. it IS the current selection)? Reuse — no SD read.
  const int existing = manager_.getFontId(familyName);
  if (existing != 0) return existing;

  const auto* family = registry_.findFamily(familyName);
  if (!family) {
    LOG_DBG("SDFS", "Preview family not found: %s", familyName);
    return 0;
  }
  manager_.unloadAll(renderer);  // one family resident at a time
  if (!manager_.loadFamily(*family, renderer, pointSize)) {
    LOG_ERR("SDFS", "Preview load failed: %s", familyName);
    return 0;
  }
  return manager_.getFontId(familyName);
}
