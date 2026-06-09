#include "JpegToFramebufferConverter.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <JPEGDEC.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdlib>
#include <memory>
#include <new>

#include "DirectPixelWriter.h"
#include "DitherUtils.h"
#include "PixelCache.h"

namespace {

// Context struct passed through JPEGDEC callbacks to avoid global mutable state.
// The draw callback receives this via pDraw->pUser (set by setUserPointer()).
// The file I/O callbacks receive the HalFile* via pFile->fHandle (set by jpegOpen()).
struct JpegContext {
  GfxRenderer* renderer{nullptr};
  const RenderConfig* config{nullptr};
  int screenWidth{0};
  int screenHeight{0};

  // Source dimensions after JPEGDEC's built-in scaling
  int scaledSrcWidth{0};
  int scaledSrcHeight{0};

  // Final output dimensions
  int dstWidth{0};
  int dstHeight{0};

  // Fine scale in 16.16 fixed-point (ESP32-C3 has no FPU).
  // X and Y axes use separate scale factors: the aspect ratio of the output (dstWidth/dstHeight)
  // may differ from the source (srcWidth/srcHeight) due to integer rounding of displayHeight.
  // Using a single (X-based) scale for both axes causes the wrong srcRow to be skipped
  // during nearest-neighbor downscaling, potentially losing critical image content.
  int32_t fineScaleFPX{1 << 16};  // X: src -> dst column mapping
  int32_t invScaleFPX{1 << 16};   // X: dst -> src column mapping
  int32_t fineScaleFPY{1 << 16};  // Y: src -> dst row mapping
  int32_t invScaleFPY{1 << 16};   // Y: dst -> src row mapping

  PixelCache cache;
  // Non-null when the image is too big for a full RAM buffer: rows stream to SD
  // during decode instead. Mutually exclusive with `cache` (full-buffer path).
  StreamingPixelCache* stream{nullptr};
  bool caching{false};

  // Per-image tone curve selection (see X4Tone in OrderedDither.h). Determined by
  // a cheap 1/8-scale luminance probe before the real decode. Defaults to Brighten
  // (= mild lift, prior behaviour for dark images) so a probe failure is safe.
  X4Tone tone{X4Tone::Brighten};

  // Per-image sharp-upscale verdict (text images). When set, the upscale path
  // uses nearest-neighbor instead of bilinear so thin high-contrast strokes
  // (e.g. code/terminal screenshots) stay crisp. Set by the same probe; defaults
  // false (= bilinear, prior behaviour).
  bool sharpUpscale{false};
};

// Accumulator for the measure pass — counts dark, bright, and mid-grey pixels
// over a coarse decode (dark/bright/text-band fractions; see OrderedDither.h).
struct JpegLumProbe {
  uint32_t dark{0};
  uint32_t bright{0};
  uint32_t mid{0};   // pixels in [X4_TEXT_PIXEL_CUTOFF, X4_BRIGHT_PIXEL_CUTOFF)
  uint32_t count{0};
};

// Measure-pass draw callback: counts dark (<=X4_DARK_PIXEL_CUTOFF), bright
// (>=X4_BRIGHT_PIXEL_CUTOFF), and mid-grey ([X4_TEXT_PIXEL_CUTOFF,
// X4_BRIGHT_PIXEL_CUTOFF)) pixels. No scaling/dithering. pUser is JpegLumProbe*.
int jpegMeasureCallback(JPEGDRAW* pDraw) {
  auto* probe = reinterpret_cast<JpegLumProbe*>(pDraw->pUser);
  if (!probe) return 0;
  const uint8_t* pixels = reinterpret_cast<uint8_t*>(pDraw->pPixels);
  const int stride = pDraw->iWidth;
  const int validW = pDraw->iWidthUsed;
  const int blockH = pDraw->iHeight;
  if (stride <= 0 || blockH <= 0 || validW <= 0) return 1;
  uint32_t d = 0;
  uint32_t b = 0;
  uint32_t m = 0;
  uint32_t c = 0;
  for (int row = 0; row < blockH; row++) {
    const uint8_t* p = &pixels[row * stride];
    for (int x = 0; x < validW; x++) {
      if (p[x] <= X4_DARK_PIXEL_CUTOFF) d++;
      if (p[x] >= X4_BRIGHT_PIXEL_CUTOFF) b++;
      if (p[x] >= X4_TEXT_PIXEL_CUTOFF && p[x] < X4_BRIGHT_PIXEL_CUTOFF) m++;
      c++;
    }
  }
  probe->dark += d;
  probe->bright += b;
  probe->mid += m;
  probe->count += c;
  return 1;
}

// File I/O callbacks use pFile->fHandle to access the HalFile*,
// avoiding the need for global file state.
void* jpegOpen(const char* filename, int32_t* size) {
  HalFile* f = new HalFile();
  if (!Storage.openFileForRead("JPG", std::string(filename), *f)) {
    delete f;
    return nullptr;
  }
  *size = f->size();
  return f;
}

void jpegClose(void* handle) {
  HalFile* f = reinterpret_cast<HalFile*>(handle);
  if (f) {
    f->close();
    delete f;
  }
}

// JPEGDEC tracks file position via pFile->iPos internally (e.g. JPEGGetMoreData
// checks iPos < iSize to decide whether more data is available). The callbacks
// MUST maintain iPos to match the actual file position, otherwise progressive
// JPEGs with large headers fail during parsing.
int32_t jpegRead(JPEGFILE* pFile, uint8_t* pBuf, int32_t len) {
  HalFile* f = reinterpret_cast<HalFile*>(pFile->fHandle);
  if (!f) return 0;
  int32_t bytesRead = f->read(pBuf, len);
  if (bytesRead < 0) return 0;
  pFile->iPos += bytesRead;
  return bytesRead;
}

int32_t jpegSeek(JPEGFILE* pFile, int32_t pos) {
  HalFile* f = reinterpret_cast<HalFile*>(pFile->fHandle);
  if (!f) return -1;
  if (!f->seek(pos)) return -1;
  pFile->iPos = pos;
  return pos;
}

// JPEGDEC object is ~17 KB due to internal decode buffers.
// Heap-allocate on demand so memory is only used during active decode.
constexpr size_t JPEG_DECODER_APPROX_SIZE = 20 * 1024;
constexpr size_t MIN_FREE_HEAP_FOR_JPEG = JPEG_DECODER_APPROX_SIZE + 16 * 1024;

// Choose JPEGDEC's built-in scale factor for coarse downscaling.
// Returns the scale denominator (1, 2, 4, or 8) and sets jpegScaleOption.
int chooseJpegScale(float targetScale, int& jpegScaleOption) {
  if (targetScale <= 0.125f) {
    jpegScaleOption = JPEG_SCALE_EIGHTH;
    return 8;
  }
  if (targetScale <= 0.25f) {
    jpegScaleOption = JPEG_SCALE_QUARTER;
    return 4;
  }
  if (targetScale <= 0.5f) {
    jpegScaleOption = JPEG_SCALE_HALF;
    return 2;
  }
  jpegScaleOption = 0;
  return 1;
}

// Fixed-point 16.16 arithmetic avoids software float emulation on ESP32-C3 (no FPU).
constexpr int FP_SHIFT = 16;
constexpr int32_t FP_ONE = 1 << FP_SHIFT;
constexpr int32_t FP_MASK = FP_ONE - 1;

int jpegDrawCallback(JPEGDRAW* pDraw) {
  JpegContext* ctx = reinterpret_cast<JpegContext*>(pDraw->pUser);
  if (!ctx || !ctx->config || !ctx->renderer) return 0;

  // In EIGHT_BIT_GRAYSCALE mode, pPixels contains 8-bit grayscale values
  // Buffer is densely packed: stride = pDraw->iWidth, valid columns = pDraw->iWidthUsed
  uint8_t* pixels = reinterpret_cast<uint8_t*>(pDraw->pPixels);
  const int stride = pDraw->iWidth;
  const int validW = pDraw->iWidthUsed;
  const int blockH = pDraw->iHeight;

  if (stride <= 0 || blockH <= 0 || validW <= 0) return 1;

  const bool useDithering = ctx->config->useDithering;
  const bool oneBit = ctx->config->oneBitDither;
  const bool blueNoise = ctx->config->ditherBlueNoise;
  const X4Tone tone = ctx->tone;
  const bool sharpUpscale = ctx->sharpUpscale;
  const bool caching = ctx->caching;
  const int32_t fineScaleFPX = ctx->fineScaleFPX;
  const int32_t invScaleFPX = ctx->invScaleFPX;
  const int32_t fineScaleFPY = ctx->fineScaleFPY;
  const int32_t invScaleFPY = ctx->invScaleFPY;
  GfxRenderer& renderer = *ctx->renderer;
  const int cfgX = ctx->config->x;
  const int cfgY = ctx->config->y;
  const int blockX = pDraw->x;
  const int blockY = pDraw->y;

  // Determine destination pixel range covered by this source block
  const int srcYEnd = blockY + blockH;
  const int srcXEnd = blockX + validW;

  int dstYStart = (int)((int64_t)blockY * fineScaleFPY >> FP_SHIFT);
  int dstYEnd = (srcYEnd >= ctx->scaledSrcHeight) ? ctx->dstHeight : (int)((int64_t)srcYEnd * fineScaleFPY >> FP_SHIFT);
  int dstXStart = (int)((int64_t)blockX * fineScaleFPX >> FP_SHIFT);
  int dstXEnd = (srcXEnd >= ctx->scaledSrcWidth) ? ctx->dstWidth : (int)((int64_t)srcXEnd * fineScaleFPX >> FP_SHIFT);

  // Pre-clamp destination ranges to screen bounds (eliminates per-pixel screen checks)
  int clampYMax = ctx->dstHeight;
  if (ctx->screenHeight - cfgY < clampYMax) clampYMax = ctx->screenHeight - cfgY;
  if (dstYStart < -cfgY) dstYStart = -cfgY;
  if (dstYEnd > clampYMax) dstYEnd = clampYMax;

  int clampXMax = ctx->dstWidth;
  if (ctx->screenWidth - cfgX < clampXMax) clampXMax = ctx->screenWidth - cfgX;
  if (dstXStart < -cfgX) dstXStart = -cfgX;
  if (dstXEnd > clampXMax) dstXEnd = clampXMax;

  if (dstYStart >= dstYEnd || dstXStart >= dstXEnd) return 1;

  // Pre-compute orientation and render-mode state once per callback invocation
  DirectPixelWriter pw;
  pw.init(renderer);

  DirectCacheWriter cw;
  if (caching) {
    if (ctx->stream) {
      cw.initStreaming(ctx->stream);
      // Rows below this block's top belong to fully-decoded MCU-rows — flush them.
      cw.flushBelow(dstYStart);
    } else {
      cw.init(ctx->cache.buffer, ctx->cache.bytesPerRow, ctx->cache.originX);
    }
  }

  // === 1:1 fast path: no scaling math ===
  if (fineScaleFPX == FP_ONE && fineScaleFPY == FP_ONE) {
    for (int dstY = dstYStart; dstY < dstYEnd; dstY++) {
      const int outY = cfgY + dstY;
      pw.beginRow(outY);
      if (caching) cw.beginRow(outY, ctx->config->y);
      const uint8_t* row = &pixels[(dstY - blockY) * stride];
      for (int dstX = dstXStart; dstX < dstXEnd; dstX++) {
        const int outX = cfgX + dstX;
        uint8_t gray = row[dstX - blockX];
        uint8_t dithered = ditherPixel(gray, outX, outY, useDithering, oneBit, blueNoise, tone);
        pw.writePixel(outX, dithered);
        if (caching) cw.writePixel(outX, dithered);
      }
    }
    return 1;
  }

  // === Nearest-neighbor upscale for bimodal high-contrast text images ===
  // Bilinear (below) blends neighbouring pixels, which blurs the thin strokes of a
  // code/terminal screenshot. Point-sampling keeps hard pixel edges — "blocky but
  // crisp" — which reads far better for text. Only used when the probe flagged the
  // image as bimodal text; photos keep the smoother bilinear path.
  if (sharpUpscale && fineScaleFPX > FP_ONE && fineScaleFPY > FP_ONE) {
    for (int dstY = dstYStart; dstY < dstYEnd; dstY++) {
      const int outY = cfgY + dstY;
      pw.beginRow(outY);
      if (caching) cw.beginRow(outY, ctx->config->y);
      int ly = (int)(((int64_t)dstY * invScaleFPY) >> FP_SHIFT) - blockY;
      if (ly < 0) ly = 0;
      if (ly >= blockH) ly = blockH - 1;
      const uint8_t* row = &pixels[ly * stride];
      for (int dstX = dstXStart; dstX < dstXEnd; dstX++) {
        const int outX = cfgX + dstX;
        int lx = (int)(((int64_t)dstX * invScaleFPX) >> FP_SHIFT) - blockX;
        if (lx < 0) lx = 0;
        if (lx >= validW) lx = validW - 1;
        uint8_t dithered = ditherPixel(row[lx], outX, outY, useDithering, oneBit, blueNoise, tone);
        pw.writePixel(outX, dithered);
        if (caching) cw.writePixel(outX, dithered);
      }
    }
    return 1;
  }

  // === Bilinear interpolation (upscale: fineScale > 1.0) ===
  // Smooths block boundaries that would otherwise create visible banding
  // on progressive JPEG DC-only decode (1/8 resolution upscaled to target).
  if (fineScaleFPX > FP_ONE && fineScaleFPY > FP_ONE) {
    // Pre-compute safe X range where lx0 and lx0+1 are both in [0, validW-1].
    // Only the left/right edge pixels (typically 0-2 and 1-8 respectively) need clamping.
    int safeXStart = (int)(((int64_t)blockX * fineScaleFPX + FP_MASK) >> FP_SHIFT);
    int safeXEnd = (int)((int64_t)(blockX + validW - 1) * fineScaleFPX >> FP_SHIFT);
    if (safeXStart < dstXStart) safeXStart = dstXStart;
    if (safeXEnd > dstXEnd) safeXEnd = dstXEnd;
    if (safeXStart > safeXEnd) safeXEnd = safeXStart;

    for (int dstY = dstYStart; dstY < dstYEnd; dstY++) {
      const int outY = cfgY + dstY;
      pw.beginRow(outY);
      if (caching) cw.beginRow(outY, ctx->config->y);
      const int32_t srcFyFP = dstY * invScaleFPY;
      const int32_t fy = srcFyFP & FP_MASK;
      const int32_t fyInv = FP_ONE - fy;
      int ly0 = (srcFyFP >> FP_SHIFT) - blockY;
      int ly1 = ly0 + 1;
      if (ly0 < 0) ly0 = 0;
      if (ly0 >= blockH) ly0 = blockH - 1;
      if (ly1 >= blockH) ly1 = blockH - 1;

      const uint8_t* row0 = &pixels[ly0 * stride];
      const uint8_t* row1 = &pixels[ly1 * stride];

      // Left edge (with X boundary clamping)
      for (int dstX = dstXStart; dstX < safeXStart; dstX++) {
        const int outX = cfgX + dstX;
        const int32_t srcFxFP = dstX * invScaleFPX;
        const int32_t fx = srcFxFP & FP_MASK;
        const int32_t fxInv = FP_ONE - fx;
        int lx0 = (srcFxFP >> FP_SHIFT) - blockX;
        int lx1 = lx0 + 1;
        if (lx0 < 0) lx0 = 0;
        if (lx1 < 0) lx1 = 0;
        if (lx0 >= validW) lx0 = validW - 1;
        if (lx1 >= validW) lx1 = validW - 1;

        int top = ((int)row0[lx0] * fxInv + (int)row0[lx1] * fx) >> FP_SHIFT;
        int bot = ((int)row1[lx0] * fxInv + (int)row1[lx1] * fx) >> FP_SHIFT;
        uint8_t gray = (uint8_t)((top * fyInv + bot * fy) >> FP_SHIFT);

        uint8_t dithered = ditherPixel(gray, outX, outY, useDithering, oneBit, blueNoise, tone);
        pw.writePixel(outX, dithered);
        if (caching) cw.writePixel(outX, dithered);
      }

      // Interior (no X boundary checks — lx0 and lx0+1 guaranteed in bounds)
      for (int dstX = safeXStart; dstX < safeXEnd; dstX++) {
        const int outX = cfgX + dstX;
        const int32_t srcFxFP = dstX * invScaleFPX;
        const int32_t fx = srcFxFP & FP_MASK;
        const int32_t fxInv = FP_ONE - fx;
        const int lx0 = (srcFxFP >> FP_SHIFT) - blockX;

        int top = ((int)row0[lx0] * fxInv + (int)row0[lx0 + 1] * fx) >> FP_SHIFT;
        int bot = ((int)row1[lx0] * fxInv + (int)row1[lx0 + 1] * fx) >> FP_SHIFT;
        uint8_t gray = (uint8_t)((top * fyInv + bot * fy) >> FP_SHIFT);

        uint8_t dithered = ditherPixel(gray, outX, outY, useDithering, oneBit, blueNoise, tone);
        pw.writePixel(outX, dithered);
        if (caching) cw.writePixel(outX, dithered);
      }

      // Right edge (with X boundary clamping)
      for (int dstX = safeXEnd; dstX < dstXEnd; dstX++) {
        const int outX = cfgX + dstX;
        const int32_t srcFxFP = dstX * invScaleFPX;
        const int32_t fx = srcFxFP & FP_MASK;
        const int32_t fxInv = FP_ONE - fx;
        int lx0 = (srcFxFP >> FP_SHIFT) - blockX;
        int lx1 = lx0 + 1;
        if (lx0 >= validW) lx0 = validW - 1;
        if (lx1 >= validW) lx1 = validW - 1;

        int top = ((int)row0[lx0] * fxInv + (int)row0[lx1] * fx) >> FP_SHIFT;
        int bot = ((int)row1[lx0] * fxInv + (int)row1[lx1] * fx) >> FP_SHIFT;
        uint8_t gray = (uint8_t)((top * fyInv + bot * fy) >> FP_SHIFT);

        uint8_t dithered = ditherPixel(gray, outX, outY, useDithering, oneBit, blueNoise, tone);
        pw.writePixel(outX, dithered);
        if (caching) cw.writePixel(outX, dithered);
      }
    }
    return 1;
  }

  // === Box-average downscale (fineScale < 1.0) ===
  // JPEGDEC already performed the coarse 1/2..1/8 downscale (averaged in the IDCT),
  // so the residual ratio here is in (0.5, 1.0] -> each dst pixel covers at most
  // ~2x2 source pixels, and those rows lie within the current MCU block (>= 8 rows
  // tall). Averaging that small window instead of point-sampling preserves thin
  // strokes (e.g. text in a screenshot) that nearest-neighbor would drop. The
  // window is clamped to the block, so a dst pixel straddling a block seam just
  // averages the rows present -- a slight under-average, never an out-of-bounds read.
  for (int dstY = dstYStart; dstY < dstYEnd; dstY++) {
    const int outY = cfgY + dstY;
    pw.beginRow(outY);
    if (caching) cw.beginRow(outY, ctx->config->y);

    int ly0 = (int)(((int64_t)dstY * invScaleFPY) >> FP_SHIFT) - blockY;
    int ly1 = (int)(((int64_t)(dstY + 1) * invScaleFPY) >> FP_SHIFT) - blockY;
    if (ly0 < 0) ly0 = 0;
    if (ly0 >= blockH) ly0 = blockH - 1;
    if (ly1 <= ly0) ly1 = ly0 + 1;
    if (ly1 > blockH) ly1 = blockH;

    for (int dstX = dstXStart; dstX < dstXEnd; dstX++) {
      const int outX = cfgX + dstX;

      int lx0 = (int)(((int64_t)dstX * invScaleFPX) >> FP_SHIFT) - blockX;
      int lx1 = (int)(((int64_t)(dstX + 1) * invScaleFPX) >> FP_SHIFT) - blockX;
      if (lx0 < 0) lx0 = 0;
      if (lx0 >= validW) lx0 = validW - 1;
      if (lx1 <= lx0) lx1 = lx0 + 1;
      if (lx1 > validW) lx1 = validW;

      uint32_t sum = 0;
      int count = 0;
      for (int yy = ly0; yy < ly1; yy++) {
        const uint8_t* row = &pixels[yy * stride];
        for (int xx = lx0; xx < lx1; xx++) {
          sum += row[xx];
          count++;
        }
      }
      const uint8_t gray = static_cast<uint8_t>(sum / count);  // count >= 1 by construction

      uint8_t dithered = ditherPixel(gray, outX, outY, useDithering, oneBit, blueNoise, tone);
      pw.writePixel(outX, dithered);
      if (caching) cw.writePixel(outX, dithered);
    }
  }

  return 1;
}

// Luminance probe: decodes the JPEG at 1/8 scale (≈1/64 the pixels) and
// classifies the image into one of four tonal classes, returning the X4 tone
// curve to apply and setting `sharpUpscale` when thin strokes need nearest-
// neighbor upscale instead of bilinear.
//
// Classification (in priority order):
//   white-on-dark  : dark≥50% && bright≥2%  → X4Tone::None,     sharp=true
//   grey-on-dark   : dark≥50% && bright<2%
//                    && mid∈[MIN,MAX]%       → X4Tone::DarkText, sharp=true
//   dark photo     : dark≥50% (else)         → X4Tone::Brighten, sharp=false
//   light image    : else                    → X4Tone::None,     sharp=false
//
// On any probe failure: returns X4Tone::Brighten, sharpUpscale=false so a
// decode error never regresses a dark image.
X4Tone jpegImageIsDark(const std::string& imagePath, bool& sharpUpscale) {
  sharpUpscale = false;
  std::unique_ptr<JPEGDEC> jpeg(new (std::nothrow) JPEGDEC());
  if (!jpeg) return X4Tone::Brighten;

  int rc = jpeg->open(imagePath.c_str(), jpegOpen, jpegClose, jpegRead, jpegSeek, jpegMeasureCallback);
  const ScopedCleanup cleanup{[&jpeg]() { jpeg->close(); }};
  if (rc != 1) return X4Tone::Brighten;

  JpegLumProbe probe;
  jpeg->setPixelType(EIGHT_BIT_GRAYSCALE);
  jpeg->setUserPointer(&probe);
  if (jpeg->decode(0, 0, JPEG_SCALE_EIGHTH) != 1 || probe.count == 0) return X4Tone::Brighten;

  const uint32_t darkPct   = probe.dark   * 100u / probe.count;
  const uint32_t brightPct = probe.bright * 100u / probe.count;
  const uint32_t midPct    = probe.mid    * 100u / probe.count;

  const X4Tone tone = classifyImageTone(darkPct, brightPct, midPct);
  // Text images (white-on-dark → None+dark, grey-on-dark → DarkText) need sharp
  // upscale; dark photos (Brighten) and light images (None+not-dark) use bilinear.
  sharpUpscale = (tone != X4Tone::Brighten) && (darkPct >= X4_DARK_FRACTION_PCT);

  const char* label = (tone == X4Tone::DarkText)  ? "grey-on-dark/darktext+sharp"
                    : (tone == X4Tone::None && darkPct >= X4_DARK_FRACTION_PCT) ? "white-on-dark/sharp"
                    : (tone == X4Tone::Brighten)   ? "dark/brighten"
                                                   : "light/skip";
  LOG_DBG("JPG", "Dark %u%% Bright %u%% Mid %u%% (%s)", darkPct, brightPct, midPct, label);
  return tone;
}

}  // namespace

bool JpegToFramebufferConverter::getDimensionsStatic(const std::string& imagePath, ImageDimensions& out) {
  size_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < MIN_FREE_HEAP_FOR_JPEG) {
    LOG_ERR("JPG", "Not enough heap for JPEG decoder (%u free, need %u)", freeHeap, MIN_FREE_HEAP_FOR_JPEG);
    return false;
  }

  std::unique_ptr<JPEGDEC> jpeg(new (std::nothrow) JPEGDEC());
  if (!jpeg) {
    LOG_ERR("JPG", "Failed to allocate JPEG decoder for dimensions");
    return false;
  }

  int rc = jpeg->open(imagePath.c_str(), jpegOpen, jpegClose, jpegRead, jpegSeek, nullptr);
  const ScopedCleanup cleanup{[&jpeg]() { jpeg->close(); }};
  if (rc != 1) {
    LOG_ERR("JPG", "Failed to open JPEG for dimensions (err=%d): %s", jpeg->getLastError(), imagePath.c_str());
    return false;
  }

  out.width = jpeg->getWidth();
  out.height = jpeg->getHeight();
  LOG_DBG("JPG", "Image dimensions: %dx%d", out.width, out.height);

  return true;
}

bool JpegToFramebufferConverter::decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer,
                                                     const RenderConfig& config) {
  LOG_DBG("JPG", "Decoding JPEG: %s", imagePath.c_str());

  size_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < MIN_FREE_HEAP_FOR_JPEG) {
    LOG_ERR("JPG", "Not enough heap for JPEG decoder (%u free, need %u)", freeHeap, MIN_FREE_HEAP_FOR_JPEG);
    return false;
  }

  // Only the 4-level X4 path applies a tone curve. For that path, probe luminance
  // to classify the image (light / dark-photo / grey-on-dark / white-on-dark) and
  // select the right curve + upscale mode. Done before allocating the real decoder
  // so only one JPEGDEC (~20 KB) is ever live at a time. The 1-bit (X3) and
  // no-dither paths don't use the curve, so skip the probe entirely.
  bool sharpUpscale = false;
  const X4Tone tone =
      (!config.oneBitDither && config.useDithering) ? jpegImageIsDark(imagePath, sharpUpscale) : X4Tone::None;

  std::unique_ptr<JPEGDEC> jpeg(new (std::nothrow) JPEGDEC());
  if (!jpeg) {
    LOG_ERR("JPG", "Failed to allocate JPEG decoder");
    return false;
  }

  JpegContext ctx;
  ctx.renderer = &renderer;
  ctx.config = &config;
  ctx.screenWidth = renderer.getScreenWidth();
  ctx.screenHeight = renderer.getScreenHeight();
  ctx.tone = tone;
  ctx.sharpUpscale = sharpUpscale;

  int rc = jpeg->open(imagePath.c_str(), jpegOpen, jpegClose, jpegRead, jpegSeek, jpegDrawCallback);
  const ScopedCleanup cleanup{[&jpeg]() { jpeg->close(); }};
  if (rc != 1) {
    LOG_ERR("JPG", "Failed to open JPEG (err=%d): %s", jpeg->getLastError(), imagePath.c_str());
    return false;
  }

  int srcWidth = jpeg->getWidth();
  int srcHeight = jpeg->getHeight();

  if (srcWidth <= 0 || srcHeight <= 0) {
    LOG_ERR("JPG", "Invalid JPEG dimensions: %dx%d", srcWidth, srcHeight);
    return false;
  }

  // JPEGDEC downscales by jpegScaleDenom (1/2..1/8) before delivering pixels, so the
  // decode cost tracks the *scaled* grid, not the raw source. Reject only absurd raw
  // dimensions here (overflow safety); the real pixel-budget check is on the scaled
  // grid below, once jpegScaleDenom is known. (A 1478x2367 cover is 3.5M raw px —
  // over MAX_SOURCE_PIXELS — but scales to well under it and decodes fine.)
  if (srcWidth > 30000 || srcHeight > 30000) {
    LOG_ERR("JPG", "JPEG source dimensions unreasonable: %dx%d", srcWidth, srcHeight);
    return false;
  }

  bool isProgressive = jpeg->getJPEGType() == JPEG_MODE_PROGRESSIVE;
  if (isProgressive) {
    LOG_INF("JPG", "Progressive JPEG detected - decoding DC coefficients only (lower quality)");
  }

  // Calculate overall target scale
  float targetScale;
  int destWidth, destHeight;

  if (config.useExactDimensions && config.maxWidth > 0 && config.maxHeight > 0) {
    destWidth = config.maxWidth;
    destHeight = config.maxHeight;
    targetScale = (float)destWidth / srcWidth;
  } else {
    float scaleX = (config.maxWidth > 0 && srcWidth > config.maxWidth) ? (float)config.maxWidth / srcWidth : 1.0f;
    float scaleY = (config.maxHeight > 0 && srcHeight > config.maxHeight) ? (float)config.maxHeight / srcHeight : 1.0f;
    targetScale = (scaleX < scaleY) ? scaleX : scaleY;
    if (targetScale > 1.0f) targetScale = 1.0f;

    destWidth = (int)(srcWidth * targetScale);
    destHeight = (int)(srcHeight * targetScale);
  }

  // Choose JPEGDEC built-in scaling for coarse downscaling.
  // Progressive JPEGs: JPEGDEC forces JPEG_SCALE_EIGHTH internally (DC-only
  // decode produces 1/8 resolution). We must match this to avoid the if/else
  // priority chain in DecodeJPEG selecting a different scale.
  int jpegScaleOption;
  int jpegScaleDenom;
  if (isProgressive) {
    jpegScaleOption = JPEG_SCALE_EIGHTH;
    jpegScaleDenom = 8;
  } else {
    jpegScaleDenom = chooseJpegScale(targetScale, jpegScaleOption);
  }

  if (destWidth <= 0 || destHeight <= 0) {
    LOG_ERR("JPG", "Degenerate output dimensions %dx%d for %s, skipping render", destWidth, destHeight,
            imagePath.c_str());
    return false;
  }

  ctx.scaledSrcWidth = (srcWidth + jpegScaleDenom - 1) / jpegScaleDenom;
  ctx.scaledSrcHeight = (srcHeight + jpegScaleDenom - 1) / jpegScaleDenom;

  // Pixel-budget check on the actual decode grid (after JPEGDEC's built-in scaling).
  if (ctx.scaledSrcWidth * ctx.scaledSrcHeight > MAX_SOURCE_PIXELS) {
    LOG_ERR("JPG", "Scaled decode grid too large (%dx%d = %d px), max %d", ctx.scaledSrcWidth, ctx.scaledSrcHeight,
            ctx.scaledSrcWidth * ctx.scaledSrcHeight, MAX_SOURCE_PIXELS);
    return false;
  }

  ctx.dstWidth = destWidth;
  ctx.dstHeight = destHeight;
  ctx.fineScaleFPX = (int32_t)((int64_t)destWidth * FP_ONE / ctx.scaledSrcWidth);
  ctx.invScaleFPX = (int32_t)((int64_t)ctx.scaledSrcWidth * FP_ONE / destWidth);
  ctx.fineScaleFPY = (int32_t)((int64_t)destHeight * FP_ONE / ctx.scaledSrcHeight);
  ctx.invScaleFPY = (int32_t)((int64_t)ctx.scaledSrcHeight * FP_ONE / destHeight);

  LOG_DBG("JPG", "JPEG %dx%d -> %dx%d (scale %.2f, jpegScale 1/%d, fineScale %.2f)%s", srcWidth, srcHeight, destWidth,
          destHeight, targetScale, jpegScaleDenom, (float)destWidth / ctx.scaledSrcWidth,
          isProgressive ? " [progressive]" : "");

  // Set pixel type to 8-bit grayscale (must be after open())
  jpeg->setPixelType(EIGHT_BIT_GRAYSCALE);
  jpeg->setUserPointer(&ctx);

  // Caching the decoded px13n lets the grayscale tiled passes re-render the image
  // from SD (~10 strips x 2 planes) instead of re-decoding it each band — without
  // it an image page is ~20 re-decodes (seconds to minutes). Two strategies:
  //   * Small images (<= 48KB at 2bpp) keep the full-RAM-buffer path: decode into
  //     one buffer, write once. Fast, no per-row SD writes.
  //   * Larger images (e.g. a full-page cover, ~80KB) can't fit a contiguous RAM
  //     buffer on this device (largest free block ~63KB), so stream rows to SD as
  //     the decode passes them. Keeps full 4-level quality, one decode.
  // Streaming needs raster (top-to-bottom) row delivery, so progressive JPEGs
  // (non-raster) fall back to no-cache.
  static constexpr size_t JPEG_MAX_FULL_BUFFER_BYTES = 48000;
  static constexpr int STREAM_BAND_ROWS = 128;
  StreamingPixelCache streamCache;
  ctx.caching = !config.cachePath.empty();
  if (ctx.caching) {
    const size_t cacheSize = (size_t)((destWidth + 3) / 4) * destHeight;
    if (cacheSize <= JPEG_MAX_FULL_BUFFER_BYTES && ctx.cache.allocate(destWidth, destHeight, config.x, config.y)) {
      // full-buffer path
    } else if (!isProgressive &&
               streamCache.begin(config.cachePath, destWidth, destHeight, config.x, STREAM_BAND_ROWS)) {
      ctx.stream = &streamCache;
      LOG_DBG("JPG", "Streaming cache to SD: %dx%d (%zu bytes)", destWidth, destHeight, cacheSize);
    } else {
      LOG_DBG("JPG", "Caching disabled (%s)", isProgressive ? "progressive" : "cache init failed");
      ctx.caching = false;
    }
  }

  unsigned long decodeStart = millis();
  rc = jpeg->decode(0, 0, jpegScaleOption);
  unsigned long decodeTime = millis() - decodeStart;

  if (rc != 1) {
    LOG_ERR("JPG", "Decode failed (rc=%d, lastError=%d)", rc, jpeg->getLastError());
    if (ctx.stream) {
      streamCache.finish();                     // close the file before removing it
      Storage.remove(config.cachePath.c_str());  // drop the partial cache
    }
    return false;
  }

  LOG_DBG("JPG", "JPEG decoding complete - render time: %lu ms", decodeTime);

  // Persist the cache. Full-buffer path writes in one shot; streaming has already
  // written most rows during decode and only needs a final flush + close.
  if (ctx.caching) {
    if (ctx.stream) {
      if (!streamCache.finish()) {
        LOG_ERR("JPG", "Streaming cache incomplete; removing partial file");
        Storage.remove(config.cachePath.c_str());
      }
    } else {
      ctx.cache.writeToFile(config.cachePath);
    }
  }

  return true;
}

bool JpegToFramebufferConverter::supportsFormat(const std::string& extension) {
  return FsHelpers::hasJpgExtension(extension);
}
