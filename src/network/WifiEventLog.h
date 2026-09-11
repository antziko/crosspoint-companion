#pragma once

#include <cstdint>

// Always-on record of why the station last left the AP.
//
// arduino-esp32 maps only NO_AP_FOUND and a repeated AUTH_FAIL onto a wl_status_t
// callers can read (STA.cpp:137-148); every other reason leaves the status at
// WL_DISCONNECTED, so a drop mid-session is indistinguishable from a slow server.
// The core does log the reason, at log_w(), which is compiled out here because
// CORE_DEBUG_LEVEL is never defined in platformio.ini.
//
// Registered once at boot rather than per screen: a drop during an OPDS transfer or
// a KOSync round happens far from the Wi-Fi screen, and that is exactly when the
// reason matters -- every request after it burns two 7s DNS timeouts and a 15s
// link-down wait before anything says the radio is gone.
namespace WifiEventLog {

// Subscribe to STA disconnect events. Idempotent; call once from setup().
void begin();

// Reason code of the last disconnect (wifi_err_reason_t), or 0 if none since the
// last clear(). Written by the Network event task, read by UI tasks.
uint8_t lastDisconnectReason();

// Name for a reason code, for a UI message. Never null.
const char* reasonName(uint8_t reason);

// Forget the recorded reason, so a later failure cannot report a stale cause.
// Call when starting a fresh connection attempt.
void clear();

}  // namespace WifiEventLog
