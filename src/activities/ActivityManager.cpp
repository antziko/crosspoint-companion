#include "ActivityManager.h"

#include <FontCacheManager.h>
#include <HalDisplay.h>
#include <HalPowerManager.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "OpdsServerStore.h"
#include "boot_sleep/BootActivity.h"
#include "boot_sleep/SleepActivity.h"
#include "browser/OpdsBookBrowserActivity.h"
#include "home/CrashActivity.h"
#include "home/FileBrowserActivity.h"
#include "home/HomeActivity.h"
#include "home/RecentBooksActivity.h"
#include "network/CrossPointWebServerActivity.h"
#include "reader/ReaderActivity.h"
#include "reader/ReadingStatsActivity.h"
#include "settings/OpdsServerListActivity.h"
#include "settings/SettingsActivity.h"
#if FREEINK_CAP_FRONTLIGHT
#include "util/FrontlightPanelActivity.h"
#endif
#include "util/FullScreenMessageActivity.h"

static portMUX_TYPE activityManagerSpinlock = portMUX_INITIALIZER_UNLOCKED;

void ActivityManager::begin() {
#if defined(configNUM_CORES) && configNUM_CORES > 1
  constexpr BaseType_t renderTaskCore = 1;
#else
  constexpr BaseType_t renderTaskCore = 0;
#endif
  xTaskCreatePinnedToCore(&renderTaskTrampoline, "ActivityManagerRender",
                          // EPUB section indexing (createSectionFile: expat parse + block
                          // layout + hyphenation + text measurement) runs on this task and
                          // is a deep call chain. 8192 left almost no margin and could
                          // corrupt/crash on complex chapters; 12288 gives headroom.
                          12288,              // Stack size
                          this,               // Parameters
                          1,                  // Priority
                          &renderTaskHandle,  // Task handle
                          renderTaskCore  // Keep long renders/cover decodes off CPU 0's idle watchdog when available

  );
  assert(renderTaskHandle != nullptr && "Failed to create render task");
}

void ActivityManager::renderTaskTrampoline(void* param) {
  auto* self = static_cast<ActivityManager*>(param);
  self->renderTaskLoop();
}

void ActivityManager::renderTaskLoop() {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    // Acquire the lock before reading currentActivity to avoid a TOCTOU race
    // where the main task deletes the activity between the null-check and render().
    RenderLock lock;
    if (currentActivity) {
      HalPowerManager::Lock powerLock;  // Ensure we don't go into low-power mode while rendering
      // Night mode inverts only the reading surfaces (appliesNightMode): resolving the
      // output polarity here, per render, means menus, popups and every other activity
      // revert to normal automatically without touching the flag themselves.
      display.setInverted(SETTINGS.screenInverted != 0 && currentActivity->appliesNightMode());
      renderInProgress_ = true;
      currentActivity->render(std::move(lock));
      renderInProgress_ = false;
    }
    // Notify any task blocked in requestUpdateAndWait() that the render is done.
    TaskHandle_t waiter = nullptr;
    taskENTER_CRITICAL(&activityManagerSpinlock);
    waiter = waitingTaskHandle;
    waitingTaskHandle = nullptr;
    taskEXIT_CRITICAL(&activityManagerSpinlock);
    if (waiter) {
      xTaskNotify(waiter, 1, eIncrement);
    }
  }
}

void ActivityManager::loop() {
  if (currentActivity) {
    if (!currentActivity->isHomeActivity() && mappedInput.wasHomeGesture()) {
      if (currentActivity->handleHomeGesture()) {
        return;
      }
      goHome();
      return;
    }

#if FREEINK_CAP_FRONTLIGHT
    // Top-edge down-swipe opens the frontlight panel, ahead of activity input so
    // it works from every screen. Suppressed while the panel itself is up, where
    // the same edge would immediately reopen it.
    if (currentActivity->name != "FrontlightPanel" && mappedInput.wasLightPanelGesture()) {
      pushActivity(std::make_unique<FrontlightPanelActivity>(renderer, mappedInput));
      return;
    }
#endif

    // Note: do not hold a lock here, the loop() method must be responsible for acquire one if needed
    currentActivity->loop();
  }

  while (pendingAction != PendingAction::None) {
    if (pendingAction == PendingAction::Pop) {
      RenderLock lock;

      if (!currentActivity) {
        // Should never happen in practice
        LOG_ERR("ACT", "Pop set but currentActivity is null; ignoring pop request");
        pendingAction = PendingAction::None;
        continue;
      }

      ActivityResult pendingResult = std::move(currentActivity->result);

      // Destroy the current activity
      exitActivity(lock);
      pendingAction = PendingAction::None;

      if (stackActivities.empty()) {
        LOG_DBG("ACT", "No more activities on stack, going home");
        lock.unlock();  // goHome may acquire its own lock
        goHome();
        continue;  // Will launch goHome immediately

      } else {
        currentActivity = std::move(stackActivities.back());
        stackActivities.pop_back();
        LOG_DBG("ACT", "Popped from activity stack, new size = %zu", stackActivities.size());
        // Resume before the handler so a handler that immediately pushes again re-pauses cleanly.
        currentActivity->onResume();
        // Handle result if necessary
        if (currentActivity->resultHandler) {
          LOG_DBG("ACT", "Handling result for popped activity");

          // Move it here to avoid the case where handler calling another startActivityForResult()
          auto handler = std::move(currentActivity->resultHandler);
          currentActivity->resultHandler = nullptr;
          lock.unlock();  // Handler may acquire its own lock
          handler(pendingResult);
        }

        // Request an update to ensure the popped activity gets re-rendered
        if (pendingAction == PendingAction::None) {
          requestUpdate();
        }

        // Handler may request another pending action, we will handle it in the next loop iteration
        continue;
      }

    } else if (pendingActivity) {
      // Current activity has requested a new activity to be launched
      if (pendingAction == PendingAction::Replace) {
        // Replace needs lock because it calls onExit() which may render
        RenderLock lock;
        exitActivity(lock);
        while (!stackActivities.empty()) {
          stackActivities.back()->onExit();
          stackActivities.pop_back();
        }
        pendingAction = PendingAction::None;
        currentActivity = std::move(pendingActivity);
        lock.unlock();  // onEnter may acquire its own lock
        currentActivity->onEnter();
      } else if (pendingAction == PendingAction::Push) {
        // Push MUST hold RenderLock across the pause/swap. Without it, the
        // render task can be mid-render() on the outgoing activity while the
        // main task runs the incoming activity's onEnter() — both touch the
        // shared GfxRenderer + SdCardFont state (overflow ring, advance table,
        // fullIntervals) with no mutex, causing a use-after-free in
        // onGlyphMiss (intermittent blank-panic reboot). render(std::move(lock))
        // holds this mutex, so acquiring it here blocks until any in-flight
        // render completes (up to ~1s e-ink refresh) — the correctness cost
        // of the previous lock-free fast path.
        RenderLock lock;
        if (renderInProgress_) {
          // Tripwire: with the lock held the render task cannot be rendering.
          // If this ever fires the race window has reopened.
          LOG_ERR("ACT", "PUSH while renderInProgress=1 task=%p", xTaskGetCurrentTaskHandle());
        }
        currentActivity->onPause();
        stackActivities.push_back(std::move(currentActivity));
        LOG_DBG("ACT", "Pushed to activity stack, new size = %zu", stackActivities.size());
        pendingAction = PendingAction::None;
        currentActivity = std::move(pendingActivity);
        lock.unlock();  // onEnter may acquire its own lock / call requestUpdateAndWait
        currentActivity->onEnter();
      }

      // onEnter may request another pending action, we will handle it in the next loop iteration
      continue;
    }
  }

  if (requestedUpdate.exchange(false)) {
    // Using direct notification to signal the render task to update
    // Increment counter so multiple rapid calls won't be lost
    if (renderTaskHandle) {
      xTaskNotify(renderTaskHandle, 1, eIncrement);
    }
  }
}

void ActivityManager::exitActivity(const RenderLock& lock) {
  // Note: lock must be held by the caller
  if (currentActivity) {
    currentActivity->onExit();
    currentActivity.reset();
  }
}

void ActivityManager::replaceActivity(std::unique_ptr<Activity>&& newActivity) {
  // Note: no lock here, this is usually called by loop() and we may run into deadlock
  if (currentActivity) {
    // Defer launch if we're currently in an activity, to avoid deleting the current activity
    // leading to the "delete this" problem
    pendingActivity = std::move(newActivity);
    pendingAction = PendingAction::Replace;
  } else {
    // No current activity, safe to launch immediately
    currentActivity = std::move(newActivity);
    currentActivity->onEnter();
  }
}

// Every goTo* below replaces the current screen, so a failed allocation must not abort: with
// -fno-exceptions `std::make_unique` calls abort() on OOM and the device reboots mid-navigation.
// Constructing through this instead leaves the user on the screen they were already on, which is
// always a better outcome than a reboot and is usually recoverable (back out, free some heap,
// retry). The template adds nothing over the std::make_unique<T> it replaces — that was already
// instantiated per activity type here.
template <typename T, typename... Args>
bool ActivityManager::replaceActivityNoThrow(const char* what, Args&&... args) {
  auto activity = makeUniqueNoThrow<T>(std::forward<Args>(args)...);
  if (!activity) {
    LOG_ERR("ACT", "OOM allocating %s; staying on the current screen", what);
    return false;
  }
  replaceActivity(std::move(activity));
  return true;
}

void ActivityManager::goToFileTransfer() {
  replaceActivityNoThrow<CrossPointWebServerActivity>("CrossPointWebServer", renderer, mappedInput);
}

void ActivityManager::goToSettings(int initialCategory) {
  replaceActivityNoThrow<SettingsActivity>("Settings", renderer, mappedInput, initialCategory);
}

void ActivityManager::goToFileBrowser(std::string path) {
  replaceActivityNoThrow<FileBrowserActivity>("FileBrowser", renderer, mappedInput, std::move(path));
}

void ActivityManager::goToRecentBooks() {
  replaceActivityNoThrow<RecentBooksActivity>("RecentBooks", renderer, mappedInput);
}

void ActivityManager::goToReadingStats() {
  replaceActivityNoThrow<ReadingStatsActivity>("ReadingStats", renderer, mappedInput);
}

void ActivityManager::goToBrowser() {
  const auto& servers = OPDS_STORE.getServers();
  // Skip the server picker when there's only one server configured
  if (servers.size() == 1) {
    replaceActivityNoThrow<OpdsBookBrowserActivity>("OpdsBookBrowser", renderer, mappedInput, servers[0]);
  } else {
    replaceActivityNoThrow<OpdsServerListActivity>("OpdsServerList", renderer, mappedInput, true);
  }
}

void ActivityManager::goToReader(std::string path, const bool allowFastInitialRefresh) {
  replaceActivityNoThrow<ReaderActivity>("Reader", renderer, mappedInput, std::move(path), allowFastInitialRefresh);
}

void ActivityManager::goToSleep(bool fromTimeout) {
  replaceActivityNoThrow<SleepActivity>("Sleep", renderer, mappedInput, fromTimeout);
  loop();  // Important: sleep screen must be rendered immediately, the caller will go to sleep right after this returns
}

void ActivityManager::goToBoot() { replaceActivityNoThrow<BootActivity>("Boot", renderer, mappedInput); }

void ActivityManager::goToFullScreenMessage(std::string message, EpdFontFamily::Style style) {
  replaceActivityNoThrow<FullScreenMessageActivity>("FullScreenMessage", renderer, mappedInput, std::move(message),
                                                    style);
}

void ActivityManager::goHome(HomeMenuItem initialMenuItem) {
  if (initialMenuItem == HomeMenuItem::NONE && currentActivity) {
    const auto& activityName = currentActivity->name;
    if (activityName == "FileBrowser") {
      initialMenuItem = HomeMenuItem::FILE_BROWSER;
    } else if (activityName == "RecentBooks") {
      initialMenuItem = HomeMenuItem::RECENTS;
    } else if (activityName == "OpdsBookBrowser") {
      initialMenuItem = HomeMenuItem::OPDS_BROWSER;
    } else if (activityName == "ReadingStats") {
      initialMenuItem = HomeMenuItem::READING_STATS;
    } else if (activityName == "CrossPointWebServer") {
      initialMenuItem = HomeMenuItem::FILE_TRANSFER;
    } else if (activityName == "Settings") {
      initialMenuItem = HomeMenuItem::SETTINGS_MENU;
    }
  }
  replaceActivityNoThrow<HomeActivity>("Home", renderer, mappedInput, initialMenuItem);
}
void ActivityManager::goToCrashReport() { replaceActivityNoThrow<CrashActivity>("Crash", renderer, mappedInput); }

void ActivityManager::pushActivity(std::unique_ptr<Activity>&& activity) {
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while pushActivity is not expected");
    pendingActivity.reset();
  }
  pendingActivity = std::move(activity);
  pendingAction = PendingAction::Push;
}

void ActivityManager::popActivity() {
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while popActivity is not expected");
    pendingActivity.reset();
  }
  pendingAction = PendingAction::Pop;
}

bool ActivityManager::preventAutoSleep() const { return currentActivity && currentActivity->preventAutoSleep(); }

bool ActivityManager::onManualSleepRequested() { return currentActivity && currentActivity->onManualSleepRequested(); }

void ActivityManager::notifyFramebufferInvalidated() {
  if (currentActivity) currentActivity->onFramebufferInvalidated();
}

bool ActivityManager::isReaderActivity() const {
  return std::any_of(stackActivities.begin(), stackActivities.end(),
                     [](const auto& activity) { return activity->isReaderActivity(); }) ||
         (currentActivity && currentActivity->isReaderActivity());
}

bool ActivityManager::isInReaderContext() const {
  if (currentActivity && currentActivity->isReaderActivity()) {
    return true;
  }
  return std::any_of(stackActivities.begin(), stackActivities.end(),
                     [](const auto& activity) { return activity && activity->isReaderActivity(); });
}

bool ActivityManager::skipLoopDelay() const { return currentActivity && currentActivity->skipLoopDelay(); }

ScreenshotInfo ActivityManager::getScreenshotInfo() const {
  if (currentActivity) {
    return currentActivity->getScreenshotInfo();
  }
  return {};
}

void ActivityManager::requestUpdate(bool immediate) {
  if (immediate) {
    if (renderTaskHandle) {
      xTaskNotify(renderTaskHandle, 1, eIncrement);
    }
  } else {
    // Deferring the update until current loop is finished
    // This is to avoid multiple updates being requested in the same loop
    requestedUpdate = true;
  }
}
void ActivityManager::requestUpdateAndWait() {
  if (!renderTaskHandle) {
    return;
  }

  // Atomic section to perform checks
  taskENTER_CRITICAL(&activityManagerSpinlock);
  auto currTaskHandler = xTaskGetCurrentTaskHandle();
  auto mutexHolder = xSemaphoreGetMutexHolder(renderingMutex);
  bool isRenderTask = (currTaskHandler == renderTaskHandle);
  bool alreadyWaiting = (waitingTaskHandle != nullptr);
  bool holdingRenderLock = (mutexHolder == currTaskHandler);
  if (!alreadyWaiting && !isRenderTask && !holdingRenderLock) {
    waitingTaskHandle = currTaskHandler;
  }
  taskEXIT_CRITICAL(&activityManagerSpinlock);

  // Render task cannot call requestUpdateAndWait() or it will cause a deadlock
  assert(!isRenderTask && "Render task cannot call requestUpdateAndWait()");

  // There should never be the case where 2 tasks are waiting for a render at the same time
  assert(!alreadyWaiting && "Already waiting for a render to complete");

  // Cannot call while holding RenderLock or it will cause a deadlock
  assert(!holdingRenderLock && "Cannot call requestUpdateAndWait() while holding RenderLock");

  xTaskNotify(renderTaskHandle, 1, eIncrement);
  // Tell the power manager the loop is parked here: it cannot poll input until the
  // render finishes, so the BUSY-wait slice hook should not yield to it meanwhile.
  powerManager.noteRenderWaitBegin();
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  powerManager.noteRenderWaitEnd();
}

// RenderLock

RenderLock::RenderLock() {
  xSemaphoreTake(activityManager.renderingMutex, portMAX_DELAY);
  isLocked = true;
}

RenderLock::RenderLock([[maybe_unused]] Activity&) {
  xSemaphoreTake(activityManager.renderingMutex, portMAX_DELAY);
  isLocked = true;
}

RenderLock::~RenderLock() {
  if (isLocked) {
    xSemaphoreGive(activityManager.renderingMutex);
    isLocked = false;
  }
}

void RenderLock::unlock() {
  if (isLocked) {
    xSemaphoreGive(activityManager.renderingMutex);
    isLocked = false;
  }
}

/**
 *
 * Checks if renderingMutex is busy.
 *
 * @return true if renderingMutex is busy, otherwise false.
 *
 */
bool RenderLock::peek() { return xQueuePeek(activityManager.renderingMutex, NULL, 0) != pdTRUE; };
