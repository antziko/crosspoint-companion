// Vertical (tategaki) reading for EpubReaderActivity, on matcha-reader's VerticalSection engine.
// Function names follow matcha's EpubReaderActivity so its fixes can be carried across by name;
// the bodies are written against this reader (see docs/matcha-sync.md).
#include <BoardConfig.h>
#include <Epub/PageTextExtractor.h>
#include <Epub/blocks/ImageBlock.h>
#include <Epub/blocks/VerticalTextBlock.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <SdDebugLog.h>
#include <TrustedTime.h>
#include <Utf8.h>

#include <algorithm>
#include <limits>

#include "BookmarkStore.h"
#include "CrossPointSettings.h"
#include "DictionaryWordSelectActivity.h"
#include "EpubReaderActivity.h"
#include "VerticalPageMarks.h"
#include "WordSelectionScan.h"
#include "ReaderUtils.h"
#include "VerticalWordTokens.h"
#include "components/UIScale.h"
#include "components/UITheme.h"

namespace {
// One build slice: long enough to lay out a few pages, short enough that a press is answered.
constexpr uint32_t kVerticalSliceMs = 30;
// The prewarm's page string and page-slot claims are plain allocations; below these the page
// draws on demand instead. The build-phase floor is higher: an OOM there drops layout glyphs.
constexpr uint32_t kPrewarmMinAllocRead = 12 * 1024;
constexpr uint32_t kPrewarmMinAllocBuild = 20 * 1024;
constexpr int kGrayStripRows = 80;

// Same panels the horizontal path treats as 1-bit image panels (EpubReaderActivity.cpp).
bool oneBitImagePanel(const GfxRenderer& renderer) { return renderer.isX3() || BoardConfig::isX4Pro(); }

// matcha's column-gap table has three steps (tight, normal, wide); ours has six line spacings.
uint8_t verticalLineSpacingLevel(const uint8_t lineSpacing) {
  if (lineSpacing == CrossPointSettings::TIGHT) return 0;
  if (lineSpacing <= CrossPointSettings::RELAXED) return 1;
  return 2;
}

void readerTextOrigin(const GfxRenderer& renderer, int& top, int& left) {
  int right, bottom;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  top += SETTINGS.getReaderScreenMargin();
  left += SETTINGS.getReaderScreenMargin();
}
}  // namespace

int EpubReaderActivity::pageIndex() const {
  if (verticalSection) return verticalSection->currentPage;
  if (section) return section->currentPage;
  return nextPageNumber;
}

int EpubReaderActivity::pageTotal() const {
  if (verticalSection) return verticalSection->pageCount;
  if (section) return section->estimatedTotalPages();
  return cachedChapterTotalPageCount;
}

void EpubReaderActivity::rememberPositionForReflow() {
  if (!hasPage()) return;
  rememberCurrentContentOffset();
  cachedSpineIndex = currentSpineIndex;
  cachedChapterTotalPageCount = section ? section->pageCount : verticalSection->pageCount;
  nextPageNumber = pageIndex();
}

bool EpubReaderActivity::readsRightToLeft() const {
  return verticalSection || (epub && ReaderUtils::isRtlBookLanguage(epub->getLanguage()));
}

bool EpubReaderActivity::readerFontCoversScript() const {
  const int fontId = SETTINGS.getReaderFontId();
  int left, width, top, height;
  return renderer.getGlyphMetrics(fontId, cjk::probeCodepoint(bookScript_), EpdFontFamily::REGULAR, &left, &width, &top,
                                  &height);
}

bool EpubReaderActivity::useVerticalText() const {
  if (!epub || !cjkPrefs.verticalOn(bookScript_, epub->pageProgressionRtl())) return false;
  // The vertical engine draws glyphs straight from the reader font, with no per-string fallback:
  // a Latin font would leave the page blank, so such a book stays horizontal.
  return readerFontCoversScript();
}

ReaderRenderSpec EpubReaderActivity::verticalSpec(const uint16_t viewportWidth, const uint16_t viewportHeight,
                                                  const int marginRight) const {
  ReaderRenderSpec spec = readerSpec(viewportWidth, viewportHeight);
  const auto& ov = SETTINGS.getReaderOverride();
  spec.lineSpacingLevel = verticalLineSpacingLevel(ov.active ? ov.lineSpacing : SETTINGS.lineSpacing);
  // The layout gives the first column's ruby only what the margin lacks, so with furigana on
  // it runs to the screen edge. Report no margin: the whole ruby gutter then sits inside the
  // text area and the reader's margin stays clear beyond it.
  spec.rightMarginPx = spec.furiganaEnabled ? 0 : static_cast<uint16_t>(std::max(0, marginRight));
  return spec;
}

bool EpubReaderActivity::prewarmVerticalPageGlyphs(const VerticalPage& vpage) {
  // Bulk-load every glyph the page needs before drawing: otherwise each uncached kanji is its
  // own SD round-trip. clearCache() first: a prewarm claims a page slot and never frees it.
  auto* fcm = renderer.getFontCacheManager();
  const uint32_t floorBytes =
      verticalSection && verticalSection->isBuilding() ? kPrewarmMinAllocBuild : kPrewarmMinAllocRead;
  if (!fcm || ESP.getMaxAllocHeap() < floorBytes) return false;
  prewarmedVPage_ = -1;
  fcm->clearCache();
  // Only the styles present: each requested style claims a slot.
  uint8_t styleMask = 0;
  for (const auto& g : vpage.glyphs) styleMask |= static_cast<uint8_t>(1u << (g.style & 0x03));
  if (styleMask == 0) styleMask = 1u << EpdFontFamily::REGULAR;
  const std::string pageText = PageTextExtractor::fromVerticalPage(vpage);
  fcm->prewarmCache(SETTINGS.getReaderFontId(), pageText.c_str(), styleMask);
  return true;
}

void EpubReaderActivity::renderVerticalPageBody(const VerticalPage& vpage, const bool glyphsAlreadyWarm,
                                                const bool imagesOnly, const bool marks) {
  if (!imagesOnly && !glyphsAlreadyWarm && prewarmVerticalPageGlyphs(vpage) && verticalSection) {
    prewarmedVPage_ = verticalSection->currentPage;
  }
  int marginTop, marginLeft;
  readerTextOrigin(renderer, marginTop, marginLeft);
  if (vpage.isImagePage()) {
    int iw = vpage.imageWidth;
    int ih = vpage.imageHeight;
    ImageBlock::fitWithin(buildViewportWidth, buildViewportHeight, iw, ih);
    ImageBlock block(vpage.imagePath, vpage.imageSrcPath, static_cast<int16_t>(iw), static_cast<int16_t>(ih));
    block.render(renderer, marginLeft + (buildViewportWidth - iw) / 2, marginTop + (buildViewportHeight - ih) / 2);
    return;
  }
  VerticalTextBlock block(vpage);
  if (imagesOnly) {
    block.renderImages(renderer, marginLeft, marginTop);
    return;
  }
  const int fontId = SETTINGS.getReaderFontId();
  if (useFurigana()) {
    // Ruby in the body font: its SUP style draws at 50%, matching the half-em gap the layout keeps.
    block.render(renderer, fontId, fontId, marginLeft, marginTop, true);
  } else {
    block.render(renderer, fontId, marginLeft, marginTop, true);
  }
  if (marks) drawVerticalMarks(vpage, marginLeft, marginTop);
}

void EpubReaderActivity::drawVerticalMarks(const VerticalPage& vpage, const int marginLeft, const int marginTop) {
  quoteOnThisPaint_ = false;
  if (!verticalSection || verticalSection->pageCount <= 0) return;
  const int fontId = SETTINGS.getReaderFontId();
  const int cellPx = verticalCellPx(renderer, fontId);
  const int page = verticalSection->currentPage;
  const int pageCount = verticalSection->pageCount;

  // Quotes of this chapter whose text is on this page. A quote saved here carries the exact
  // chapter offset of its first character; one saved on a horizontal page carries its page's
  // first offset (or none) and is found by its opening words, near where it was saved.
  constexpr size_t kMaxQuotes = 6;
  constexpr int kRelocatePages = 3;
  VerticalPageMarks::QuoteRange quotes[kMaxQuotes];
  size_t quoteCount = 0;
  if (SETTINGS.quoteHighlightStyle != CrossPointSettings::QUOTE_STYLE_OFF) {
    for (const auto& b : BOOKMARKS.getBookmarks()) {
      if (quoteCount >= kMaxQuotes) break;
      if (!b.quote || b.spineIndex != static_cast<uint16_t>(currentSpineIndex)) continue;
      bool candidate = false;
      if (b.visibleTextOffset != 0) {
        const auto at = verticalSection->getPageForVisibleTextOffset(b.visibleTextOffset, true);
        candidate = at.has_value() && (*at == page || *at + 1 == page);
      } else {
        const float saved = b.progress * static_cast<float>(pageCount);
        candidate = saved > page - kRelocatePages - 1 && saved < page + kRelocatePages + 1;
      }
      if (!candidate) continue;
      uint16_t start = 0;
      if (!VerticalPageMarks::tokenAtOffset(renderer, vpage, fontId, marginLeft, marginTop, cellPx,
                                            b.visibleTextOffset, b.snippet, start) &&
          !VerticalPageMarks::findSnippet(renderer, vpage, fontId, marginLeft, marginTop, cellPx, b.snippet, start)) {
        continue;
      }
      const int words = std::abs(static_cast<int>(b.endWord) - static_cast<int>(b.startWord));
      quotes[quoteCount++] = {start, static_cast<uint16_t>(std::min(start + words, 0xFFFF))};
    }
  }
  quoteOnThisPaint_ = quoteCount > 0;
  VerticalPageMarks::draw(renderer, vpage, fontId, marginLeft, marginTop, cellPx, currentChapterHash(), page + 1,
                          pageCount, quotes, quoteCount);
}

void EpubReaderActivity::presentVerticalPage(const VerticalPage& vpage, const bool glyphsWarm) {
  struct PxcSlotGuard {
    ~PxcSlotGuard() { ImageBlock::releaseRenderCache(); }
  } pxcSlotGuard;

  renderer.setImageDitherMode(SETTINGS.imageDither);
  const uint8_t aaMode = SETTINGS.getReaderTextAntiAliasing();
  renderer.setOneBitImages(oneBitImagePanel(renderer) || aaMode == CrossPointSettings::TEXT_AA_OFF);
  renderer.setTextAntiAlias(aaMode == CrossPointSettings::TEXT_AA_ANTIALIASED);

  const bool hasImages = vpage.isImagePage() || VerticalTextBlock(vpage).hasImages();
  const bool grayImages = hasImages && !oneBitImagePanel(renderer) && aaMode != CrossPointSettings::TEXT_AA_OFF;
  const bool doGray = aaMode == CrossPointSettings::TEXT_AA_ANTIALIASED || grayImages;
  // Antialiased: the text contributes gray too. Otherwise the planes only carry the images.
  const bool grayImagesOnly = aaMode != CrossPointSettings::TEXT_AA_ANTIALIASED;
  lastPageHadImages = hasImages;
  lastPageUsedGrayscale = false;

  renderer.clearScreen();
  renderVerticalPageBody(vpage, glyphsWarm);
  renderStatusBar();

  if (grayImages) {
    // A HALF base sets the particles too firmly for the gray planes that follow (see renderContents).
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  } else {
    // Top bit: a vertical page is never the same frame as the horizontal page of that number.
    cleanRefreshForNightQuotes(0x80000000u | (static_cast<uint32_t>(currentSpineIndex) << 16) |
                               static_cast<uint16_t>(verticalSection ? verticalSection->currentPage : 0));
    if (pagesUntilFullRefresh <= 1) statsCheckpointPending = true;
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh, /*async=*/false,
                                         /*grayscaleFollows=*/doGray && renderer.supportsStripGrayscale());
  }
  // Gray or halftone image charge needs the next page's HALF cleanup.
  if (hasImages) pagesUntilFullRefresh = CrossPointSettings::REFRESH_COUNTDOWN_FORCE_FULL;
  if (!doGray) return;

  lastPageUsedGrayscale = true;
  // Marks are black-and-white only, as on a horizontal page.
  const auto drawPlanes = [&]() {
    renderVerticalPageBody(vpage, /*glyphsAlreadyWarm=*/true, grayImagesOnly, /*marks=*/false);
  };
  if (renderer.supportsStripGrayscale()) {
    const int gh = renderer.getDisplayHeight();
    const int gwBytes = renderer.getDisplayWidthBytes();
    auto scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * kGrayStripRows);
    if (!scratch) {
      LOG_ERR("ERS", "OOM: grayscale strip scratch (%d bytes); vertical page stays B/W", gwBytes * kGrayStripRows);
      return;
    }
    for (int plane = 0; plane < 2; plane++) {
      const bool lsb = plane == 0;
      renderer.setRenderMode(lsb ? GfxRenderer::GRAYSCALE_LSB : GfxRenderer::GRAYSCALE_MSB);
      for (int y = 0; y < gh; y += kGrayStripRows) {
        const int rows = std::min(kGrayStripRows, gh - y);
        renderer.beginStripTarget(scratch.get(), y, rows);
        renderer.clearScreen(0x00);
        drawPlanes();
        renderer.endStripTarget();
        renderer.writeGrayscalePlaneStrip(lsb, scratch.get(), y, rows);
      }
    }
    renderer.setRenderMode(GfxRenderer::BW);
    renderer.displayGrayBuffer();
    renderer.cleanupGrayscaleWithFrameBuffer();
    return;
  }
  if (!renderer.storeBwBuffer()) {
    LOG_ERR("ERS", "Vertical gray pass skipped: B/W page could not be parked");
    return;
  }
  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  drawPlanes();
  renderer.copyGrayscaleLsbBuffers();
  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  drawPlanes();
  renderer.copyGrayscaleMsbBuffers();
  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.restoreBwBuffer();
}

void EpubReaderActivity::earlyRenderVerticalPageThunk(void* ctx, const VerticalPage& page, const int pageIndex) {
  static_cast<EpubReaderActivity*>(ctx)->earlyRenderVerticalPage(page, pageIndex);
}

void EpubReaderActivity::earlyRenderVerticalPage(const VerticalPage& page, const int pageIndex) {
  // The build lends the framebuffer to its inflate while extracting the chapter; drawing then
  // would scribble on those bytes. Layout runs with the framebuffer back, which is when the
  // target page normally lands.
  if (!renderer.hasFrameBuffer()) return;
  const auto start = millis();
  if (verticalSection) verticalSection->currentPage = pageIndex;
  renderer.clearScreen();
  renderVerticalPageBody(page);
  renderStatusBar();
  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  earlyPageShown_ = true;
  // The build resumes the moment this returns and needs its headroom back.
  if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseCache();
  ImageBlock::releaseRenderCache();
  prewarmedVPage_ = -1;
  LOG_DBG("ERS", "Early render of vertical page %d in %lums", pageIndex, millis() - start);
}

void EpubReaderActivity::tickVerticalBuild() {
  if (!verticalSection || !verticalSection->isBuilding()) return;
  const uint32_t loansBefore = renderer.frameBufferLoanCount();
  const VerticalSection::BuildStep step = verticalSection->buildSomeMore(kVerticalSliceMs);
  // A slice that lent the framebuffer hands it back blank while the panel still shows the page.
  if (renderer.frameBufferLoanCount() != loansBefore) pageBufferStale = true;
  if (step == VerticalSection::BuildStep::InProgress) return;
  if (step == VerticalSection::BuildStep::Failed) {
    LOG_ERR("ERS", "Background vertical build failed");
    failedVerticalSpineIndex = currentSpineIndex;
    verticalSection.reset();
    requestUpdate();
    return;
  }
  // The page on screen is the truth now; render() redraws it against the final page count.
  verticalBuildFinished_ = true;
  requestUpdate();
}

void EpubReaderActivity::renderVertical(const int marginRight, const uint16_t viewportWidth,
                                        const uint16_t viewportHeight) {
  const auto showError = [this](const StrId message) {
    renderer.clearScreen();
    renderer.drawCenteredText(uiScaleSpec().titleFontId, 300, I18N.get(message), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    automaticPageTurnActive = false;
  };

  if (failedVerticalSpineIndex == currentSpineIndex) {
    // Already failed this session (usually a transient low heap): don't rebuild every frame.
    showError(StrId::STR_PAGE_LOAD_ERROR);
    return;
  }

  // Columns run to the very bottom of the text area, where a horizontal page ends on a line's
  // descent: keep a quarter line clear above the status bar.
  const int bottomGap = renderer.getLineHeight(SETTINGS.getReaderFontId()) / 4;
  const uint16_t columnHeight = static_cast<uint16_t>(std::max(1, static_cast<int>(viewportHeight) - bottomGap));
  const ReaderRenderSpec spec = verticalSpec(viewportWidth, columnHeight, marginRight);
  const bool finishedInLoop = verticalBuildFinished_ && verticalSection && !verticalSection->isBuilding();
  verticalBuildFinished_ = false;
  if (!verticalSection || finishedInLoop) {
    bool builtNow = finishedInLoop;
    if (!verticalSection) {
      LOG_DBG("ERS", "Loading vertical section, index: %d", currentSpineIndex);
      prewarmedVPage_ = -1;
      earlyPageShown_ = false;
      verticalSection = makeUniqueNoThrow<VerticalSection>(epub, currentSpineIndex, renderer);
      if (!verticalSection) {
        LOG_ERR("ERS", "OOM: VerticalSection");
        failedVerticalSpineIndex = currentSpineIndex;
        showError(StrId::STR_PAGE_LOAD_ERROR);
        return;
      }
      currentPageFootnotes.clear();  // vertical sections collect no footnotes
      if (!verticalSection->loadSectionFile(spec, /*retryDegraded=*/true)) {
        LOG_DBG("ERS", "Vertical cache not found, building...");
        drawIndexingPopup();
        // The build is the reader's heaviest step; the glyph caches are dead weight until it ends.
        if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseCache();
        // Show the reader's page the moment it is laid out, then finish the chapter between
        // frames (tickVerticalBuild). Positions that need the finished chapter -- a percent jump,
        // an anchor, a saved content offset -- wait for the whole build instead.
        const bool needsWholeChapter = pendingPercentJump || !pendingAnchor.empty() || pendingVisibleOffset != 0 ||
                                       (cachedVisibleTextOffset.has_value() && cachedSpineIndex == currentSpineIndex);
        if (!needsWholeChapter) {
          const int target = pendingPageJump.has_value() ? *pendingPageJump : std::max(0, nextPageNumber);
          verticalSection->setEarlyRenderHook(this, &EpubReaderActivity::earlyRenderVerticalPageThunk, target);
        }
        VerticalSection::BuildStep step = verticalSection->startBuild(spec) ? VerticalSection::BuildStep::InProgress
                                                                            : VerticalSection::BuildStep::Failed;
        while (step == VerticalSection::BuildStep::InProgress && !earlyPageShown_) {
          step = verticalSection->buildSomeMore(kVerticalSliceMs);
        }
        if (step == VerticalSection::BuildStep::InProgress) {
          LOG_DBG("ERS", "Vertical page shown; laying out the rest of chapter %d between frames", currentSpineIndex);
          pendingPageJump.reset();
          nextPageNumber = verticalSection->currentPage;
          return;
        }
        if (step == VerticalSection::BuildStep::Failed) {
          LOG_ERR("ERS", "Failed to build vertical section");
          failedVerticalSpineIndex = currentSpineIndex;
          verticalSection.reset();
          showError(StrId::STR_PAGE_LOAD_ERROR);
          return;
        }
        builtNow = true;
      }
    }

    if (builtNow && earlyPageShown_) {
      // The early page and any turns made during the build already name a page of THIS layout.
      pendingPageJump.reset();
    } else {
      int page = pendingPageJump.has_value() ? *pendingPageJump : nextPageNumber;
      pendingPageJump.reset();
      const int count = verticalSection->pageCount;
      if (!pendingAnchor.empty()) {
        if (const auto anchored = verticalSection->getPageForAnchor(pendingAnchor)) page = *anchored;
      } else if (pendingVisibleOffset != 0) {
        if (const auto p = verticalSection->getPageForVisibleTextOffset(pendingVisibleOffset, true)) page = *p;
      } else if (cachedVisibleTextOffset.has_value() && cachedSpineIndex == currentSpineIndex) {
        if (const auto p = verticalSection->getPageForVisibleTextOffset(*cachedVisibleTextOffset)) page = *p;
      } else if (pendingPercentJump && count > 0) {
        page = static_cast<int>(pendingSpineProgress * static_cast<float>(count));
      }
      verticalSection->currentPage = count > 0 ? std::clamp(page, 0, count - 1) : 0;
    }
    earlyPageShown_ = false;
    pendingAnchor.clear();
    pendingVisibleOffset = 0;
    pendingParagraphAnchor = UINT16_MAX;
    pendingPercentJump = false;
    pendingSeekSnippet_.clear();
    pendingSeekWord_ = false;
    cachedVisibleTextOffset.reset();
    cachedChapterTotalPageCount = 0;
  }

  // A turn past the pages built so far: lay out until it exists.
  while (verticalSection->isBuilding() && verticalSection->currentPage >= verticalSection->pageCount) {
    const auto step = verticalSection->buildSomeMore(kVerticalSliceMs);
    if (step == VerticalSection::BuildStep::Failed) {
      failedVerticalSpineIndex = currentSpineIndex;
      verticalSection.reset();
      showError(StrId::STR_PAGE_LOAD_ERROR);
      return;
    }
    if (step == VerticalSection::BuildStep::Done) break;
  }

  if (verticalSection->pageCount == 0) {
    showError(StrId::STR_EMPTY_CHAPTER);
    return;
  }
  verticalSection->currentPage = std::clamp(verticalSection->currentPage, 0, verticalSection->pageCount - 1);

  const bool stillBuilding = verticalSection->isBuilding();
  if (stillBuilding) verticalSection->notePageDrawnBetweenSlices();
  const VerticalPage* vpage = verticalSection->getPage();
  if (!vpage) {
    automaticPageTurnActive = false;
    if (verticalSection->lastReadHeapRefused()) {
      // Low heap, not corruption: the cache is valid. Reclaim font memory and retry.
      LOG_ERR("ERS", "Vertical page read refused on low heap; retrying");
      if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseCache();
      prewarmedVPage_ = -1;
      requestUpdate();
      return;
    }
    LOG_ERR("ERS", "Failed to read vertical page; clearing its cache");
    verticalSection->clearCache();
    verticalSection.reset();
    requestUpdate();
    return;
  }

  const auto start = millis();
  presentVerticalPage(*vpage, prewarmedVPage_ == verticalSection->currentPage);
  LOG_DBG("ERS", "Rendered vertical page %d in %lums", verticalSection->currentPage, millis() - start);
  if (stillBuilding) {
    // Drawn between two build slices: hand the build its headroom back.
    if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseCache();
    ImageBlock::releaseRenderCache();
    verticalSection->releasePageCache();
    prewarmedVPage_ = -1;
  }
  lastRenderCompleteMs = millis();
  openBookRecord.markPageRendered();
  readerSession.onRenderComplete(lastRenderCompleteMs, trustedtime::trustedNow(), getProgressBasisPoints());
  finishPageRender(viewportWidth, viewportHeight);
}

void EpubReaderActivity::paintVerticalPageThunk(void* ctx) {
  auto* self = static_cast<EpubReaderActivity*>(ctx);
  // The reader sits under the word-select screen with its section alive; nothing else reads the
  // section's page slot while that screen renders.
  if (const VerticalPage* page = self->verticalSection ? self->verticalSection->getPage() : nullptr) {
    self->renderVerticalPageBody(*page);
  }
}

void EpubReaderActivity::openVerticalWordSelect(const bool framebufferContainsPage, const int pointX,
                                                const int pointY, const bool highlight) {
  const auto mode = highlight ? DictionaryWordSelectActivity::Mode::HighlightRange
                              : DictionaryWordSelectActivity::Mode::Dictionary;
  int marginTop, marginLeft;
  readerTextOrigin(renderer, marginTop, marginLeft);
  const int fontId = SETTINGS.getReaderFontId();
  std::vector<WordSelectNavigator::WordInfo> words;
  std::vector<WordSelectNavigator::Row> rows;
  std::string textPool;
  std::string nextPageFirstWord;
  int cellPx = 0;
  int currentPage = 0;
  int pageCount = 0;
  auto wordScan = newWordScan();
  std::vector<CjkWordMerge::TokenGlyphs> scanCells;
  {
    // getPage() hands out the section's single page slot: copy everything out under the lock.
    RenderLock lock(*this);
    if (!verticalSection) return;
    currentPage = verticalSection->currentPage;
    pageCount = verticalSection->pageCount;
    const VerticalPage* vpage = verticalSection->getPage();
    if (!vpage) {
      lock.unlock();
      requestUpdate();
      return;
    }
    // Measured while the reader font is loaded: released, its reference glyph reports the line
    // height and every cell comes out too narrow.
    cellPx = verticalCellPx(renderer, fontId);
    if (!buildVerticalWordTokens(renderer, *vpage, fontId, marginLeft, marginTop, cellPx, words, rows, textPool,
                                 wordScan ? &scanCells : nullptr)) {
      return;
    }
    if (wordScan) {
      if (scanCells.size() == words.size()) {
        wordScan->initFromVerticalPage(*vpage);
      } else {
        wordScan.reset();
      }
    }
    // A word split by the page turn can still be looked up: the next page's opening characters,
    // one paragraph's worth. Fetched last, since it replaces the page in the slot.
    if (currentPage + 1 < pageCount) {
      if (const VerticalPage* next = verticalSection->getPage(currentPage + 1)) {
        const std::string text = PageTextExtractor::fromVerticalPage(*next);
        const size_t end = text.find('\n');
        nextPageFirstWord = text.substr(0, std::min<size_t>(end == std::string::npos ? text.size() : end, 32));
        nextPageFirstWord.resize(utf8SafeTruncateBuffer(nextPageFirstWord.c_str(), nextPageFirstWord.size()));
        if (wordScan && !next->glyphs.empty() && !nextPageFirstWord.empty()) {
          wordScan->appendLookupContext(nextPageFirstWord, next->glyphs.front().paragraphIndex);
        }
      }
    }
  }

  std::string chapterTitle;
  const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
  if (tocIndex != -1) chapterTitle = epub->getTocItem(tocIndex).title;
  // A highlight keeps the plain chapter title, as on a horizontal page; a lookup's card adds X/Y.
  const std::string quoteChapter = chapterTitle;
  if (pageCount > 0) {
    char pos[24];
    snprintf(pos, sizeof(pos), " %d/%d", currentPage + 1, pageCount);
    chapterTitle += pos;
  }
  // Same strip the horizontal path reports: the status bar band the page left below the text.
  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  const int reservedBottomHeight = std::max<int>(SETTINGS.getReaderScreenMargin(), statusBarHeight);
  const WordSelectNavigator::InitialMarker initialMarker = computeWordSelectMarker();
  pauseMarkerDwell();
  accumulateVisibleSegment();

  auto wordSelect = makeUniqueNoThrow<DictionaryWordSelectActivity>(
      renderer, mappedInput, nullptr, marginLeft, marginTop, epub->getCachePath(), nextPageFirstWord,
      framebufferContainsPage && !highlight, highlight ? 0 : reservedBottomHeight, mode, initialMarker,
      highlight ? std::string() : chapterTitle, pointX, pointY);
  if (!wordSelect) {
    LOG_ERR("EPUB", "OOM: DictionaryWordSelectActivity (vertical)");
    return;
  }
  wordSelect->setVerticalPage(std::move(words), std::move(rows), std::move(textPool), cellPx,
                              {this, &EpubReaderActivity::paintVerticalPageThunk});
  wordSelect->setMarksReloader(this, &EpubReaderActivity::reloadLookupMarksThunk);
  if (wordScan) {
    wordSelect->setWordScan(std::move(wordScan), wordScanCachePath(), static_cast<uint16_t>(currentSpineIndex),
                            static_cast<uint16_t>(currentPage), std::move(scanCells));
  }
  const uint16_t spine = static_cast<uint16_t>(currentSpineIndex);
  startActivityForResult(std::move(wordSelect), [this, spine, currentPage, pageCount, quoteChapter, cellPx,
                                                 marginLeft, marginTop](const ActivityResult& result) {
    ignoreBackUntilRelease = true;
    const auto* hr = result.isCancelled ? nullptr : std::get_if<HighlightRangeResult>(&result.data);
    if (!hr) {
      // A lookup enrolls a flashcard, which anchors the underline drawn on the repaint below.
      reloadLookupMarks();
      requestUpdate();
      return;
    }
    // The exact chapter offset of the quote's first character: it places the quote in either
    // layout (a horizontal page lands on it, this one marks from it even for one character).
    uint32_t startOffset = 0;
    {
      RenderLock lock(*this);
      if (const VerticalPage* vpage = verticalSection ? verticalSection->getPage() : nullptr) {
        VerticalPageMarks::offsetOfToken(renderer, *vpage, SETTINGS.getReaderFontId(), marginLeft, marginTop, cellPx,
                                         static_cast<uint16_t>(std::max(0, hr->startWordIndex)), startOffset);
      }
    }
    const float progress = pageCount > 0 ? static_cast<float>(currentPage) / static_cast<float>(pageCount) : 0.0f;
    SdDebugLog::log("PGM", "save vertical spine=%u page=%d/%d off=%u sw=%d ew=%d \"%.63s\"", spine, currentPage + 1,
                    pageCount, static_cast<unsigned>(startOffset), hr->startWordIndex, hr->endWordIndex,
                    hr->previewText.c_str());
    const auto addRes = BOOKMARKS.addQuote(spine, progress, static_cast<uint16_t>(std::max(0, hr->startWordIndex)),
                                           static_cast<uint16_t>(std::max(0, hr->endWordIndex)), pageCount,
                                           quoteChapter.empty() ? nullptr : quoteChapter.c_str(),
                                           hr->previewText.c_str(), currentPage, startOffset);
    if (addRes == BookmarkStore::AddResult::LimitReached) {
      RenderLock lock(*this);
      GUI.drawPopup(renderer, tr(STR_MARK_LIMIT));
      activityManager.holdForReading(900);
    }
    requestUpdate();
  });
}
