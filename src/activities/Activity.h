#pragma once
#include <Logging.h>
#include <Memory.h>  // makeUniqueNoThrow, for startActivityForResultNoThrow

#include <cassert>
#include <memory>
#include <string>
#include <utility>

#include "ActivityManager.h"  // for using the ActivityManager singleton
#include "ActivityResult.h"
#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "RenderLock.h"
#include "util/ScreenshotInfo.h"

class Activity {
  friend class ActivityManager;

 protected:
  std::string name;
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;

  ActivityResultHandler resultHandler;
  ActivityResult result;

 public:
  explicit Activity(std::string name, GfxRenderer& renderer, MappedInputManager& mappedInput)
      : name(std::move(name)), renderer(renderer), mappedInput(mappedInput) {}
  virtual ~Activity() = default;
  virtual void onEnter();
  virtual void onExit();
  // Called when this activity is suspended (another activity is pushed over it on the
  // stack) and when it is resumed (the pushed activity pops). Default no-op. The reader
  // uses these to freeze its wall-clock reading-time while a sub-screen is foreground.
  virtual void onPause() {}
  virtual void onResume() {}
  // Called when something outside render() has wiped or overwritten the framebuffer — the
  // manual screen refresh in main.cpp, which blanks it and then asks for a re-render.
  // Activities carrying incremental framebuffer state across renders (snapshots, dirty rects,
  // "the page is already drawn" flags) MUST drop it here, or the next render restores pixels
  // that are no longer on screen. The default no-op is correct for any activity whose render()
  // repaints from scratch.
  virtual void onFramebufferInvalidated() {}
  virtual void loop() {}

  virtual void render(RenderLock&&) {}

  // If immediate is true, the update will be triggered immediately.
  // Otherwise, it will be deferred until the end of the current loop iteration.
  virtual void requestUpdate(bool immediate = false);

  // Request an immediate render and block until it completes.
  virtual void requestUpdateAndWait();

  virtual bool skipLoopDelay() { return false; }
  virtual bool preventAutoSleep() { return false; }
  // Called by the main loop when the user makes a *manual* power-button sleep gesture
  // (never on auto-sleep timeout). Return true to take over the gesture and abort the
  // sleep (e.g. show a confirmation prompt); false to let the main loop deep-sleep.
  virtual bool onManualSleepRequested() { return false; }
  virtual bool isReaderActivity() const { return false; }
  // True for the reading surfaces night mode inverts (EPUB/TXT/XTC and the dictionary
  // overlays drawn over them). Resolved per render by ActivityManager, so menus,
  // popups and every other screen keep normal polarity without managing the flag.
  virtual bool appliesNightMode() const { return false; }
  virtual bool isHomeActivity() const { return false; }
  // True for screens that keep their header band even when the user has turned the top bar off
  // outside Home. Home always keeps it (it IS the top bar); override elsewhere only when the
  // header carries content rather than chrome, as the dictionary definition's headword does.
  virtual bool keepsTopBar() const { return isHomeActivity(); }
  virtual bool handleHomeGesture() { return false; }
  virtual ScreenshotInfo getScreenshotInfo() const { return {}; }

  // Start a new activity without destroying the current one
  // Note: requestUpdate() will be invoked automatically once resultHandler finishes
  void startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler);

  // As above, but allocates the activity itself and tolerates failure.
  //
  // `std::make_unique` is throwing `new`: with -fno-exceptions an OOM calls abort() and the
  // device reboots. That is not theoretical — an X3 rebooted here on every Chinese dictionary
  // lookup, pushing a 4844-byte DictionaryDefinitionActivity onto a heap whose largest block
  // the glyph prewarm had just taken down to 3444 bytes.
  //
  // Returns false (having logged) instead, leaving the caller on its current screen. Callers
  // that need a different fallback should check the return value; most simply want "nothing
  // happened", which is already far better than a reboot.
  //
  // The template costs nothing over what it replaces: `std::make_unique<T>` was already
  // instantiated per activity type at each of these call sites.
  template <typename T, typename... Args>
  bool startActivityForResultNoThrow(ActivityResultHandler resultHandler, Args&&... args) {
    auto activity = makeUniqueNoThrow<T>(std::forward<Args>(args)...);
    if (!activity) {
      LOG_ERR("ACT", "OOM allocating activity; staying put");
      return false;
    }
    startActivityForResult(std::move(activity), std::move(resultHandler));
    return true;
  }

  // Set the result to be passed back to the previous activity when this activity finishes
  void setResult(ActivityResult&& result);

  // Finish this activity and return to the previous one on the stack (if any)
  void finish();

  // Convenience method to facilitate API transition to ActivityManager
  // TODO: remove this in near future
  void onGoHome(HomeMenuItem item = HomeMenuItem::NONE);
  void onSelectBook(const std::string& path);

 protected:
  enum class ListTouchResult : uint8_t {
    None,      // touch did not hit the list
    Consumed,  // touchdown moved the highlight (repaint already requested)
    Activated  // tap landed on a row: selectedIndex is updated, caller activates it
  };

  // Shared touch handling for selectable list screens: touchdown highlights the
  // touched row, a tap selects and reports Activated. The caller supplies the
  // list band and runs its own activate action on Activated.
  ListTouchResult handleListTouch(int& selectedIndex, int itemCount, int listTop, int listHeight, bool hasSubtitle);
};
