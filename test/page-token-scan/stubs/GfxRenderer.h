#pragma once
// Host-test stub for lib/GfxRenderer/GfxRenderer.h.
// Only the surface PageTokenScan.cpp touches: one advance query, with a width that is a
// deterministic function of the byte length so a test can assert what was measured.

#include <cstddef>
#include <cstdint>
#include <cstring>

class GfxRenderer {
 public:
  int getTextAdvanceX(int /*fontId*/, const char* text, uint8_t /*style*/ = 0) const {
    lastMeasured = text ? text : "";
    return text ? static_cast<int>(std::strlen(text)) : 0;
  }

  mutable const char* lastMeasured = "";
};
