#pragma once

#include <Epub/Page.h>
#include <Epub/PageLink.h>
#include <I18n.h>

#include <memory>
#include <vector>

#include "activities/Activity.h"

// Link selection overlaid on the current reader page, modeled on DictionaryWordSelectActivity:
// the page renders underneath and each internal link (footnote marker, index page number,
// cross-reference) gets a highlight box. Up/Down/Left/Right move between links, Confirm jumps,
// Back returns to the reader. Lists the page's links, not its footnote entries, so a dense index
// page (up to Page::MAX_LINKS_PER_PAGE links) is reachable past the footnote cap.
class EpubReaderFootnoteSelectActivity final : public Activity {
 public:
  explicit EpubReaderFootnoteSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                            std::unique_ptr<Page> page, int marginLeft, int marginTop)
      : Activity("EpubReaderFootnoteSelect", renderer, mappedInput),
        page(std::move(page)),
        marginLeft(marginLeft),
        marginTop(marginTop) {}

 private:
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void buildFootnoteLinks();
  void performJump();
  bool drawHighlightWithSnapshot();
  void drawHints() const;

  std::unique_ptr<Page> page;
  const int marginLeft;
  const int marginTop;

  struct FootnoteLink {
    const PageLink* link;
    size_t linkIndex;  // into page->links
  };
  std::vector<FootnoteLink> footnoteLinks;
  int selected = 0;

  // Differential highlight repaint: pixels under the current highlight box.
  static constexpr size_t SNAPSHOT_CAPACITY = 4096;
  std::unique_ptr<uint8_t[]> snapshot;
  int16_t snapshotX = 0;
  int16_t snapshotY = 0;
  int16_t snapshotW = 0;
  int16_t snapshotH = 0;
  int snapshotIdx = -1;

  unsigned long lastHorizontalMoveTime = 0;
};
