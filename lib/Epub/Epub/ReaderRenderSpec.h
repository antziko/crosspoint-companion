#pragma once
#include <cstdint>

// The resolved text-rendering configuration a reader hands to the layout engine.
// Section-cache validation keys on every field: a section file built with a
// different spec is discarded and rebuilt.
//
// Build one via CrossPointSettings::readerRenderSpec(width, height), which fills
// every field: the settings-derived ones from the store, the viewport from the
// caller. Taking the viewport as arguments is what keeps a spec from existing in
// a half-filled state — the 0 defaults below are a last-resort backstop (a 0x0
// viewport lays out nothing), not an invitation to omit it.
//
// Passing one struct rather than a parameter list is load-bearing for the cache:
// the fields that carry a per-book override must be read through the
// override-aware getters at every site, and a list of ten positional arguments
// made it possible for a build site to read the global value while every load
// site read the override. The factory is the single place that resolution
// happens.
struct ReaderRenderSpec {
  int fontId = 0;
  float lineCompression = 1.0f;
  bool extraParagraphSpacing = false;
  uint8_t paragraphAlignment = 0;
  uint16_t viewportWidth = 0;
  uint16_t viewportHeight = 0;
  bool hyphenationEnabled = false;
  bool embeddedStyle = true;
  uint8_t imageRendering = 0;
  bool focusReadingEnabled = false;
};
