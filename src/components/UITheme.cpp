#include "UITheme.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <Logging.h>

#include <algorithm>
#include <memory>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/UIScale.h"
#include "components/themes/BaseTheme.h"
#include "components/themes/lyra/Lyra3CoversTheme.h"
#include "components/themes/lyra/LyraTheme.h"
#include "components/themes/roundedraff/RoundedRaffTheme.h"
#include "components/themes/vega/VegaTheme.h"

// main.cpp's renderer singleton; the compact header height is a font metric, and
// MappedInputManager.cpp reaches it the same way.
extern GfxRenderer renderer;

UITheme UITheme::instance;

UITheme::UITheme() {
  auto themeType = static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme);
  setTheme(themeType);
}

void UITheme::reload() {
  auto themeType = static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme);
  setTheme(themeType);
}

// Deliberately the one place in the UI that keeps throwing `new`. Everywhere else a failed
// activity allocation degrades to "stay put", but currentTheme is dereferenced by every draw
// call with no null check, so handing back nullptr would convert a logged abort into a wild
// null-deref — strictly worse. These objects are a vtable pointer each and are only allocated
// at boot and on an explicit theme change, both on a heap with tens of KB free.
void UITheme::setTheme(CrossPointSettings::UI_THEME type) {
  switch (type) {
    case CrossPointSettings::UI_THEME::CLASSIC:
      LOG_DBG("UI", "Using Classic theme");
      currentTheme = std::make_unique<BaseTheme>();
      currentMetrics = &BaseMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::LYRA:
      LOG_DBG("UI", "Using Lyra theme");
      currentTheme = std::make_unique<LyraTheme>();
      currentMetrics = &LyraMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::ROUNDEDRAFF:
      LOG_DBG("UI", "Using RoundedRaff theme");
      currentTheme = std::make_unique<RoundedRaffTheme>();
      currentMetrics = &RoundedRaffMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::LYRA_3_COVERS:
      LOG_DBG("UI", "Using Lyra 3 Covers theme");
      currentTheme = std::make_unique<Lyra3CoversTheme>();
      currentMetrics = &Lyra3CoversMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::VEGA:
      LOG_DBG("UI", "Using Vega theme");
      currentTheme = std::make_unique<VegaTheme>();
      currentMetrics = &VegaMetrics::values;
      break;
  }
  metricsValid = false;
}

UITheme::TopBarPolicyFn UITheme::topBarPolicy = nullptr;

bool UITheme::isTopBarHidden() {
  if (SETTINGS.topBarOtherScreens) return false;
  // No policy installed yet (boot, before main.cpp wires the ActivityManager): keep the band,
  // so nothing renders half-collapsed before the foreground screen is known.
  return topBarPolicy != nullptr && !topBarPolicy();
}

int UITheme::compactHeaderHeight() {
  // One title line plus symmetric breathing room. Derived from the font rather than tabled per
  // theme so it cannot come out shorter than the glyphs it has to hold. Resolved once and kept:
  // getMetrics() runs on every render path, and getLineHeight() both walks the font map and
  // LOG_ERRs for a font that is not inserted yet. Until main.cpp inserts the UI fonts the floor
  // applies; the metrics cache keys on the result, so the band grows to its real height the
  // moment they are up.
  // Air above and below the title line. Enough that a compact band reads as a band
  // and its title does not sit against the panel's top edge.
  constexpr int kTitlePadding = 10;
  constexpr int kFallbackLineHeight = 20;
  static int resolved = 0;
  if (resolved == 0) {
    const int lineHeight = renderer.getLineHeight(uiScaleSpec().titleFontId);
    if (lineHeight <= 0) return kFallbackLineHeight + kTitlePadding * 2;
    resolved = lineHeight + kTitlePadding * 2;
  }
  return resolved;
}

const ThemeMetrics& UITheme::getMetrics() const {
  // hasTouch() can flip once touch init completes after static construction, so the
  // cached copy is refreshed when the flag differs instead of copying the struct per call.
  const bool touch = gpio.hasTouch();
  // Shrinking headerHeight is what reclaims the bar: every screen sizes its content from
  // topPadding + headerHeight, so the layout closes up without per-screen arithmetic. The band
  // does not vanish — it keeps the screen title; drawHeader() drops the battery, WiFi bars,
  // clock and rule around it. 0 means "top bar shown", and doubles as the cache key.
  const int compactHeader = isTopBarHidden() ? compactHeaderHeight() : 0;
  // Lists follow the reader's Screen Margin, so one setting sets how close text
  // sits to the bezel everywhere. Only on a theme that draws a row BAND
  // (listInset > 0): a theme without one -- Classic -- carries its whole indent
  // as listSidePadding, which is text padding inside the row, not a margin.
  const int listInset = currentMetrics->listInset > 0 ? SETTINGS.screenMargin : currentMetrics->listInset;
  if (!metricsValid || touch != metricsForTouch || compactHeader != metricsCompactHeader ||
      listInset != metricsListInset) {
    adjustedMetrics = *currentMetrics;
    if (touch) {
      adjustedMetrics.buttonHintsHeight = 0;
    }
    // Never taller than the themed band: on a theme whose header is already tight this is a no-op.
    if (compactHeader > 0 && compactHeader < adjustedMetrics.headerHeight) {
      adjustedMetrics.headerHeight = compactHeader;
    }
    adjustedMetrics.listInset = listInset;
    metricsForTouch = touch;
    metricsCompactHeader = compactHeader;
    metricsListInset = listInset;
    metricsValid = true;
  }
  return adjustedMetrics;
}

int UITheme::getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasTabBar, bool hasButtonHints,
                                     bool hasSubtitle, int extraReservedHeight) {
  const ThemeMetrics metrics = UITheme::getInstance().getMetrics();
  auto orientation = renderer.getOrientation();
  int reservedHeight = metrics.topPadding;
  if (hasHeader) {
    reservedHeight += metrics.headerHeight + metrics.verticalSpacing;
  }
  if (hasTabBar) {
    reservedHeight += metrics.tabBarHeight;
  }
  if (hasButtonHints && orientation != GfxRenderer::Orientation::LandscapeClockwise &&
      orientation != GfxRenderer::Orientation::LandscapeCounterClockwise) {
    reservedHeight += metrics.verticalSpacing + metrics.buttonHintsHeight;
  }
  const int availableHeight = renderer.getScreenHeight() - reservedHeight - extraReservedHeight;
  return UITheme::getInstance().getTheme().getListPageItems(availableHeight, hasSubtitle);
}

// Screen area excluding the button hints
Rect UITheme::getScreenSafeArea(const GfxRenderer& renderer, bool hasFrontButtonHints, bool hasSideButtonHints) {
  auto orientation = renderer.getOrientation();
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  Rect safeArea = Rect{0, 0, screenWidth, screenHeight};
  const ThemeMetrics metrics = getMetrics();
  switch (orientation) {
    case GfxRenderer::Orientation::Portrait:
      if (hasFrontButtonHints) {
        safeArea.height -= metrics.buttonHintsHeight;
      }
      break;
    case GfxRenderer::Orientation::LandscapeClockwise:
      if (hasFrontButtonHints) {
        safeArea.x += metrics.buttonHintsHeight;
        safeArea.width -= metrics.buttonHintsHeight;
      }
      break;
    case GfxRenderer::Orientation::PortraitInverted:
      if (hasFrontButtonHints) {
        safeArea.y += metrics.buttonHintsHeight;
        safeArea.height -= metrics.buttonHintsHeight;
      }
      break;
    case GfxRenderer::Orientation::LandscapeCounterClockwise:
      if (hasFrontButtonHints) {
        safeArea.width -= metrics.buttonHintsHeight;
      }
      break;
  }
  return safeArea;
}

std::string UITheme::getCoverThumbPath(std::string coverBmpPath, int coverHeight) {
  size_t pos = coverBmpPath.find("[HEIGHT]", 0);
  if (pos != std::string::npos) {
    coverBmpPath.replace(pos, 8, std::to_string(coverHeight));
  }
  return coverBmpPath;
}

UIIcon UITheme::getFileIcon(const std::string& filename) {
  // Only folders get an icon; books and files render without one (None) so the
  // file browser and recent-books lists show an icon for directories only.
  return filename.back() == '/' ? Folder : None;
}

int UITheme::getStatusBarHeight() {
  const ThemeMetrics metrics = UITheme::getInstance().getMetrics();

  // Add status bar margin
  const bool showStatusBar =
      SETTINGS.statusBarChapterPageCount || SETTINGS.statusBarBookProgressPercentage ||
      SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE || SETTINGS.statusBarBattery ||
      SETTINGS.statusBarClock != CrossPointSettings::STATUS_BAR_CLOCK_MODE::STATUS_BAR_CLOCK_HIDE;
  const bool showProgressBar =
      SETTINGS.statusBarProgressBar != CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS;
  return (showStatusBar ? (metrics.statusBarVerticalMargin) : 0) +
         (showProgressBar ? (((SETTINGS.statusBarProgressBarThickness + 1) * 2) + metrics.progressBarMarginTop) : 0);
}

int UITheme::getProgressBarHeight() {
  const ThemeMetrics metrics = UITheme::getInstance().getMetrics();
  const bool showProgressBar =
      SETTINGS.statusBarProgressBar != CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS;
  return (showProgressBar ? (((SETTINGS.statusBarProgressBarThickness + 1) * 2) + metrics.progressBarMarginTop) : 0);
}

// Centered text implementation that takes the safe area into account
void UITheme::drawCenteredText(const GfxRenderer& renderer, Rect screen, int fontId, int y, const char* text,
                               bool black, EpdFontFamily::Style style) {
  const int x = screen.x + (screen.width - renderer.getTextWidth(fontId, text, style)) / 2;
  renderer.drawText(fontId, x, y, text, black, style);
}

void UITheme::drawCenteredWrappedText(const GfxRenderer& renderer, Rect bounds, int fontId, const char* text,
                                      int maxLines, bool black, EpdFontFamily::Style style,
                                      TextVerticalAlignment verticalAlignment) {
  if (!text || *text == '\0' || bounds.width <= 0 || bounds.height <= 0 || maxLines <= 0) return;

  const int lineHeight = renderer.getLineHeight(fontId);
  if (lineHeight <= 0) return;

  const int lineLimit = std::min(maxLines, bounds.height / lineHeight);
  if (lineLimit <= 0) return;

  const auto alignedTop = [&](const int textHeight) {
    switch (verticalAlignment) {
      case TextVerticalAlignment::CENTER:
        return bounds.y + (bounds.height - textHeight) / 2;
      case TextVerticalAlignment::BOTTOM:
        return bounds.y + bounds.height - textHeight;
      case TextVerticalAlignment::TOP:
      default:
        return bounds.y;
    }
  };

  if (renderer.getTextWidth(fontId, text, style) <= bounds.width) {
    drawCenteredText(renderer, bounds, fontId, alignedTop(lineHeight), text, black, style);
    return;
  }

  const auto lines = renderer.wrappedText(fontId, text, bounds.width, lineLimit, style);
  int y = alignedTop(static_cast<int>(lines.size()) * lineHeight);
  for (const auto& line : lines) {
    drawCenteredText(renderer, bounds, fontId, y, line.c_str(), black, style);
    y += lineHeight;
  }
}
