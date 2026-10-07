#pragma once

#include <cstddef>
#include <cstdint>

class GfxRenderer;

// The progress block every transfer screen shares: a rounded bar, then a size line
// ("1.2 MB of 3.4 MB" left, "35%" right), then a rate line ("48 KB/s · about 0:45 left").
// A transfer whose size the server never sent gets a hatched bar and "1.2 MB downloaded"
// instead of a percentage it cannot know. Draws straight into the renderer; allocates nothing.
//
// Screens repaint this only at their own throttled steps (on X3 a repaint stalls the SD
// writes the transfer makes), so every figure here is one that stays true between paints.
namespace DownloadProgress {

struct State {
  size_t done = 0;
  size_t total = 0;         // 0 = unknown
  uint32_t elapsedMs = 0;   // since the transfer started; drives the rate line
  bool shadedTrack = true;  // false on screens that hold one frame for minutes (firmware
                            // flashing): a dither held that long risks panel burn-in
};

// Height draw() uses for `width`, for screens that centre the block.
int height(const GfxRenderer& renderer);

// Draws the block with its top-left at (x, y); returns the height used.
int draw(const GfxRenderer& renderer, int x, int y, int width, const State& state);

// "512 B", "12.3 KB", "1.2 MB", "1.05 GB".
void formatBytes(char* out, size_t outSize, uint64_t bytes);

}  // namespace DownloadProgress
