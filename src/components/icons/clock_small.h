#pragma once
#include <cstdint>

// size: 16x16
// Inline clock face for status-bar / chrome time indicator. Drawn beside the time text to
// signal that the value is a real wall-clock time (synced from RTC or NTP).
//
// Source data is pre-rotated 90° CCW from the user-visible layout: drawIcon's transform
// (GfxRenderer.cpp:721) maps source-row-r to user_x = x_r + width - 1 - r and source-col-c
// to user_y = y_r + c, which is a 90° CW rotation. So bytes here are stored such that
// source's "top" appears at the user's "right" and source's "left" appears at the user's
// "top". Width must be a multiple of 8 (drawImageTransparent uses w/8 with integer truncation).
static const uint8_t ClockSmallIcon[] = {0xFC, 0x3F, 0xF0, 0x0F, 0xE7, 0xE7, 0xCF, 0xF3, 0x9F, 0xF9, 0xBE,
                                         0x7D, 0x3E, 0x7C, 0x30, 0x7C, 0x30, 0x7C, 0x3F, 0xFC, 0xBF, 0xFD,
                                         0x9F, 0xF9, 0xCF, 0xF3, 0xE7, 0xE7, 0xF0, 0x0F, 0xFC, 0x3F};
