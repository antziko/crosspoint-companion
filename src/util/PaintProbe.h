#pragma once

#include <Arduino.h>  // millis()
#include <GfxRenderer.h>
#include <SdCardFont.h>
#include <SdDebugLog.h>
#include <esp_heap_caps.h>

#include <cstdint>

// Times one screen paint and attributes its SD-font work, mirrored to SD because the X3 is read
// untethered. Construct before drawing, call markDrawn() before displayBuffer() and log() after.
//
// Line 1: `<tag>: <name> draw= display= measures= miss= missMs= free= largest=`. measures =
// getTextWidth() calls, the CPU-side cost of fitting text (truncation, wrapping). display= is the panel
// floor (~480ms FAST on X3), the same for every screen; miss/missMs are glyphs read from SD one
// at a time (~3ms each).
// Line 2, per SD font that did any font work: why those reads happened. ioFail = SD read failed
// (never cached, retried every draw); hit/evict/ring/budget = the overflow ring, which should
// serve a repeat; rebuild = full mini SD passes (~85-120ms each); guardSkip = rebuilds the
// trimmed-mini guard avoided; recent = the last four codepoints read.
class PaintProbe {
 public:
  explicit PaintProbe(const GfxRenderer& renderer)
      : renderer_(renderer), start_(millis()), measuresBefore_(renderer.textWidthCalls()) {
    int i = 0;
    for (const auto& [fontId, font] : renderer_.getSdCardFonts()) {
      if (i >= MAX_FONTS) break;
      if (font) before_[i] = Counters::of(font->getStats());
      i++;
    }
  }

  void markDrawn() { drawn_ = millis(); }

  void log(const char* tag, const char* name) const {
    const unsigned long now = millis();
    const unsigned long drawn = drawn_ ? drawn_ : now;
    uint32_t miss = 0, missMs = 0;
    int i = 0;
    for (const auto& [fontId, font] : renderer_.getSdCardFonts()) {
      if (i >= MAX_FONTS) break;
      const Counters& b = before_[i++];
      if (!font) continue;
      const Counters a = Counters::of(font->getStats());
      miss += delta(a.miss, b.miss);
      missMs += delta(a.missMs, b.missMs);
    }
    SdDebugLog::setEnabled(true);
    SdDebugLog::log(tag, "%s draw=%lu display=%lu measures=%u miss=%u missMs=%u free=%u largest=%u", name,
                    drawn - start_, now - drawn, static_cast<unsigned>(renderer_.textWidthCalls() - measuresBefore_),
                    static_cast<unsigned>(miss), static_cast<unsigned>(missMs),
                    static_cast<unsigned>(esp_get_free_heap_size()),
                    static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));

    i = 0;
    for (const auto& [fontId, font] : renderer_.getSdCardFonts()) {
      if (i >= MAX_FONTS) break;
      const Counters& b = before_[i++];
      if (!font) continue;
      const SdCardFont::Stats& st = font->getStats();
      const Counters a = Counters::of(st);
      const uint32_t fMiss = delta(a.miss, b.miss);
      const uint32_t rebuild = delta(a.rebuild, b.rebuild);
      const uint32_t skip = delta(a.skip, b.skip);
      const uint32_t evict = delta(a.evict, b.evict);
      const uint32_t clear = delta(a.clear, b.clear);
      if (fMiss == 0 && rebuild == 0 && skip == 0 && evict == 0 && clear == 0) continue;
      constexpr uint8_t N = SdCardFont::Stats::RECENT_MISSES;
      const uint8_t h = st.recentMissHead;  // oldest of the recent ring
      SdDebugLog::log(tag,
                      "  font=%d miss=%u ioFail=%u oom=%u hit=%u evict=%u clear=%u ring=%u/%uB budget=%u "
                      "rebuild=%u guardSkip=%u style=%u recent=U+%04X U+%04X U+%04X U+%04X file=%s",
                      fontId, static_cast<unsigned>(fMiss), static_cast<unsigned>(delta(a.ioFail, b.ioFail)),
                      static_cast<unsigned>(delta(a.oom, b.oom)), static_cast<unsigned>(delta(a.hit, b.hit)),
                      static_cast<unsigned>(evict), static_cast<unsigned>(clear),
                      static_cast<unsigned>(font->overflowCount()), static_cast<unsigned>(font->overflowBytesUsed()),
                      static_cast<unsigned>(font->overflowBudgetNow()), static_cast<unsigned>(rebuild),
                      static_cast<unsigned>(skip), st.lastMissStyle, static_cast<unsigned>(st.recentMissCps[h % N]),
                      static_cast<unsigned>(st.recentMissCps[(h + 1) % N]),
                      static_cast<unsigned>(st.recentMissCps[(h + 2) % N]),
                      static_cast<unsigned>(st.recentMissCps[(h + 3) % N]), font->filePath());
    }
  }

 private:
  // Only the counters a paint diffs, so the snapshot stays small on the render task's stack
  // (6 fonts x 36 B) rather than holding whole Stats structs.
  struct Counters {
    uint32_t miss, missMs, ioFail, oom, hit, evict, clear, rebuild, skip;
    static Counters of(const SdCardFont::Stats& s) {
      return {s.overflowMisses,    s.overflowMissMs, s.overflowIoFails, s.bitmapOom,     s.overflowHits,
              s.overflowEvictions, s.overflowClears, s.miniRebuilds,    s.miniGuardSkips};
    }
  };
  static constexpr int MAX_FONTS = 6;
  // The reader's PrewarmScope resets the stats; a counter below its snapshot means a reset
  // happened mid-paint, so report the raw value rather than a wrapped delta.
  static uint32_t delta(uint32_t now, uint32_t then) { return now >= then ? now - then : now; }

  const GfxRenderer& renderer_;
  const unsigned long start_;
  const uint32_t measuresBefore_;
  unsigned long drawn_ = 0;
  Counters before_[MAX_FONTS] = {};
};
