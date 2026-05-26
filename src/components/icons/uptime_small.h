#pragma once
#include <cstdint>

// size: 16x16
// Play triangle for inline status-bar / chrome use beside the session-uptime readout.
// Drawn instead of the clock face when the time hasn't yet synced.
//
// Source data is pre-rotated 90° CCW from the user-visible layout (see clock_small.h for
// details). The bytes below visualize as an UP-pointing triangle in source storage but
// render as a RIGHT-pointing play triangle in the user's view after drawIcon's 90° CW
// rotation. Width must be a multiple of 8.
static const uint8_t UptimeSmallIcon[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0xFE, 0x7F, 0xFE, 0x3F, 0xFC,
                                          0x3F, 0xFC, 0x1F, 0xF8, 0x1F, 0xF8, 0x0F, 0xF0, 0x0F, 0xF0, 0x07,
                                          0xE0, 0x07, 0xE0, 0x03, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
