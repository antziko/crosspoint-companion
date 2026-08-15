#pragma once

#include <EpdFontFamily.h>
#include <HalDisplay.h>

namespace BidiUtils {
// Paragraph base direction for the Unicode BiDi algorithm (UAX#9).
// AUTO: scan text for first strong directional character (P2/P3 rules)
// LTR:  force left-to-right paragraph embedding level
// RTL:  force right-to-left paragraph embedding level
enum class BidiBaseDir : signed char { AUTO = -1, LTR = 0, RTL = 1 };
}  // namespace BidiUtils

class FontCacheManager;
class SdCardFont;

#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "Bitmap.h"

// Color representation: uint8_t mapped to 4x4 Bayer matrix dithering levels
// 0 = transparent, 1-16 = gray levels (white to black)
enum Color : uint8_t { Clear = 0x00, White = 0x01, LightGray = 0x05, DarkGray = 0x0A, Black = 0x10 };

class GfxRenderer {
 public:
  enum RenderMode { BW, GRAYSCALE_LSB, GRAYSCALE_MSB };

  // Logical screen orientation from the perspective of callers
  enum Orientation {
    Portrait,                  // 480x800 logical coordinates (current default)
    LandscapeClockwise,        // 800x480 logical coordinates, rotated 180° (swap top/bottom)
    PortraitInverted,          // 480x800 logical coordinates, inverted
    LandscapeCounterClockwise  // 800x480 logical coordinates, native panel orientation
  };

 private:
  static constexpr size_t BW_BUFFER_CHUNK_SIZE = 8000;  // 8KB chunks to allow for non-contiguous memory

  // Ink extent of `text` from an SD font's metrics table alone, no glyph loads. Returns false
  // (writing nothing) when the table cannot answer exactly, so getTextWidth falls back to the
  // glyph path. See the definition for why it is all-or-nothing per string.
  bool getSdInkWidth(int fontId, const char* text, EpdFontFamily::Style style, int* outWidth) const;

  HalDisplay& display;
  RenderMode renderMode;
  Orientation orientation;
  bool fadingFix;
  mutable bool forceCleanRefreshOnce_ = false;  // one-shot HALF_REFRESH override (see forceCleanRefreshNextPaint)
  uint8_t* frameBuffer = nullptr;
  uint16_t panelWidth = HalDisplay::DISPLAY_WIDTH;
  uint16_t panelHeight = HalDisplay::DISPLAY_HEIGHT;
  uint16_t panelWidthBytes = HalDisplay::DISPLAY_WIDTH_BYTES;
  uint32_t frameBufferSize = HalDisplay::BUFFER_SIZE;
  // Display > Image Dither, raw setting value (IMG_DITHER_* in OrderedDither.h).
  uint8_t imageDitherMode_ = IMG_DITHER_BLUE_NOISE;
  // True when images should render as a 1-bit halftone instead of 4-level gray
  // (always on X3; on X4 when text AA is off, for true black + no two-stage flash).
  bool oneBitImages_ = false;
  // When false, glyphs contribute nothing to the grayscale planes, so anti-aliased
  // text edges keep the BW pass's solid black ("Sharp" text). Images are unaffected.
  bool textAntiAlias_ = true;
  std::vector<uint8_t*> bwBufferChunks;
  std::map<int, EpdFontFamily> fontMap;
  // Mutable because ensureSdCardFontReady() is const (called from layout code
  // that holds a const GfxRenderer&) but triggers SD card reads and heap
  // allocation inside the SdCardFont objects. Same pragmatic compromise as
  // fontCacheManager_ below.
  mutable std::map<int, SdCardFont*> sdCardFonts_;

  // Mutable because drawText() is const but needs to delegate scan-mode
  // recording to the (non-const) FontCacheManager. Same pragmatic compromise
  // as before, concentrated in a single pointer instead of four fields.
  mutable FontCacheManager* fontCacheManager_ = nullptr;

  // Tiled grayscale strip target. When active, drawPixel()/clearScreen()
  // operate on a caller-owned scratch holding one horizontal band of physical
  // rows [_stripY0, _stripY0 + _stripRows) (panelWidthBytes wide) instead of
  // the shared framebuffer, clipping pixels outside the band. Lets grayscale
  // planes render band-by-band straight to the controller without destroying
  // the BW framebuffer (no storeBwBuffer). Mutable because the render path is
  // const. See beginStripTarget()/endStripTarget().
  mutable uint8_t* _stripBuf = nullptr;
  mutable int _stripY0 = 0;
  mutable int _stripRows = 0;
  mutable bool _stripActive = false;

  // CJK UI font fallback map: primary (built-in, Latin-only) UI font id -> a
  // size-matched SD-card font id that carries CJK glyphs. When a string drawn
  // or measured with a mapped primary font contains a CJK codepoint the primary
  // cannot render, the whole string is routed to the mapped fallback so it
  // appears at the same point size as the surrounding UI text. Populated by the
  // app-level SD font setup when an SD family is loaded. See resolveTextFontId().
  std::map<int, int> fallbackFontMap_;

  // If `text` contains a CJK codepoint that `fontId` cannot render and `fontId`
  // has a registered fallback, returns the fallback id; otherwise returns
  // fontId unchanged. The whole string is routed as a unit so each draw/measure
  // call stays single-font (consistent bit depth, metrics, wrapping).
  int resolveTextFontId(int fontId, const char* text, EpdFontFamily::Style style) const;

  void renderChar(const EpdFontFamily& fontFamily, uint32_t cp, int* x, int* y, bool pixelState,
                  EpdFontFamily::Style style) const;
  void freeBwBufferChunks();
  template <Color color>
  void drawPixelDither(int x, int y) const;
  template <Color color>
  void fillArc(int maxRadius, int cx, int cy, int xDir, int yDir) const;
  // Byte-aligned, orientation-specialized rectangle fill. Rotates the rect's
  // two opposing corners into physical-framebuffer space once, then walks each
  // physical row with head-mask / middle memset / tail-mask byte writes — no
  // per-pixel rotation, no per-pixel RMW.
  template <Color color>
  void fillRectImpl(int x, int y, int width, int height) const;

 public:
  explicit GfxRenderer(HalDisplay& halDisplay)
      : display(halDisplay), renderMode(BW), orientation(Portrait), fadingFix(false) {}
  ~GfxRenderer() { freeBwBufferChunks(); }

  // Setup
  void begin();  // must be called right after display.begin()
  void insertFont(int fontId, EpdFontFamily font);
  // Clears both the flash-font map and any SD-font registration for fontId.
  // Coupled to avoid dangling SdCardFont* in sdCardFonts_ when callers free
  // the underlying SdCardFont and forget the SD-side unregister.
  void removeFont(int fontId) {
    fontMap.erase(fontId);
    sdCardFonts_.erase(fontId);
  }
  void setFontCacheManager(FontCacheManager* m) { fontCacheManager_ = m; }
  FontCacheManager* getFontCacheManager() const { return fontCacheManager_; }
  bool isFontCacheScanning() const;
  const std::map<int, EpdFontFamily>& getFontMap() const { return fontMap; }
  void registerSdCardFont(int fontId, SdCardFont* font) { sdCardFonts_[fontId] = font; }
  void unregisterSdCardFont(int fontId) { removeFont(fontId); }
  void clearSdCardFonts() { sdCardFonts_.clear(); }
  const std::map<int, SdCardFont*>& getSdCardFonts() const { return sdCardFonts_; }
  bool isSdCardFont(int fontId) const { return sdCardFonts_.count(fontId) > 0; }
  // Register/clear size-matched CJK UI fallbacks (see fallbackFontMap_).
  // setFallbackFont maps a primary UI font id to an SD font id of the same size.
  void setFallbackFont(int primaryFontId, int fallbackFontId) { fallbackFontMap_[primaryFontId] = fallbackFontId; }
  void clearFallbackFonts() { fallbackFontMap_.clear(); }
  // True when fontId is the TARGET of any fallback (i.e. some UI font resolves CJK through it).
  // Callers freeing individual SD fonts use this to leave the UI's fallbacks alone.
  bool isFallbackTarget(int fontId) const {
    for (const auto& [primary, fallback] : fallbackFontMap_) {
      if (fallback == fontId) return true;
    }
    return false;
  }
  // Ensure SD card font glyph data is loaded for the given text. Called from layout code
  // (which holds a const GfxRenderer&) before measuring word widths. Safe to call on non-SD fonts (no-op).
  // styleMask: bitmask of styles to prepare (bit 0=regular, 1=bold, 2=italic, 3=bold-italic).
  void ensureSdCardFontReady(int fontId, const char* utf8Text, uint8_t styleMask = 0x0F) const;
  void ensureSdCardFontReady(int fontId, const std::deque<std::string>& words, bool includeHyphen,
                             uint8_t styleMask = 0x0F) const;

  // Orientation control (affects logical width/height and coordinate transforms)
  void setOrientation(const Orientation o) { orientation = o; }
  Orientation getOrientation() const { return orientation; }

  // Fading fix control
  void setFadingFix(const bool enabled) { fadingFix = enabled; }

  // Screen ops
  int getScreenWidth() const;
  int getScreenHeight() const;
  void tapToLogical(float nx, float ny, int& outX, int& outY) const;
  void displayBuffer(HalDisplay::RefreshMode refreshMode = HalDisplay::FAST_REFRESH) const;
  // Push only the logical rectangle (lx,ly,lw,lh) to the panel using a windowed
  // sub-rectangle refresh. The logical rect is mapped to byte-aligned native panel
  // coordinates via the current orientation transform. Falls back to a full
  // FAST_REFRESH if the mapping produces an invalid rect.
  // EXPERIMENTAL — safe for single isolated refreshes; see HalDisplay::displayWindow.
  void displayWindowRegion(int lx, int ly, int lw, int lh) const;
  // Force the next displayBuffer() to use HALF_REFRESH (state-collapsed, ignores stale
  // old-RAM residue) regardless of the mode requested, then revert to normal. Used to
  // clear e-ink ghosting on the first paint after a silent reboot: the seamless boot skips
  // the panel clear, so a fast paint would ghost the pre-reboot frame (e.g. the KOReader
  // sync result screen) under the new content. HALF avoids FULL's hard black/white flash.
  void forceCleanRefreshNextPaint() const { forceCleanRefreshOnce_ = true; }
  // Read a rectangular region of the 1-bpp framebuffer into 'dst'. Inputs are
  // SCREEN coordinates (the same coordinate system fillRect / drawText use).
  // Internally the rectangle is rotated into panel-memory coordinates and
  // snapped outward to byte boundaries, so the bytes actually read can cover
  // up to one extra byte per row vs. the requested screen width. Output is
  // packed MSB-first with (alignedMemoryWidth / 8) bytes per row.
  // Returns the bytes written, or 0 if dstCapacity is insufficient or the
  // rectangle is empty / fully out of bounds.
  //
  // Symmetric with writeFramebufferRegion: passing the SAME screen rectangle
  // to writeFramebufferRegion will restore exactly the pixels this read.
  size_t readFramebufferRegion(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t* dst, size_t dstCapacity) const;

  // Restore a rectangular region of the framebuffer previously captured by
  // readFramebufferRegion using the SAME SCREEN rectangle. Inputs are SCREEN
  // coordinates; src must hold exactly the bytes returned by
  // readFramebufferRegion for the same screen rect (same row layout, same
  // total length). No-op if the rectangle is empty or fully out of bounds.
  void writeFramebufferRegion(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t* src);

  // Non-blocking refresh: starts the waveform and returns so CPU work (e.g.
  // grayscale strip rendering) can overlap the panel's refresh time. The
  // framebuffer must stay untouched until waitRefreshComplete(). Falls back to
  // a blocking refresh when fadingFix is enabled or the panel lacks deferral
  // support. See HalDisplay::displayBufferAsync for the baseline contract.
  void displayBufferAsync(HalDisplay::RefreshMode refreshMode = HalDisplay::FAST_REFRESH) const;
  void waitRefreshComplete() const;
  // True when displayBufferAsync() genuinely overlaps: panel defers and
  // fadingFix isn't forcing the blocking path. Callers can skip overlap
  // scaffolding (e.g. whole-plane grayscale buffers) when false.
  bool supportsAsyncRefresh() const;
  void invertScreen() const;
  void clearScreen(uint8_t color = 0xFF) const;
  void getOrientedViewableTRBL(int* outTop, int* outRight, int* outBottom, int* outLeft) const;

  // Tiled grayscale strip target. While active, drawPixel() and clearScreen()
  // operate on `scratch` (panelWidthBytes * stripRows bytes, holding physical
  // rows [stripY0, stripY0 + stripRows)) instead of the framebuffer; pixels
  // whose physical row falls outside the band are clipped. The clip is applied
  // after the orientation rotate, so it is orientation-agnostic. Used to render
  // grayscale planes band-by-band without a full second buffer.
  void beginStripTarget(uint8_t* scratch, int stripY0, int stripRows) const;
  void endStripTarget() const;

  // Band culling for tiled grayscale. Takes a glyph bounding box in logical
  // screen coords and returns false only when a strip is active AND the box's
  // physical y-extent lies entirely outside the active band, letting callers
  // skip an expensive bitmap decode. Returns true when no strip is active.
  // Corners are rotated to physical, so it is orientation-aware.
  bool glyphIntersectsStrip(int x0, int y0, int x1, int y1) const;

  // Active pixel-write target for raw writers (DirectPixelWriter) that bypass
  // drawPixel for speed. When a strip target is active these return the band
  // scratch plus its physical-row origin and extent; otherwise the full
  // framebuffer ([0, panelHeight)). Writers subtract the origin and clip to the
  // extent, so they honor tiled-grayscale banding without per-pixel method calls.
  uint8_t* getWriteTarget() const { return _stripActive ? _stripBuf : frameBuffer; }
  int getWriteOriginY() const { return _stripActive ? _stripY0 : 0; }
  int getWriteRows() const { return _stripActive ? _stripRows : panelHeight; }

  // Drawing
  void drawPixel(int x, int y, bool state = true) const;
  void drawLine(int x1, int y1, int x2, int y2, bool state = true) const;
  void drawLine(int x1, int y1, int x2, int y2, int lineWidth, bool state) const;
  void drawArc(int maxRadius, int cx, int cy, int xDir, int yDir, int lineWidth, bool state) const;
  void drawRect(int x, int y, int width, int height, bool state = true) const;
  void drawRect(int x, int y, int width, int height, int lineWidth, bool state) const;
  void drawRoundedRect(int x, int y, int width, int height, int lineWidth, int cornerRadius, bool state) const;
  void drawRoundedRect(int x, int y, int width, int height, int lineWidth, int cornerRadius, bool roundTopLeft,
                       bool roundTopRight, bool roundBottomLeft, bool roundBottomRight, bool state) const;
  void maskRoundedRectOutsideCorners(int x, int y, int width, int height, int radius, Color color = Color::White) const;
  void fillRect(int x, int y, int width, int height, bool state = true) const;
  // Dim an already-drawn region to a grey checkerboard stipple: clears every
  // pixel on the (x+y) even diagonal, thinning glyphs/ink so the region recedes
  // (used for dimmed list rows and chapter/status footers). Surrounding pixels
  // should already be white for the stipple to read as grey.
  void dimRegionCheckerboard(int x, int y, int width, int height) const;
  // Fast clear-to-white over a rectangle. Equivalent in effect to
  // fillRect(x, y, w, h, false) but uses byte-aligned memset for the
  // middle of each panel-memory row, with bit-mask OR only at the byte
  // edges. Roughly 10x faster than fillRect for wide regions because it
  // avoids the per-pixel rotateCoordinates/bounds-check/bit-RMW path.
  // Handles all four orientations via rotateCoordinates on the rect's
  // two opposite corners. Clamps to panel bounds; out-of-bounds rects
  // are silently dropped (unlike drawPixel, which logs each
  // out-of-bounds pixel — callers may legitimately pass slightly
  // margin-overlapping rects).
  void clearRect(int x, int y, int width, int height) const;
  void fillRectDither(int x, int y, int width, int height, Color color) const;
  void fillRoundedRect(int x, int y, int width, int height, int cornerRadius, Color color) const;
  void fillRoundedRect(int x, int y, int width, int height, int cornerRadius, bool roundTopLeft, bool roundTopRight,
                       bool roundBottomLeft, bool roundBottomRight, Color color) const;
  void drawImage(const uint8_t bitmap[], int x, int y, int width, int height) const;
  void drawIcon(const uint8_t bitmap[], int x, int y, int size) const;
  void drawBitmap(const Bitmap& bitmap, int x, int y, int maxWidth, int maxHeight, float cropX = 0,
                  float cropY = 0) const;
  void drawBitmap1Bit(const Bitmap& bitmap, int x, int y, int maxWidth, int maxHeight) const;
  // Counter-invert content images in the logical framebuffer so output-level night mode
  // leaves their original polarity unchanged (photos stay photos, not negatives).
  // No-op unless the display is actually inverted.
  void preserveImagePolarity(int x, int y, int width, int height) const;
  void fillPolygon(const int* xPoints, const int* yPoints, int numPoints, bool state = true) const;

  // Text
  int getTextWidth(int fontId, const char* text, EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                   BidiUtils::BidiBaseDir baseDir = BidiUtils::BidiBaseDir::AUTO) const;
  // Pen advance width (cursor end position), NOT the ink bounding box getTextWidth
  // returns. Use for caret/sub-span placement under centered/left-drawn text (e.g.
  // underlining a word). Operates on the raw bytes (no BiDi reshaping) so a byte
  // offset into `text` maps to a stable advance for LTR runs.
  int getTextAdvanceWidth(int fontId, const char* text, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  // Ink extents (relative to pen origin 0): *minX = first glyph left side bearing,
  // *maxX = rightmost inked pixel. Place a sub-span at penOrigin + minX, width
  // maxX - minX. No BiDi reshaping (LTR byte offsets).
  void getTextInkBounds(int fontId, const char* text, int* minX, int* maxX,
                        EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  void drawCenteredText(int fontId, int y, const char* text, bool black = true,
                        EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                        BidiUtils::BidiBaseDir baseDir = BidiUtils::BidiBaseDir::AUTO) const;
  void drawText(int fontId, int x, int y, const char* text, bool black = true,
                EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                BidiUtils::BidiBaseDir baseDir = BidiUtils::BidiBaseDir::AUTO) const;
  /// Draw ONE codepoint with every glyph pixel replicated `scale` times in both axes, from
  /// `yTop` (the top of the scaled text cell, matching drawText's y convention). Returns the
  /// scaled pen advance, or 0 when the glyph is unavailable.
  ///
  /// A codepoint rather than a string: the caller is the dictionary gloss box drawing a single
  /// Han character, so kerning, ligatures, BiDi and combining marks have nothing to do, and
  /// keeping them out keeps this off drawText's hot path entirely.
  ///
  /// Ink only — a replicated pixel block is either drawn or not, so the 2-bit grayscale levels
  /// collapse the same way BW mode already collapses them in renderCharImpl. Scaling up an
  /// anti-aliased edge would smear it into a 3x3 block of solid ink; thresholding is what keeps
  /// the enlarged strokes crisp.
  int drawGlyphScaled(int fontId, uint32_t cp, int x, int yTop, int scale, bool black = true,
                      EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getSpaceWidth(int fontId, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  /// Returns the total inter-word advance: fp4::toPixel(spaceAdvance + kern(leftCp,' ') + kern(' ',rightCp)).
  /// Using a single snap avoids the +/-1 px rounding error that arises when space advance and kern are
  /// snapped separately and then added as integers.
  int getSpaceAdvance(int fontId, uint32_t leftCp, uint32_t rightCp, EpdFontFamily::Style style) const;
  /// Returns the kerning adjustment between two adjacent codepoints.
  int getKerning(int fontId, uint32_t leftCp, uint32_t rightCp, EpdFontFamily::Style style) const;
  int getTextAdvanceX(int fontId, const char* text, EpdFontFamily::Style style) const;
  int getFontAscenderSize(int fontId) const;
  int getLineHeight(int fontId) const;
  int getLineHeight(int fontId, float compression) const;
  std::string truncatedText(int fontId, const char* text, int maxWidth,
                            EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  /// Word-wrap \p text into at most \p maxLines lines, each no wider than
  /// \p maxWidth pixels. Overflowing words and excess lines are UTF-8-safely
  /// truncated with an ellipsis (U+2026).
  std::vector<std::string> wrappedText(int fontId, const char* text, int maxWidth, int maxLines,
                                       EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;

  // Helper for drawing rotated text (90 degrees clockwise, for side buttons)
  void drawTextRotated90CW(int fontId, int x, int y, const char* text, bool black = true,
                           EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getTextHeight(int fontId) const;

  // Grayscale functions
  void setRenderMode(const RenderMode mode) { this->renderMode = mode; }
  RenderMode getRenderMode() const { return renderMode; }
  // Grayscale preconditioning settle pass (no-op on X4). The rect overload
  // takes the gray region in LOGICAL screen coordinates and rotates it to the
  // panel; the no-arg overload settles the full frame. Call after the BW base
  // frame is displayed and before the grayscale planes are written.
  void preconditionGrayscale() const;
  void preconditionGrayscale(int x, int y, int w, int h) const;
  // Display the framebuffer as the base frame for a grayscale overlay that
  // follows (X3: OEM differential base waveform; others: plain display with
  // `fallback`).
  void displayGrayscaleBase(HalDisplay::RefreshMode fallback = HalDisplay::HALF_REFRESH) const;
  void copyGrayscaleLsbBuffers() const;
  void copyGrayscaleMsbBuffers() const;
  void displayGrayBuffer() const;

  // Tiled grayscale (X4): stream one band of a plane straight to controller RAM
  // from `scratch` (panelWidthBytes * numRows, physical rows [yStart, yStart+
  // numRows)), bypassing the framebuffer. supportsStripGrayscale() gates use.
  void writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t* scratch, int yStart, int numRows) const;
  bool supportsStripGrayscale() const;

  // True when running on the X3 (UC81xx) panel. Image rendering uses this to
  // choose 1-bit halftone over the X3 4-level grayscale waveform path.
  bool isX3() const { return display.isX3Mode(); }

  // Image dither algorithm (Display > Image Dither). Set from src (which owns
  // CrossPointSettings) with the raw IMG_DITHER_* value; read by ImageBlock and
  // the Bitmap path. Bridges the lib/src boundary so lib code never includes
  // CrossPointSettings.
  void setImageDitherMode(uint8_t mode) { imageDitherMode_ = mode; }
  uint8_t imageDitherMode() const { return imageDitherMode_; }
  // EPUB convenience: blue-noise vs Bayer for the stateless ordered path.
  // Error-diffusion isn't possible on JPEG block decode, so it maps to blue
  // noise (the best ordered field) for EPUB images.
  bool imageDitherBlueNoise() const { return imageDitherMode_ != IMG_DITHER_BAYER; }
  // 1-bit halftone vs 4-level grayscale for images. Set by the reader from
  // (isX3 || text AA off); read by ImageBlock to pick the render path + cache.
  void setOneBitImages(bool v) { oneBitImages_ = v; }
  bool oneBitImages() const { return oneBitImages_; }
  void setTextAntiAlias(bool v) { textAntiAlias_ = v; }
  bool textAntiAlias() const { return textAntiAlias_; }
  bool storeBwBuffer();    // Returns true if buffer was stored successfully
  void restoreBwBuffer();  // Restore and free the stored buffer
  void cleanupGrayscaleWithFrameBuffer() const;

  // Font helpers
  const uint8_t* getGlyphBitmap(const EpdFontData* fontData, const EpdGlyph* glyph) const;

  // Lend the 48 KB framebuffer's bytes to a memory-hungry phase (chapter
  // builds) WITHOUT freeing the allocation, so it never moves and repeated
  // loans cannot fragment the heap. Between release and restore NOTHING may
  // draw or display — the panel keeps showing its last refreshed image. The
  // lent bytes are published via buildscratch::claim() for consumers like
  // InflateStream. restore returns the buffer white, so the caller must
  // redraw the full screen; it cannot fail (no allocation involved).
  void releaseFrameBufferForBuild();
  bool restoreFrameBufferAfterBuild();
  bool hasFrameBuffer() const { return frameBuffer != nullptr; }

  // RAII form of the loan above, for blocking build regions with early-return
  // error paths: restores on scope exit (or explicitly via end()). Display the
  // popup/screen the panel should hold BEFORE constructing one. Constructing
  // while the framebuffer is already lent yields an inert loan (nesting-safe).
  class FrameBufferLoan {
   public:
    explicit FrameBufferLoan(GfxRenderer& renderer);
    ~FrameBufferLoan() { end(); }
    void end();
    FrameBufferLoan(const FrameBufferLoan&) = delete;
    FrameBufferLoan& operator=(const FrameBufferLoan&) = delete;

   private:
    GfxRenderer& renderer_;
    bool active_ = false;
  };

  // Low level functions
  uint8_t* getFrameBuffer() const;
  size_t getBufferSize() const;
  uint16_t getDisplayWidth() const { return panelWidth; }
  uint16_t getDisplayHeight() const { return panelHeight; }
  uint16_t getDisplayWidthBytes() const { return panelWidthBytes; }

  // Region cache: take a logical (orientation-aware) rect, hit the framebuffer
  // bytes that the rect can have touched, and pump them in or out of a caller-
  // supplied buffer. Used by HomeActivity to snapshot just the cover tile
  // (~16 KB in Portrait) instead of cloning the entire 48 KB framebuffer.
  //
  // getRegionByteSize: required buffer length for the rect at current orientation.
  // copyRegionToBuffer / copyBufferToRegion: false if `bufSize` is smaller than that.
  size_t getRegionByteSize(int logicalX, int logicalY, int logicalW, int logicalH) const;
  bool copyRegionToBuffer(int logicalX, int logicalY, int logicalW, int logicalH, uint8_t* buf, size_t bufSize) const;
  bool copyBufferToRegion(int logicalX, int logicalY, int logicalW, int logicalH, const uint8_t* buf,
                          size_t bufSize) const;
};
