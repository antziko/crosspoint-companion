#pragma once

#include <string>

class GfxRenderer;

// The finished B/W frame of a random /sleep wallpaper, saved on the way into sleep so the
// on-wake review can show it without decoding the BMP again (~1.1 s on X3). The file
// records the wallpaper path and the orientation it was drawn in; load() rejects anything
// else, and the review then decodes as before. Grayscale renders are never saved: their
// gray planes are not in the framebuffer.
namespace SleepReviewFrame {

// Writes the current framebuffer for `imagePath`. Failures only cost the fast path.
void save(const GfxRenderer& renderer, const std::string& imagePath);

// Fills the framebuffer from a frame saved for `imagePath` in the current orientation.
// One-shot: the file is removed whether or not it matched.
bool load(GfxRenderer& renderer, const std::string& imagePath);

// Removes any saved frame.
void discard();

}  // namespace SleepReviewFrame
