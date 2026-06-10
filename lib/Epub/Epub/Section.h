#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "Epub.h"

class Page;
class GfxRenderer;

class Section {
  std::shared_ptr<Epub> epub;
  const int spineIndex;
  GfxRenderer& renderer;
  std::string filePath;
  HalFile file;

  void writeSectionFileHeader(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                              uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled,
                              bool embeddedStyle, uint8_t imageRendering, bool focusReadingEnabled);
  uint32_t onPageComplete(std::unique_ptr<Page> page);

 public:
  uint16_t pageCount = 0;
  int currentPage = 0;

  // Phase-A diagnostics: createSectionFile() collapses five distinct failure
  // causes into a single false return. This captures WHICH one fired and the
  // free heap AT the failure point (before any cleanup/clear that recovers heap),
  // so the [E2] overlay + SD log report an actionable number instead of the
  // post-reset heap. floor/htmlSize are set where relevant (LowHeap), else 0.
  struct BuildFailure {
    enum class Reason : uint8_t { None = 0, Stream, LowHeap, OpenWrite, Parse, Lut };
    Reason reason = Reason::None;
    uint32_t failHeap = 0;  // esp_get_free_heap_size() at the failing return
    uint32_t floor = 0;     // required heap floor (LowHeap only)
    uint32_t htmlSize = 0;  // inflated temp-HTML size (0 if stream failed)
    // Stream sub-reason (Reason::Stream only): ZipFile::StreamResult cast to
    // uint8_t. Map via ZipFile::streamResultTag(). 0 (Ok) when not applicable.
    uint8_t streamSub = 0;
  };
  // Short tag for overlays/logs, e.g. "LOWHEAP". Never null.
  static const char* buildFailureTag(BuildFailure::Reason r);

  explicit Section(const std::shared_ptr<Epub>& epub, const int spineIndex, GfxRenderer& renderer)
      : epub(epub),
        spineIndex(spineIndex),
        renderer(renderer),
        filePath(epub->getCachePath() + "/sections/" + std::to_string(spineIndex) + ".bin") {}
  ~Section() = default;
  bool loadSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                       uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle,
                       uint8_t imageRendering, bool focusReadingEnabled);
  bool clearCache() const;
  bool createSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                         uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle,
                         uint8_t imageRendering, bool focusReadingEnabled,
                         const std::function<void(int)>& popupFn = nullptr, BuildFailure* outFailure = nullptr);
  std::unique_ptr<Page> loadPageFromSectionFile();
  std::string getTextFromSectionFile();

  // Look up the page number for an anchor id from the section cache file.
  std::optional<uint16_t> getPageForAnchor(const std::string& anchor) const;

  // Get the page count from the section cache file without fully loading it.
  std::optional<uint16_t> getCachedPageCount() const;

  // Look up the page number for a synthetic paragraph index from XPath p[N].
  std::optional<uint16_t> getPageForParagraphIndex(uint16_t pIndex) const;

  // Look up the page number for a running list-item index from the li LUT.
  std::optional<uint16_t> getPageForListItemIndex(uint16_t liIndex) const;

  // Look up the synthetic paragraph index for the given rendered page.
  std::optional<uint16_t> getParagraphIndexForPage(uint16_t page) const;
};
