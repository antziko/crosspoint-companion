#include "Activity.h"

#include <Arduino.h>
#include <SdDebugLog.h>
#include <esp_heap_caps.h>

#include "ActivityManager.h"
#include "components/ListCursor.h"

namespace {
// Heap profiling. onEnter() runs before an activity allocates (baseline), onExit()
// runs after it frees (should return to ~baseline). Diffing consecutive lines shows
// each activity's footprint; a downward free-heap drift across enter/exit cycles
// flags a leak. Mirrored to SD (/opds_debug.txt) for untethered X3 capture.
void logHeap(const char* phase, const char* name) {
#ifdef TRACE_HEAP
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  const uint32_t minEver = ESP.getMinFreeHeap();
  LOG_DBG("MEM", "%s %-22s free=%u largest=%u minEver=%u", phase, name, freeHeap, largest, minEver);
  SdDebugLog::setEnabled(true);
  SdDebugLog::log("MEM", "%s %s free=%u largest=%u minEver=%u", phase, name, freeHeap, largest, minEver);
#else
  (void)phase;
  (void)name;
#endif
}
}  // namespace

void Activity::onEnter() {
  LOG_DBG("ACT", "Entering activity: %s", name.c_str());
  // A freshly opened screen has a selection only because the buttons need one. Withhold its
  // highlight until the user navigates (see ListCursor); a list that opens positioned on a
  // later row is unaffected.
  ListCursor::hide();
  logHeap("enter", name.c_str());
}

void Activity::onExit() {
  LOG_DBG("ACT", "Exiting activity: %s", name.c_str());
  logHeap("exit ", name.c_str());
}

void Activity::requestUpdate(bool immediate) { activityManager.requestUpdate(immediate); }

void Activity::requestUpdateAndWait() { activityManager.requestUpdateAndWait(); }

void Activity::onGoHome(HomeMenuItem item) { activityManager.goHome(item); }

void Activity::onSelectBook(const std::string& path) { activityManager.goToReader(path); }

void Activity::startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler) {
  this->resultHandler = std::move(resultHandler);
  activityManager.pushActivity(std::move(activity));
}

void Activity::setResult(ActivityResult&& result) { this->result = std::move(result); }

void Activity::finish() { activityManager.popActivity(); }

Activity::ListTouchResult Activity::handleListTouch(int& selectedIndex, const int itemCount, const int listTop,
                                                    const int listHeight, const bool hasSubtitle) {
  int touched = -1;
  if (mappedInput.wasListItemTouchedDown(touched, itemCount, selectedIndex, listTop, listHeight, hasSubtitle)) {
    if (selectedIndex != touched) {
      selectedIndex = touched;
      requestUpdate();
    }
    return ListTouchResult::Consumed;
  }
  if (mappedInput.wasListItemTapped(touched, itemCount, selectedIndex, listTop, listHeight, hasSubtitle)) {
    selectedIndex = touched;
    return ListTouchResult::Activated;
  }
  return ListTouchResult::None;
}
