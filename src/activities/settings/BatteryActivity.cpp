#include "BatteryActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalGPIO.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BatteryLog.h"

namespace {
constexpr int PLOT_COLUMN_PX = 2;
constexpr int STRIP_GAP_PX = 3;  // between the plot, each strip row and the time labels
constexpr uint32_t DAY_S = 86400;
// Session mode has no time axis; these sample counts roughly match the dated spans
// (one sample per 10 awake minutes plus wake/sleep pairs).
constexpr uint32_t SESSION_SAMPLES[] = {150, 700, battery::RING_CAPACITY};
constexpr uint32_t RANGE_SPAN_S[] = {DAY_S, 7 * DAY_S, 30 * DAY_S};

// "6.7": tenths from a ratio, no float formatting.
void formatTenths(const uint64_t tenths, char* buf, const size_t len) {
  snprintf(buf, len, "%lu.%lu", static_cast<unsigned long>(tenths / 10), static_cast<unsigned long>(tenths % 10));
}

void formatDate(const uint32_t epoch, char* buf, const size_t len) {
  const time_t t = static_cast<time_t>(epoch);
  struct tm local;
  localtime_r(&t, &local);
  strftime(buf, len, "%Y-%m-%d", &local);
}

void drawDotted(const GfxRenderer& renderer, const int x1, const int y1, const int x2, const int y2) {
  const int steps = std::max(std::abs(x2 - x1), std::abs(y2 - y1));
  if (steps == 0) {
    renderer.drawPixel(x1, y1);
    return;
  }
  for (int i = 0; i <= steps; i += 3) {
    renderer.drawPixel(x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps);
  }
}

// Word-wraps one row to the width available (up to 3 lines), dropping lines that would
// cross `bottom`. Returns the y below what was drawn.
int drawWrapped(const GfxRenderer& renderer, const int font, const int x, int y, const int maxWidth, const int bottom,
                const char* text, const EpdFontFamily::Style style) {
  const int lineH = renderer.getLineHeight(font) + 4;
  for (const std::string& line : renderer.wrappedText(font, text, maxWidth, 3, style)) {
    if (y + lineH > bottom) break;
    renderer.drawText(font, x, y, line.c_str(), true, style);
    y += lineH;
  }
  return y;
}

struct StatsCtx {
  battery::LastChargeFinder charge;
  battery::UsageAccumulator sinceCharge;
  battery::UsageAccumulator week;
  uint32_t weekStart = 0;  // 0 = no clock, week not computed
  uint32_t count = 0;
};

struct GraphCtx {
  battery::BucketBuilder* builder;
  bool session;
  uint32_t t0;
  uint32_t t1;
  uint32_t startSeq;
  uint32_t total;
  bool any = false;
};
}  // namespace

BatteryActivity::BatteryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Battery", renderer, mappedInput) {}

void BatteryActivity::onEnter() {
  Activity::onEnter();

  {
    // The gauge shares I2C with the render task's status-bar reads.
    RenderLock lock(*this);
    nowSocOk = powerManager.getBatteryPercentageChecked(nowSoc);
    nowMv = powerManager.getBatteryMillivolts();
    haveGauge = powerManager.readGaugeHealth(gauge) && gauge.designMah > 0;
  }
  usbNow = gpio.isUsbConnectedCached();
  time_t t = 0;
  nowEpoch =
      (halClock.utcEpoch(t) && t >= static_cast<time_t>(battery::MIN_VALID_EPOCH)) ? static_cast<uint32_t>(t) : 0;
  haveHeader = BatteryLog::readHeader(header);
  range = static_cast<Range>(std::min<uint8_t>(APP_STATE.batteryGraphRange, 2));
  savedRange = range;

  const Rect plot = plotRect(contentRect());
  bucketCount = std::max(1, plot.width / PLOT_COLUMN_PX);
  // ~5 bytes per column (<= 2 KB at 800 px); a per-record array would need 16+ KB.
  buckets = makeUniqueNoThrow<battery::Bucket[]>(static_cast<size_t>(bucketCount));
  if (!buckets) {
    LOG_ERR("BATTERY", "OOM: %d graph buckets", bucketCount);
    bucketCount = 0;
  }

  buildStats();
  buildGraph();
  requestUpdate();
}

void BatteryActivity::onExit() {
  buckets.reset();
  // Saved once on the way out, not on every range press.
  if (range != savedRange) {
    APP_STATE.batteryGraphRange = static_cast<uint8_t>(range);
    APP_STATE.saveToFile();
  }
  Activity::onExit();
}

void BatteryActivity::buildStats() {
  // ~400 bytes of accumulators: heap, not the loop task's stack.
  auto ctx = makeUniqueNoThrow<StatsCtx>();
  if (!ctx) {
    LOG_ERR("BATTERY", "OOM: stats");
    return;
  }
  ctx->weekStart = nowEpoch > 7 * DAY_S ? nowEpoch - 7 * DAY_S : 0;
  BatteryLog::forEach(
      [](const battery::Record& r, void* p) {
        auto* c = static_cast<StatsCtx*>(p);
        ++c->count;
        const uint32_t before = c->charge.endSeq();
        c->charge.add(r);
        // A newer charge end restarts the "since charge" totals from that sample.
        if (c->charge.endSeq() != before) c->sinceCharge = battery::UsageAccumulator{};
        c->sinceCharge.add(r);
        if (c->weekStart != 0 && r.dated() && r.epoch >= c->weekStart) c->week.add(r);
      },
      ctx.get());
  recordCount = ctx->count;
  chargeFound = ctx->charge.found();
  chargingNow = ctx->charge.charging() || usbNow;
  chargeEndEpoch = ctx->charge.endEpoch();
  chargeEndSoc = ctx->charge.endSoc();
  sinceCharge = ctx->sinceCharge.usage();
  haveWeek = ctx->weekStart != 0 && ctx->week.usage().samples > 1;
  lastWeek = ctx->week.usage();
}

void BatteryActivity::buildGraph() {
  graphEmpty = true;
  if (!buckets || !haveHeader) return;
  for (int i = 0; i < bucketCount; ++i) buckets[i] = battery::Bucket{};

  const int r = static_cast<int>(range);
  battery::BucketBuilder builder(buckets.get(), bucketCount);
  GraphCtx ctx{&builder, false, 0, 0, 0, 0};

  // Dated axis whenever the clock is known and the log has dated samples; by sample order
  // otherwise (an X4 that has not had a Wi-Fi time sync). On a mixed X4 log the dated view
  // leaves out the undated sessions.
  sessionMode = nowEpoch == 0 || header.firstEpoch == 0;
  if (!sessionMode) {
    ctx.t1 = nowEpoch + 1;
    ctx.t0 = nowEpoch > RANGE_SPAN_S[r] ? nowEpoch - RANGE_SPAN_S[r] : 0;
    graphT0 = ctx.t0;
    graphT1 = ctx.t1;
    BatteryLog::forEach(
        [](const battery::Record& rec, void* p) {
          auto* c = static_cast<GraphCtx*>(p);
          if (rec.socKnown() && rec.dated() && rec.epoch >= c->t0 && rec.epoch < c->t1) c->any = true;
          c->builder->addDated(rec, c->t0, c->t1);
        },
        &ctx);
  }
  if (sessionMode) {
    battery::BucketBuilder sessionBuilder(buckets.get(), bucketCount);
    const uint32_t newest = header.nextSeq;
    const uint32_t first = battery::firstSeq(newest);
    const uint32_t available = newest - first;
    ctx.builder = &sessionBuilder;
    ctx.total = std::min(available, SESSION_SAMPLES[r]);
    ctx.startSeq = newest - ctx.total;
    BatteryLog::forEach(
        [](const battery::Record& rec, void* p) {
          auto* c = static_cast<GraphCtx*>(p);
          if (rec.seq < c->startSeq) return;
          if (rec.socKnown()) c->any = true;
          c->builder->addIndexed(rec, rec.seq - c->startSeq, c->total);
        },
        &ctx);
  }
  graphEmpty = !ctx.any;
}

void BatteryActivity::selectTab(const int index) {
  if (index < 0 || index >= TAB_COUNT || TAB_ORDER[index] == tab) return;
  tab = TAB_ORDER[index];
  requestUpdate();
}

void BatteryActivity::cycleRange() {
  range = static_cast<Range>((static_cast<int>(range) + 1) % 3);
  buildGraph();
  requestUpdate();
}

Rect BatteryActivity::summaryRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int y = safe.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  return Rect{safe.x, y, safe.width, renderer.getLineHeight(SMALL_FONT_ID) + 2};
}

Rect BatteryActivity::tabBarRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect summary = summaryRect();
  return Rect{summary.x, summary.y + summary.height + metrics.verticalSpacing, summary.width, metrics.tabBarHeight};
}

Rect BatteryActivity::contentRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect tabs = tabBarRect();
  const int top = tabs.y + tabs.height + metrics.verticalSpacing;
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int height = safe.y + safe.height - top - metrics.verticalSpacing;
  return Rect{safe.x, top, safe.width, std::max(0, height)};
}

Rect BatteryActivity::plotRect(const Rect& content) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineH = renderer.getLineHeight(SMALL_FONT_ID);
  // The left column holds the % labels and the strip row labels.
  const int axisW = std::max({renderer.getTextWidth(SMALL_FONT_ID, "100%"),
                              renderer.getTextWidth(SMALL_FONT_ID, tr(STR_BATTERY_ROW_AWAKE)),
                              renderer.getTextWidth(SMALL_FONT_ID, tr(STR_BATTERY_ROW_CHARGE))}) +
                    6;
  const int x = content.x + metrics.contentSidePadding + axisW;
  const int right = content.x + content.width - metrics.contentSidePadding;
  const int top = content.y + lineH + 8;  // range caption above
  // Below: the Awake and Charge rows, then the time labels.
  const int bottom = content.y + content.height - 3 * (lineH + STRIP_GAP_PX);
  return Rect{x, top, std::max(0, right - x), std::max(0, bottom - top)};
}

std::vector<TabInfo> BatteryActivity::buildTabs() const {
  return {{tr(STR_BATTERY_LEVEL), tab == Tab::Level},
          {tr(STR_BATTERY_USAGE), tab == Tab::Usage},
          {tr(STR_BATTERY_HEALTH), tab == Tab::Health}};
}

void BatteryActivity::loop() {
  if (mappedInput.hasTouch()) {
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasScreenTapped(tx, ty)) {
      int index = 0;
      if (GUI.tabIndexFromPoint(renderer, tabBarRect(), buildTabs(), tx, ty, index)) {
        selectTab(index);
        return;
      }
      const Rect content = contentRect();
      if (tab == Tab::Level && ty >= content.y && ty < content.y + content.height) {
        cycleRange();
        return;
      }
    }
  }

  int current = 0;
  for (int i = 0; i < TAB_COUNT; ++i)
    if (TAB_ORDER[i] == tab) current = i;
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    selectTab((current + TAB_COUNT - 1) % TAB_COUNT);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    selectTab((current + 1) % TAB_COUNT);
    return;
  }
  // On press, not release: Settings opens this screen on the Confirm press, so its release
  // lands here and would change the range on entry.
  if (tab == Tab::Level && mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    cycleRange();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
  }
}

void BatteryActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, UITheme::getInstance().getSafeHeaderRect(renderer), tr(STR_BATTERY));

  // Now: "92%, 4.05 V, On battery"
  const Rect summary = summaryRect();
  char socBuf[8];
  if (nowSocOk) {
    snprintf(socBuf, sizeof(socBuf), "%u%%", static_cast<unsigned>(nowSoc));
  } else {
    snprintf(socBuf, sizeof(socBuf), "--");
  }
  char line[96];
  snprintf(line, sizeof(line), "%s: %s, %u.%02u V, %s", tr(STR_BATTERY_NOW), socBuf,
           static_cast<unsigned>(nowMv / 1000), static_cast<unsigned>((nowMv % 1000) / 10),
           usbNow ? tr(STR_BATTERY_ON_USB) : tr(STR_BATTERY_ON_BATTERY));
  const std::string fitted =
      renderer.truncatedText(SMALL_FONT_ID, line, summary.width - 2 * metrics.contentSidePadding);
  renderer.drawText(SMALL_FONT_ID, summary.x + metrics.contentSidePadding, summary.y, fitted.c_str());

  GUI.drawTabBar(renderer, tabBarRect(), buildTabs(), true);

  const Rect content = contentRect();
  switch (tab) {
    case Tab::Level:
      renderLevel(content);
      break;
    case Tab::Usage:
      renderUsage(content);
      break;
    case Tab::Health:
      renderHealth(content);
      break;
  }

  const char* confirmHint = tab == Tab::Level ? tr(STR_BATTERY_RANGE) : "";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmHint, tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void BatteryActivity::renderLevel(const Rect& content) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int left = content.x + metrics.contentSidePadding;
  static constexpr StrId RANGE_LABELS[] = {StrId::STR_BATTERY_RANGE_DAY, StrId::STR_BATTERY_RANGE_WEEK,
                                           StrId::STR_BATTERY_RANGE_MONTH};
  const char* caption = sessionMode ? tr(STR_BATTERY_BY_SESSION) : I18N.get(RANGE_LABELS[static_cast<int>(range)]);
  const std::string fitted = renderer.truncatedText(
      SMALL_FONT_ID, caption, content.width - 2 * metrics.contentSidePadding, EpdFontFamily::BOLD);
  renderer.drawText(SMALL_FONT_ID, left, content.y, fitted.c_str(), true, EpdFontFamily::BOLD);

  const Rect plot = plotRect(content);
  if (plot.width <= 0 || plot.height <= 0) return;
  if (graphEmpty) {
    const char* msg = recordCount == 0 ? tr(STR_BATTERY_NO_DATA) : tr(STR_BATTERY_NO_SAMPLES_IN_RANGE);
    const int w = renderer.getTextWidth(UI_10_FONT_ID, msg);
    renderer.drawText(UI_10_FONT_ID, plot.x + (plot.width - w) / 2, plot.y + plot.height / 2, msg);
  }

  const auto socToY = [&](const int soc) { return plot.y + plot.height - 1 - soc * (plot.height - 1) / 100; };
  const int lineH = renderer.getLineHeight(SMALL_FONT_ID);

  // Gridlines every 25%, labelled at 0/50/100.
  for (int soc = 0; soc <= 100; soc += 25) {
    const int y = socToY(soc);
    for (int x = plot.x; x < plot.x + plot.width; x += 4) renderer.drawPixel(x, y);
    if (soc % 50 == 0) {
      char label[8];
      snprintf(label, sizeof(label), "%d%%", soc);
      const int w = renderer.getTextWidth(SMALL_FONT_ID, label);
      renderer.drawText(SMALL_FONT_ID, plot.x - w - 6, y - lineH / 2, label);
    }
  }
  renderer.drawLine(plot.x, plot.y, plot.x, plot.y + plot.height - 1);

  int prevX = -1;
  int prevY = 0;
  for (int col = 0; col < bucketCount && !graphEmpty; ++col) {
    const battery::Bucket& b = buckets[col];
    if (b.empty()) continue;
    const int x = plot.x + col * PLOT_COLUMN_PX;
    const int yFirst = socToY(b.firstSoc);
    if (prevX >= 0) {
      // Dotted across a sleep (no samples while off), solid within an awake session.
      if (b.flags & battery::BUCKET_WAKE) {
        drawDotted(renderer, prevX, prevY, x, yFirst);
      } else {
        renderer.drawLine(prevX, prevY, x, yFirst, 2, true);
      }
    }
    const int yTop = socToY(b.maxSoc);
    const int yBottom = socToY(b.minSoc);
    renderer.fillRect(x, yTop, PLOT_COLUMN_PX, std::max(2, yBottom - yTop + 1));
    prevX = x;
    prevY = socToY(b.lastSoc);
  }

  // Awake and Charge rows: solid bars where the flag is set, labelled in the axis column.
  const int awakeY = plot.y + plot.height + STRIP_GAP_PX;
  const int chargeY = awakeY + lineH + STRIP_GAP_PX;
  const int barInset = std::max(1, lineH / 5);
  const auto stripRow = [&](const int rowY, const char* name, const uint8_t flag) {
    const int w = renderer.getTextWidth(SMALL_FONT_ID, name);
    renderer.drawText(SMALL_FONT_ID, plot.x - w - 6, rowY, name);
    if (graphEmpty) return;
    for (int col = 0; col < bucketCount; ++col) {
      // Runs of flagged columns are drawn as one rect.
      if (!(buckets[col].flags & flag)) continue;
      int end = col;
      while (end + 1 < bucketCount && (buckets[end + 1].flags & flag)) ++end;
      renderer.fillRect(plot.x + col * PLOT_COLUMN_PX, rowY + barInset, (end - col + 1) * PLOT_COLUMN_PX,
                        lineH - 2 * barInset);
      col = end;
    }
  };
  stripRow(awakeY, tr(STR_BATTERY_ROW_AWAKE), battery::BUCKET_AWAKE);
  stripRow(chargeY, tr(STR_BATTERY_ROW_CHARGE), battery::BUCKET_CHARGING);

  const int labelY = chargeY + lineH + STRIP_GAP_PX;
  if (!sessionMode) {
    renderTimeAxis(plot, labelY);
    return;
  }
  // Session mode has no clock: oldest at the left, newest at the right.
  renderer.drawText(SMALL_FONT_ID, plot.x, labelY, tr(STR_BATTERY_OLDER));
  const char* rightLabel = tr(STR_BATTERY_NOW);
  renderer.drawText(SMALL_FONT_ID, plot.x + plot.width - renderer.getTextWidth(SMALL_FONT_ID, rightLabel), labelY,
                    rightLabel);
}

void BatteryActivity::renderTimeAxis(const Rect& plot, const int labelY) const {
  if (graphT1 <= graphT0 || bucketCount <= 0) return;
  const uint32_t span = graphT1 - graphT0;
  // Same column mapping as BucketBuilder::addDated, so ticks line up with the samples.
  const auto xOf = [&](const uint32_t epoch) {
    const int col = static_cast<int>(static_cast<uint64_t>(epoch - graphT0) * bucketCount / span);
    return plot.x + col * PLOT_COLUMN_PX;
  };
  const auto inWindow = [&](const time_t t) {
    return t >= static_cast<time_t>(graphT0) && t < static_cast<time_t>(graphT1);
  };
  const auto gridline = [&](const int x) {
    for (int y = plot.y; y < plot.y + plot.height; y += 4) renderer.drawPixel(x, y);
  };
  int labelRight = plot.x - 1;  // right edge of the last label drawn, to skip overlaps
  const auto label = [&](const int centre, const char* text) {
    const int w = renderer.getTextWidth(SMALL_FONT_ID, text);
    if (w > plot.width) return;
    const int x = std::clamp(centre - w / 2, plot.x, plot.x + plot.width - w);
    if (x <= labelRight + 4) return;
    renderer.drawText(SMALL_FONT_ID, x, labelY, text);
    labelRight = x + w;
  };

  // Local calendar walk through mktime (tm_isdst = -1), so DST days are 23/25 hours.
  const time_t start = static_cast<time_t>(graphT0);
  struct tm tick;
  localtime_r(&start, &tick);
  tick.tm_min = 0;
  tick.tm_sec = 0;
  tick.tm_isdst = -1;
  char text[12];

  if (range == Range::Day) {
    // Every 6 hours on the local clock: 00, 06, 12, 18.
    tick.tm_hour = (tick.tm_hour / 6) * 6;
    for (int i = 0; i < 6; ++i, tick.tm_hour += 6) {
      tick.tm_isdst = -1;
      const time_t t = mktime(&tick);
      if (!inWindow(t)) continue;
      const int x = xOf(static_cast<uint32_t>(t));
      gridline(x);
      if (SETTINGS.clockFormat == 1) {
        const int h12 = tick.tm_hour % 12 == 0 ? 12 : tick.tm_hour % 12;
        snprintf(text, sizeof(text), "%d%s", h12, tick.tm_hour < 12 ? "am" : "pm");
      } else {
        snprintf(text, sizeof(text), "%02d:00", tick.tm_hour);
      }
      label(x, text);
    }
    return;
  }

  // 7 days: a line at each local midnight, the day of the month centred under its day.
  // 30 days: a line and a "day/month" label every 5th day.
  tick.tm_hour = 0;
  const bool week = range == Range::Week;
  const int days = week ? 8 : 31;
  for (int i = 0; i <= days; ++i, ++tick.tm_mday) {
    tick.tm_isdst = -1;
    struct tm day = tick;
    const time_t midnight = mktime(&day);
    if (week) {
      struct tm next = tick;
      ++next.tm_mday;
      next.tm_isdst = -1;
      const time_t nextMidnight = mktime(&next);
      if (inWindow(midnight)) gridline(xOf(static_cast<uint32_t>(midnight)));
      // Only label a day with enough of it on the graph to sit under.
      const uint32_t from = std::max<uint32_t>(static_cast<uint32_t>(midnight), graphT0);
      const uint32_t to = std::min<uint32_t>(static_cast<uint32_t>(nextMidnight), graphT1);
      if (to <= from || to - from < DAY_S / 2) continue;
      snprintf(text, sizeof(text), "%d", day.tm_mday);
      label((xOf(from) + xOf(to - 1)) / 2, text);
    } else {
      if (!inWindow(midnight) || day.tm_mday % 5 != 0 || day.tm_mday > 30) continue;
      const int x = xOf(static_cast<uint32_t>(midnight));
      gridline(x);
      snprintf(text, sizeof(text), "%d/%d", day.tm_mday, day.tm_mon + 1);
      label(x, text);
    }
  }
}

int BatteryActivity::renderUsageBlock(const char* title, const battery::Usage& u, const int x, int y,
                                      const int bottom) const {
  char buf[96];
  char a[24];
  char b[24];
  // Rows that would cross the button hints are dropped (landscape has room for fewer).
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int right = safe.x + safe.width - metrics.contentSidePadding;
  const auto row = [&](const char* text, const int font, const int indent, const EpdFontFamily::Style style) {
    y = drawWrapped(renderer, font, x + indent, y, right - x - indent, bottom, text, style);
  };

  row(title, UI_10_FONT_ID, 0, EpdFontFamily::BOLD);
  snprintf(buf, sizeof(buf), tr(STR_BATTERY_USED_FORMAT), static_cast<unsigned long>(u.usedPct));
  row(buf, UI_10_FONT_ID, 0, EpdFontFamily::REGULAR);
  battery::formatSpan(u.awakeS, a, sizeof(a));
  snprintf(buf, sizeof(buf), tr(STR_BATTERY_AWAKE_FORMAT), a);
  row(buf, UI_10_FONT_ID, 0, EpdFontFamily::REGULAR);
  if (u.readingS > 0 || u.wifiS > 0) {
    battery::formatSpan(u.readingS, a, sizeof(a));
    battery::formatSpan(u.wifiS, b, sizeof(b));
    snprintf(buf, sizeof(buf), tr(STR_BATTERY_AWAKE_SPLIT_FORMAT), a, b);
    row(buf, SMALL_FONT_ID, 12, EpdFontFamily::REGULAR);
  }
  if (u.asleepS > 0) {
    battery::formatSpan(u.asleepS, a, sizeof(a));
    snprintf(buf, sizeof(buf), tr(STR_BATTERY_ASLEEP_FORMAT), a);
    row(buf, UI_10_FONT_ID, 0, EpdFontFamily::REGULAR);
  }
  // Rates only once there is enough time behind them to mean something.
  if (u.awakeS >= 1800) {
    formatTenths(static_cast<uint64_t>(u.usedAwakePct) * 36000 / u.awakeS, a, sizeof(a));
    snprintf(buf, sizeof(buf), tr(STR_BATTERY_DRAIN_AWAKE_FORMAT), a);
    row(buf, UI_10_FONT_ID, 0, EpdFontFamily::REGULAR);
  }
  if (u.asleepS >= 6 * 3600) {
    formatTenths(static_cast<uint64_t>(u.usedAsleepPct) * 864000 / u.asleepS, a, sizeof(a));
    snprintf(buf, sizeof(buf), tr(STR_BATTERY_DRAIN_ASLEEP_FORMAT), a);
    row(buf, UI_10_FONT_ID, 0, EpdFontFamily::REGULAR);
  }
  return y;
}

void BatteryActivity::renderUsage(const Rect& content) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int x = content.x + metrics.contentSidePadding;
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID) + 4;
  int y = content.y;

  if (recordCount == 0) {
    renderer.drawText(UI_10_FONT_ID, x, y, tr(STR_BATTERY_NO_DATA));
    return;
  }

  char buf[96];
  if (chargingNow) {
    snprintf(buf, sizeof(buf), "%s", tr(STR_BATTERY_CHARGING_NOW));
  } else if (!chargeFound || chargeEndSoc > 100) {
    snprintf(buf, sizeof(buf), "%s", tr(STR_BATTERY_NO_CHARGE));
  } else if (chargeEndEpoch != 0 && nowEpoch >= chargeEndEpoch) {
    char ago[24];
    battery::formatSpan(nowEpoch - chargeEndEpoch, ago, sizeof(ago));
    snprintf(buf, sizeof(buf), tr(STR_BATTERY_CHARGED_AGO_FORMAT), static_cast<unsigned>(chargeEndSoc), ago);
  } else {
    snprintf(buf, sizeof(buf), tr(STR_BATTERY_CHARGED_TO_FORMAT), static_cast<unsigned>(chargeEndSoc));
  }
  const int bottom = content.y + content.height;
  y = drawWrapped(renderer, UI_10_FONT_ID, x, y, content.x + content.width - metrics.contentSidePadding - x, bottom,
                  buf, EpdFontFamily::REGULAR);
  y += metrics.verticalSpacing;

  y = renderUsageBlock(tr(STR_BATTERY_SINCE_CHARGE), sinceCharge, x, y, bottom);
  if (haveWeek && y + lineH * 3 < bottom) {
    y += metrics.verticalSpacing;
    renderUsageBlock(tr(STR_BATTERY_RANGE_WEEK), lastWeek, x, y, bottom);
  }
}

void BatteryActivity::renderHealth(const Rect& content) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int x = content.x + metrics.contentSidePadding;
  const int bottom = content.y + content.height;
  int y = content.y;
  char buf[96];
  char a[24];
  const int maxWidth = content.x + content.width - metrics.contentSidePadding - x;
  const auto row = [&](const char* text, const int font, const EpdFontFamily::Style style) {
    y = drawWrapped(renderer, font, x, y, maxWidth, bottom, text, style);
  };

  if (haveGauge) {
    // Same threshold the boot-time capacity load uses (bq27220TooHigh): above it the gauge
    // is still on TI's ~3000 mAh default, so a health ratio would be meaningless.
    const bool uncalibrated = gauge.fullChargeMah > gauge.designMah + gauge.designMah / 4;
    if (uncalibrated) {
      row(tr(STR_BATTERY_HEALTH_UNKNOWN), UI_10_FONT_ID, EpdFontFamily::BOLD);
      snprintf(buf, sizeof(buf), tr(STR_BATTERY_UNCALIBRATED_FORMAT), static_cast<unsigned>(gauge.fullChargeMah),
               static_cast<unsigned>(gauge.designMah));
      row(buf, SMALL_FONT_ID, EpdFontFamily::REGULAR);
    } else {
      const unsigned health =
          std::min<unsigned>(100, static_cast<unsigned>(gauge.fullChargeMah) * 100 / gauge.designMah);
      snprintf(buf, sizeof(buf), tr(STR_BATTERY_HEALTH_FORMAT), health);
      row(buf, UI_10_FONT_ID, EpdFontFamily::BOLD);
      snprintf(buf, sizeof(buf), tr(STR_BATTERY_CAPACITY_FORMAT), static_cast<unsigned>(gauge.fullChargeMah),
               static_cast<unsigned>(gauge.designMah));
      row(buf, UI_10_FONT_ID, EpdFontFamily::REGULAR);
    }
    snprintf(buf, sizeof(buf), tr(STR_BATTERY_GAUGE_CYCLES_FORMAT), static_cast<unsigned>(gauge.cycles));
    row(buf, UI_10_FONT_ID, EpdFontFamily::REGULAR);
    // The boot-time capacity load writes Learned FCC = design, so an exact match means the
    // gauge has not re-learned it yet.
    if (gauge.fullChargeMah == gauge.designMah) row(tr(STR_BATTERY_CALIBRATING), SMALL_FONT_ID, EpdFontFamily::REGULAR);
  } else {
    row(tr(STR_BATTERY_NO_CAPACITY), UI_10_FONT_ID, EpdFontFamily::REGULAR);
  }
  y += metrics.verticalSpacing;

  if (!haveHeader || recordCount == 0) {
    row(tr(STR_BATTERY_NO_DATA), UI_10_FONT_ID, EpdFontFamily::REGULAR);
    return;
  }
  formatTenths(header.chargedPct / 10, a, sizeof(a));
  snprintf(buf, sizeof(buf), tr(STR_BATTERY_EQ_CYCLES_FORMAT), a);
  row(buf, UI_10_FONT_ID, EpdFontFamily::REGULAR);
  battery::formatSpan(header.awakeS, a, sizeof(a));
  snprintf(buf, sizeof(buf), tr(STR_BATTERY_TOTAL_AWAKE_FORMAT), a);
  row(buf, UI_10_FONT_ID, EpdFontFamily::REGULAR);
  if (header.firstEpoch != 0) {
    formatDate(header.firstEpoch, a, sizeof(a));
    snprintf(buf, sizeof(buf), tr(STR_BATTERY_LOGGED_SINCE_FORMAT), a);
    row(buf, UI_10_FONT_ID, EpdFontFamily::REGULAR);
  }
  snprintf(buf, sizeof(buf), tr(STR_BATTERY_SAMPLES_FORMAT), static_cast<unsigned long>(recordCount));
  row(buf, SMALL_FONT_ID, EpdFontFamily::REGULAR);
}
