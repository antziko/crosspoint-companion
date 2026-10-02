#include "PanelDoseTrace.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <SdDebugLog.h>

#include <cstdio>

namespace {
uint16_t nowSec() { return static_cast<uint16_t>(millis() / 1000); }
}  // namespace

bool PanelDoseTrace::ensureTable() {
  // The master switch reads true until settings load, so a boot paint may have allocated the
  // table for a user who has SD logging off: hand it back as soon as that is known.
  if (!SdDebugLog::isMasterEnabled()) {
    tiles.reset();
    return false;
  }
  if (tiles) return true;
  if (allocFailed) return false;
  tiles = makeUniqueNoThrow<Tile[]>(GRID * GRID);
  if (!tiles) {
    allocFailed = true;  // diagnostics only: never retry into a tight heap
    LOG_ERR("GFX", "OOM: dose trace %u bytes", static_cast<unsigned>(sizeof(Tile) * GRID * GRID));
    return false;
  }
  memset(tiles.get(), 0, sizeof(Tile) * GRID * GRID);
  return true;
}

void PanelDoseTrace::noteGlobal(const Kind kind) {
  if (kind == Fast) {
    totalFast++;
  } else if (kind < KIND_COUNT) {
    total[kind]++;
  }
}

void PanelDoseTrace::notePaint(const uint8_t* fb, const uint16_t widthBytes, const uint16_t height, const Kind kind,
                               const unsigned long railsMs) {
  noteGlobal(kind);
  if (!fb || widthBytes < GRID || height < GRID || !ensureTable()) return;
  const uint16_t sec = nowSec();
  const auto railsSec = static_cast<uint16_t>(railsMs / 1000);
  for (int tr = 0; tr < GRID; tr++) {
    const int y0 = tr * height / GRID;
    const int y1 = (tr + 1) * height / GRID;
    for (int tc = 0; tc < GRID; tc++) {
      const int x0 = tc * widthBytes / GRID;
      const int x1 = (tc + 1) * widthBytes / GRID;
      uint32_t h = 2166136261u;
      uint32_t blackBits = 0;
      for (int y = y0; y < y1; y++) {
        const uint8_t* row = fb + static_cast<size_t>(y) * widthBytes;
        for (int x = x0; x < x1; x++) {
          h = (h ^ row[x]) * 16777619u;
          blackBits += __builtin_popcount(static_cast<uint8_t>(~row[x]));  // 0 bits are black
        }
      }
      const auto folded = static_cast<uint16_t>(h ^ (h >> 16));
      Tile& t = tiles[tr * GRID + tc];
      if (t.hash == folded && t.secAt != 0) continue;  // held: keeps accumulating
      t.hash = folded;
      t.fastAt = totalFast;
      for (int k = 0; k < KIND_COUNT; k++) t.at[k] = total[k];
      t.secAt = sec ? sec : 1;  // 0 marks a never-painted tile
      t.railsSecAt = railsSec;
      const uint32_t bits = static_cast<uint32_t>(y1 - y0) * (x1 - x0) * 8;
      t.inkPct = bits ? static_cast<uint8_t>(blackBits * 100 / bits) : 0;
    }
  }
}

void PanelDoseTrace::dump(const char* why, const uint16_t widthBytes, const uint16_t height, const int orientation,
                          const unsigned long railsMs) const {
  SdDebugLog::log("DOSE", "%s grid=%dx%d native=%ux%u orient=%d totals fast=%u scrub=%u half=%u full=%u gray=%u railsMs=%lu",
                  why, GRID, GRID, static_cast<unsigned>(widthBytes) * 8, static_cast<unsigned>(height), orientation,
                  static_cast<unsigned>(totalFast), static_cast<unsigned>(total[Scrub]),
                  static_cast<unsigned>(total[Half]), static_cast<unsigned>(total[Full]),
                  static_cast<unsigned>(total[Gray]));
  if (!tiles) {
    SdDebugLog::log("DOSE", "no tile table (%s)", allocFailed ? "oom" : "logging was off");
    return;
  }
  const uint16_t sec = nowSec();
  const auto railsSec = static_cast<uint16_t>(railsMs / 1000);

  // Per-tile value since the tile's content last changed. Stays are what the held content sat
  // through: drive delivered to pixels that did not change.
  enum Field { F_SCRUB, F_FAST, F_HALF, F_FULL, F_GRAY, F_HELD, F_RAILS, F_INK, FIELD_COUNT };
  static constexpr const char* kNames[FIELD_COUNT] = {"scrub", "fast", "half", "full", "gray", "heldS", "railsS", "ink%"};
  const auto value = [&](const Tile& t, const int f) -> unsigned {
    if (t.secAt == 0) return 0;
    switch (f) {
      case F_SCRUB:
        return static_cast<uint8_t>(total[Scrub] - t.at[Scrub]);
      case F_FAST:
        return static_cast<uint16_t>(totalFast - t.fastAt);
      case F_HALF:
        return static_cast<uint8_t>(total[Half] - t.at[Half]);
      case F_FULL:
        return static_cast<uint8_t>(total[Full] - t.at[Full]);
      case F_GRAY:
        return static_cast<uint8_t>(total[Gray] - t.at[Gray]);
      case F_HELD:
        return static_cast<uint16_t>(sec - t.secAt);
      case F_RAILS:
        return static_cast<uint16_t>(railsSec - t.railsSecAt);
      default:
        return t.inkPct;
    }
  };

  // Decisive summary: inked tiles (where text sat) against blank ones.
  unsigned long sum[2][FIELD_COUNT] = {};
  unsigned maxv[2][FIELD_COUNT] = {};
  unsigned n[2] = {0, 0};
  for (int i = 0; i < GRID * GRID; i++) {
    const Tile& t = tiles[i];
    if (t.secAt == 0) continue;
    const int g = t.inkPct >= 3 ? 0 : (t.inkPct == 0 ? 1 : -1);
    if (g < 0) continue;
    n[g]++;
    for (int f = 0; f < FIELD_COUNT; f++) {
      const unsigned v = value(t, f);
      sum[g][f] += v;
      if (v > maxv[g][f]) maxv[g][f] = v;
    }
  }
  for (int g = 0; g < 2; g++) {
    if (n[g] == 0) continue;
    SdDebugLog::log("DOSE", "%s n=%u avg/max scrub=%lu/%u fast=%lu/%u half=%lu/%u full=%lu/%u gray=%lu/%u heldS=%lu/%u railsS=%lu/%u",
                    g == 0 ? "inked" : "blank", n[g], sum[g][F_SCRUB] / n[g], maxv[g][F_SCRUB],
                    sum[g][F_FAST] / n[g], maxv[g][F_FAST], sum[g][F_HALF] / n[g], maxv[g][F_HALF],
                    sum[g][F_FULL] / n[g], maxv[g][F_FULL], sum[g][F_GRAY] / n[g], maxv[g][F_GRAY],
                    sum[g][F_HELD] / n[g], maxv[g][F_HELD], sum[g][F_RAILS] / n[g], maxv[g][F_RAILS]);
  }

  // Full grids, one line per tile row (native space; see orient= to map onto the photo).
  char line[96];
  for (int f = 0; f < FIELD_COUNT; f++) {
    for (int tr = 0; tr < GRID; tr++) {
      int len = snprintf(line, sizeof(line), "%s r%02d:", kNames[f], tr);
      for (int tc = 0; tc < GRID && len > 0 && len < static_cast<int>(sizeof(line)); tc++) {
        len += snprintf(line + len, sizeof(line) - len, " %u", value(tiles[tr * GRID + tc], f));
      }
      SdDebugLog::log("DOSE", "%s", line);
    }
  }
}
