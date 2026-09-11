#pragma once
#include "fontIds.h"

// FreeInkUI font slots. Row heights, header height, and touch sizes are not
// chosen here: FreeInkApp derives all metric tokens from the body font's line
// height (themeTokensForLineHeight). One fixed tier for every board — the
// user-facing UI-scale setting was removed.
struct UIScaleSpec {
  int smallFontId;
  int bodyFontId;
  int titleFontId;
};

inline UIScaleSpec uiScaleSpec() {
  UIScaleSpec spec{};
  // One size for the whole UI: headers, list rows, dialogs and toasts all draw
  // at Ubuntu 10 (24px line), so nothing on a screen reads a tier above or
  // below its neighbours. Emphasis comes from weight (titles are bold), not
  // from size. The slots stay distinct so a component can still ask for a
  // specific role, and so a future scale setting has somewhere to land.
  //
  // Band geometry does not follow: header height and list row height come from
  // ThemeMetrics constants (BaseTheme.h), not from these fonts, so changing the
  // tier resizes text without reflowing any screen.
  //
  // The UI font, not a reader font: fui headers draw book and directory titles,
  // and the built-in Ubuntu UI fonts cover Hebrew (plus the size-matched SD CJK
  // fallback) where the NotoSans reader subsets do not.
  spec.smallFontId = UI_10_FONT_ID;
  spec.bodyFontId = UI_10_FONT_ID;
  spec.titleFontId = UI_10_FONT_ID;
  return spec;
}
