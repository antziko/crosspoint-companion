#pragma once

#include <cstdint>

#include "BatteryHistory.h"

// Per-board battery history on SD (/.crosspoint/battery/<board>.bin; format in
// BatteryHistory.h). The device records nothing while asleep, so samples are taken at
// wake, every SAMPLE_INTERVAL_S while awake, on USB plug/unplug and at sleep entry.
// Everything runs on the main loop task, so the sampler and the Battery screen never
// touch the file concurrently.
namespace BatteryLog {

constexpr uint32_t SAMPLE_INTERVAL_S = 600;
// If no battery reading succeeds this long after boot, the wake sample is written with
// an unknown level so the session still shows up.
constexpr uint32_t WAKE_READ_GRACE_S = 60;

// Note what the device is doing; call every loop iteration. Cheap, no I/O.
void noteActivity(bool reading, bool wifi);

// True when tick() has a sample to write: the caller then takes the render lock (the
// gauge shares I2C with the themes' battery reads) and calls tick().
bool due(bool usb);
void tick(bool usb);

// Final sample before deep sleep. Call before storage is torn down.
void onSleep(bool usb);

// Reader side for the Battery screen.
bool readHeader(battery::Header& out);
// Calls fn for every valid record, oldest first. False when the file can't be opened.
bool forEach(void (*fn)(const battery::Record& r, void* ctx), void* ctx);

}  // namespace BatteryLog
