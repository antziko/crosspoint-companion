#pragma once

#include <cstdint>
#include <memory>

// Ghost diagnostics: per-tile history of the drive each region of the glass received while its
// content held still. A differential or unipolar waveform still drives an UNCHANGED pixel
// (X3 `_half` re-drives it fully toward its current colour; the fast bank tops it up), so the
// pre-sleep imprint is set by how many such paints a tile's content sat through, how long it
// sat, and how long the rails were up meanwhile -- none of which the session-wide paint counts
// can tell apart.
//
// The panel is cut into GRID x GRID tiles in NATIVE framebuffer space. Each paint re-hashes the
// tiles; a tile whose hash changed starts a new history (its counters are snapshotted from the
// global totals), one that did not keeps accumulating. Gray paints are counted globally only:
// they do not replace the 1-bit frame.
//
// Allocated lazily (~2 KB) and only while SD logging is on, so it costs nothing otherwise.
class PanelDoseTrace {
 public:
  enum Kind : uint8_t { Fast, Scrub, Half, Full, Gray, KIND_COUNT };

  // Call after a paint reached the panel. `fb` is the 1-bit frame that was just sent.
  void notePaint(const uint8_t* fb, uint16_t widthBytes, uint16_t height, Kind kind, unsigned long railsMs);
  // A paint that does not replace the 1-bit frame (grayscale base/overlay, precondition).
  void noteGlobal(Kind kind);
  // Writes the grids to the SD log. `why` names the moment (e.g. "sleep").
  void dump(const char* why, uint16_t widthBytes, uint16_t height, int orientation, unsigned long railsMs) const;

 private:
  static constexpr int GRID = 12;
  struct Tile {
    uint16_t hash;
    uint16_t fastAt;
    uint8_t at[KIND_COUNT];  // scrub/half/full/gray snapshots; at[Fast] unused (fastAt is wider)
    uint16_t secAt;          // seconds since boot when the content last changed
    uint16_t railsSecAt;     // rails-up seconds at that moment
    uint8_t inkPct;
  };
  bool ensureTable();
  uint16_t totalFast = 0;
  uint8_t total[KIND_COUNT] = {};
  std::unique_ptr<Tile[]> tiles;
  bool allocFailed = false;
};
