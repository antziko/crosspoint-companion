#pragma once
// Host-test stub for src/CrossPointSettings.h (device-only). LookupHistory
// only reads the history cap; tests set it directly per-case.
class CrossPointSettings {
 public:
  static CrossPointSettings& getInstance() {
    static CrossPointSettings instance;
    return instance;
  }
  int lookupHistoryCap = 100;
  int getLookupHistoryCapValue() const { return lookupHistoryCap; }
};

#define SETTINGS CrossPointSettings::getInstance()
