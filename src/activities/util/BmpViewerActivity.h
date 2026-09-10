#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "MappedInputManager.h"
#include "activities/Activity.h"
#include "activities/TouchActionBar.h"

class BmpViewerActivity final : public Activity {
 public:
  BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  // Re-renders the current image. Invoked on every requestUpdate (e.g. the
  // power-button manual refresh), so a manual refresh actually redraws the
  // bitmap instead of leaving the cleared (white) framebuffer on screen.
  void render(RenderLock&&) override;

 private:
  // Find the immediate previous/next sibling image (.bmp or .png) of the current file by a
  // single bounded directory scan (largest name < current, smallest name > current). Holds
  // O(1) RAM regardless of how many images the folder has — a folder of thousands of sleep
  // BMPs would OOM-abort if every name were materialised into a vector.
  void computeSiblings();
  void renderImage();
  // Replaces the current image with a sibling and re-enters, so the neighbours
  // and the cover state are recomputed for it. No-op on an empty name.
  void showSibling(const std::string& name);
  // Rebuild the on-screen answers for touch boards, which draw no button hints:
  // the same "< / cover action / >" row, minus whatever is unavailable here.
  void layoutActionBar(bool hasPrevious, bool hasNext, const char* coverLabel);
  // Decode and draw the current file through the PNG converter. Returns false when the
  // file cannot be read or has unusable dimensions; the caller then shows the error page.
  bool renderPngImage(int pageWidth, int pageHeight);
  // Only a BMP can become the sleep cover: SleepActivity reads /sleep.bmp exclusively, so
  // offering "Set Cover" for a PNG would write a file it never looks at.
  bool canSetSleepCover() const;
  void doSetSleepCover();
  void doClearSleepCover();

  std::string filePath;
  // Immediate neighbours of the current image (filenames only, empty = none). Recomputed
  // each time the shown image changes, so navigation never holds the whole listing.
  std::string prevName;
  std::string nextName;
  // True while a sleep cover (/sleep.bmp) exists: the Confirm button then clears
  // it instead of setting one. Refreshed in onEnter().
  bool coverExists = false;

  // On-screen answers for touch boards (see TouchActionBar). The bar is built
  // per image, so its buttons -- and therefore these parallel arrays -- vary
  // with what this image offers.
  enum class BarAction : uint8_t { Previous, Cover, Next };
  TouchActionBar actionBar_;
  const char* barLabels_[TouchActionBar::kMaxButtons] = {};
  BarAction barActions_[TouchActionBar::kMaxButtons] = {};
  int barCount_ = 0;
};