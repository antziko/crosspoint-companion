#pragma once

#include <HalPowerManager.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "activities/Activity.h"
#include "util/BatteryHistory.h"

struct Rect;
struct TabInfo;

// Settings > Battery: level graph, usage since the last charge, and health, all read
// from BatteryLog's per-board history. Everything is computed once on entry (and on a
// range change), never per frame.
class BatteryActivity final : public Activity {
 public:
  explicit BatteryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Tab : uint8_t { Level, Usage, Health };
  enum class Range : uint8_t { Day, Week, Month };
  static constexpr Tab TAB_ORDER[] = {Tab::Level, Tab::Usage, Tab::Health};
  static constexpr int TAB_COUNT = 3;

  Tab tab = Tab::Level;
  Range range = Range::Day;
  Range savedRange = Range::Day;

  // Snapshot taken on entry.
  battery::Header header;
  bool haveHeader = false;
  uint32_t nowEpoch = 0;  // 0 = no clock
  uint16_t nowSoc = 0;
  bool nowSocOk = false;
  uint16_t nowMv = 0;
  bool usbNow = false;
  HalPowerManager::GaugeHealth gauge;
  bool haveGauge = false;

  // Usage tab.
  battery::Usage sinceCharge;
  battery::Usage lastWeek;
  bool haveWeek = false;
  bool chargeFound = false;
  bool chargingNow = false;
  uint32_t chargeEndEpoch = 0;
  uint8_t chargeEndSoc = battery::SOC_UNKNOWN;
  uint32_t recordCount = 0;

  // Level tab: one bucket per plot column, sized on entry from the plot width.
  std::unique_ptr<battery::Bucket[]> buckets;
  int bucketCount = 0;
  bool sessionMode = false;
  bool graphEmpty = true;
  uint32_t graphT0 = 0;  // dated window [graphT0, graphT1)
  uint32_t graphT1 = 0;

  void buildStats();
  void buildGraph();
  void selectTab(int index);
  void cycleRange();

  Rect summaryRect() const;
  Rect tabBarRect() const;
  Rect contentRect() const;
  Rect plotRect(const Rect& content) const;
  std::vector<TabInfo> buildTabs() const;

  void renderLevel(const Rect& content) const;
  // Dated mode: local-time gridlines and labels (hours for 24 h, days for 7/30 days).
  void renderTimeAxis(const Rect& plot, int labelY) const;
  void renderUsage(const Rect& content) const;
  void renderHealth(const Rect& content) const;
  // Draws one usage block (title + rows) from y, stopping above `bottom`; returns the next free y.
  int renderUsageBlock(const char* title, const battery::Usage& u, int x, int y, int bottom) const;
};
