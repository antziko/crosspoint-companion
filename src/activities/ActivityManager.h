#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <cassert>
#include <memory>
#include <string>
#include <vector>

#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "util/ScreenshotInfo.h"

class Activity;    // forward declaration
class RenderLock;  // forward declaration

enum class HomeMenuItem { NONE, FILE_BROWSER, RECENTS, OPDS_BROWSER, READING_STATS, FILE_TRANSFER, SETTINGS_MENU };

/**
 * ActivityManager
 *
 * This mirrors the same concept of Activity in Android, where an activity represents a single screen of the UI. The
 * manager is responsible for launching activities, and ensuring that only one activity is active at a time.
 *
 * It also provides a stack mechanism to allow activities to launch sub-activities and get back the results when the
 * sub-activity is done. For example, the WebServer activity can launch a WifiSelect activity to let the user choose a
 * wifi network, and get back the selected network when the user is done.
 *
 * Main differences from Android's ActivityManager:
 * - No onPause/onResume, since we don't have a concept of background activities
 * - onActivityResult is implemented via a callback instead of a separate method, for simplicity
 */
class ActivityManager {
  friend class RenderLock;

 protected:
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;
  std::vector<std::unique_ptr<Activity>> stackActivities;
  std::unique_ptr<Activity> currentActivity;

  void exitActivity(const RenderLock& lock);

  // Pending activity to be launched on next loop iteration
  std::unique_ptr<Activity> pendingActivity;
  enum class PendingAction { None, Push, Pop, Replace };
  PendingAction pendingAction = PendingAction::None;

  // Task to render and display the activity
  TaskHandle_t renderTaskHandle = nullptr;
  static void renderTaskTrampoline(void* param);
  [[noreturn]] virtual void renderTaskLoop();

  // Set by requestUpdateAndWait(); read and cleared by the render task after render completes.
  // Note: only one waiting task is supported at a time
  TaskHandle_t waitingTaskHandle = nullptr;

  // Mutex to protect rendering operations from race conditions
  // Must only be used via RenderLock
  SemaphoreHandle_t renderingMutex = nullptr;

  // Whether to trigger a render after the current loop()
  // This variable must only be set by the main loop, to avoid race conditions
  std::atomic<bool> requestedUpdate{false};

  // Set true by the render task only while currentActivity->render() runs.
  // Read by the Push path as a race tripwire (must be false there now that
  // Push holds RenderLock). volatile: written by render task, read by main.
  volatile bool renderInProgress_ = false;

 public:
  explicit ActivityManager(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : renderer(renderer), mappedInput(mappedInput), renderingMutex(xSemaphoreCreateMutex()) {
    assert(renderingMutex != nullptr && "Failed to create rendering mutex");
    stackActivities.reserve(10);
  }
  ~ActivityManager() { assert(false); /* should never be called */ };

  void begin();
  void loop();

  // Will replace currentActivity and drop all activities on stack
  void replaceActivity(std::unique_ptr<Activity>&& newActivity);

  // Allocating form of the above that degrades instead of aborting on OOM; every goTo* below
  // uses it. Definition (and the reasoning) live beside those in ActivityManager.cpp — it is
  // only ever instantiated there, so it does not need to be visible to other translation units.
  template <typename T, typename... Args>
  bool replaceActivityNoThrow(const char* what, Args&&... args);

  // goTo... functions are convenient wrapper for replaceActivity()
  void goToFileTransfer();
  void goToSettings(int initialCategory = 0);
  void goToFileBrowser(std::string path = {});
  void goToRecentBooks();
  void goToReadingStats();
  void goToBrowser();
  // allowFastInitialRefresh defaults true: an ordinary reader entry (book open
  // from the browser/home, end-of-book "open next") repaints over UI that a
  // fast/normal first paint clears well enough, so it must NOT flash on every
  // open. Callers that land the reader over a full-screen frame a fast diff can't
  // clear -- wake-from-sleep and KOReader sync return -- pass false to force the
  // initial HALF scrub (see ReaderActivity::initialRefreshCountdown). Cold boot is
  // handled explicitly in main.cpp via allowFastInitialReaderRefresh.
  void goToReader(std::string path, bool allowFastInitialRefresh = true);
  void goToSleep(bool fromTimeout = false);
  void goToBoot();
  void goToFullScreenMessage(std::string message, EpdFontFamily::Style style = EpdFontFamily::REGULAR);
  void goToCrashReport();
  // cleanInitialRefresh forces Home's first paint to HALF instead of the FAST default, for
  // the one caller that lands Home over a retained sleep image (splashless wake, no frame
  // file). The reader equivalent is goToReader's allowFastInitialRefresh=false.
  void goHome(HomeMenuItem initialMenuItem = HomeMenuItem::NONE, bool cleanInitialRefresh = false);

  // This will move current activity to stack instead of deleting it
  void pushActivity(std::unique_ptr<Activity>&& activity);

  // Remove the currentActivity, returning the last one on stack
  // Note: if popActivity() on last activity on the stack, we will goHome()
  void popActivity();

  bool preventAutoSleep() const;
  // Offer a *manual* sleep gesture to the active activity. Returns true if the activity
  // took it over (e.g. the reader showed a "sync before sleep" prompt), meaning the main
  // loop must NOT deep-sleep this iteration. Default activities return false → sleep proceeds.
  bool onManualSleepRequested();
  // Tell the active activity that the framebuffer no longer holds what it drew. Call between
  // wiping the framebuffer outside a render and asking for the re-render, so activities that
  // repaint differentially drop their stale state first.
  void notifyFramebufferInvalidated();
  bool isReaderActivity() const;
  bool isInReaderContext() const;
  bool skipLoopDelay() const;
  ScreenshotInfo getScreenshotInfo() const;

  // If immediate is true, the update will be triggered immediately.
  // Otherwise, it will be deferred until the end of the current loop iteration.
  void requestUpdate(bool immediate = false);

  // Trigger a render and block until it completes.
  // Must NOT be called from the render task or while holding a RenderLock.
  void requestUpdateAndWait();
};

extern ActivityManager activityManager;  // singleton, to be defined in main.cpp
