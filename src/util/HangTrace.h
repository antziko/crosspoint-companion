#pragma once

#include <cstdint>

// Last-step breadcrumbs for silent hangs: no panic, no watchdog (the idle-task check is off), no
// crash report. Each task writes the step it is on into RTC memory, which survives a reset, and
// the next boot logs what both were doing. One store per mark, no SD, no lock.
namespace HangTrace {

enum Task : uint8_t { Loop = 0, Render = 1 };

// Steps. Loop: main loop and word-select input. Render: render task and word-select render.
enum Step : uint16_t {
  LoopTop = 1,
  LoopActivity = 2,  // inside activityManager.loop()
  LoopIdle = 3,      // past activityManager.loop()
  DwsLoopNav = 10,   // word select: handleNavigation
  DwsLoopAuto = 11,  // word select: auto range settle/select
  DwsLoopRest = 12,  // word select: touch/confirm/back handling
  RenderWait = 20,   // render task idle, waiting for a request
  RenderRun = 21,    // render task inside an activity's render()
  DwsGloss = 30,     // word select render: updateGloss (SD peek)
  DwsDiff = 31,      // differential path
  DwsDiffDisplay = 32,
  DwsClear = 33,      // full path: clearScreen + controller overlay
  DwsPrewarm = 34,    // full path: scan + prewarm
  DwsPage = 35,       // full path: page->render
  DwsMarks = 36,      // full path: drawPageMarks
  DwsHighlight = 37,  // full path: highlight
  DwsGlossDraw = 38,  // full path: drawGloss + hints
  DwsDisplay = 39,    // full path: displayBuffer
  DwsDone = 40,
};

void mark(Task task, Step step);
// Log the previous boot's breadcrumbs (once, after SD logging is up) and clear them.
void reportPreviousBoot();

}  // namespace HangTrace
