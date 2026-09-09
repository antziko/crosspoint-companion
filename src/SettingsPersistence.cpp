#include "SettingsPersistence.h"

#include <Logging.h>

#if LOG_LEVEL >= 2

#include <cstring>

#include "SettingsList.h"

// Development-only cross-check between kPersistedSettings (SettingsPersistence.h) and the
// real settings list (SettingsList.h). The two must agree on which settings persist, which
// member each writes, and the range a loaded value is validated against.
//
// This exists because the two are edited independently: adding an option to an enum in
// SettingsList.h without widening the matching bound here would silently clamp that new
// value back to the default on every load -- a data-loss bug with no visible symptom at the
// point it is introduced. Failing loudly at boot during development is the cheapest place
// to catch it.
//
// Deliberately not compiled into gh_release: this is the only place that still needs
// SettingsList.h from the persistence path, and pulling that header in costs both flash
// (it is entirely inline) and the ~5.8 KB heap allocation this table exists to avoid.
// Called from setup(), where free heap is ~200 KB and building the list is free.
void verifySettingsPersistenceTable() {
  // Registry-less, matching how toJson()/fromJson() used to call it: the dynamic
  // font-family / dictionary entries stay in their static form and carry no valuePtr.
  const std::vector<SettingInfo> list = getSettingsList();

  int errors = 0;
  size_t matched = 0;

  for (const SettingInfo& info : list) {
    if (!info.key) continue;

    if (info.stringOffset) {
      // No persisted setting is string-backed today (SettingInfo::String has no call
      // sites), and PersistedU8 cannot express one. Adding the first needs a second table.
      LOG_ERR("CPSVFY", "'%s' is string-backed; PersistedU8 cannot persist it", info.key);
      errors++;
      continue;
    }
    if (!info.valuePtr) continue;  // dynamic entry, stored in its own file

    const PersistedU8* row = nullptr;
    for (const PersistedU8& p : kPersistedSettings) {
      if (strcmp(p.key, info.key) == 0) {
        row = &p;
        break;
      }
    }
    if (!row) {
      LOG_ERR("CPSVFY", "'%s' is in SettingsList but missing from kPersistedSettings", info.key);
      errors++;
      continue;
    }
    matched++;

    if (row->ptr != info.valuePtr) {
      LOG_ERR("CPSVFY", "'%s' maps to a different member in each table", info.key);
      errors++;
    }

    // Expected bounds, mirroring the clamps the old SettingInfo loop applied.
    uint8_t lo = 0;
    uint8_t hi = 0;
    bool clampToRange = false;
    switch (info.type) {
      case SettingType::TOGGLE:
        hi = 1;
        break;
      case SettingType::ENUM:
        if (info.enumValues.empty()) continue;  // runtime-labelled (SD fonts); no static bound
        hi = static_cast<uint8_t>(info.enumValues.size() - 1);
        break;
      case SettingType::VALUE:
        lo = info.valueRange.min;
        hi = info.valueRange.max;
        clampToRange = true;
        break;
      default:
        continue;
    }

    if (row->lo != lo || row->hi != hi || row->clampToRange != clampToRange) {
      LOG_ERR("CPSVFY", "'%s' bounds differ: table %u..%u clamp=%d, list %u..%u clamp=%d", info.key, row->lo, row->hi,
              row->clampToRange, lo, hi, clampToRange);
      errors++;
    }
  }

  // getSettingsList() erases rows the current board cannot show (SettingsList.h's
  // BoardConfig::hasTouch() block): the touch-only control on button boards, the
  // front-button/frontlight ones on touch boards. kPersistedSettings deliberately carries
  // all of them so settings.json stays board-independent -- a file written on an X3 loads
  // unchanged on a touch board and vice versa. Only the UI row is board-specific, not the
  // stored value, so these are expected absences rather than drift.
  // The frontlight keys are additionally compiled out of boards without one
  // (FREEINK_CAP_FRONTLIGHT), and tapForReaderMenu is dropped on boards with no
  // Home key. Same reasoning: the value travels, only the row is board-specific.
  static constexpr const char* kBoardConditionalKeys[] = {
      "touchReaderControls", "frontButtonFollowOrientation", "fadingFix", "tapForReaderMenu", "frontlightBrightness",
      "frontlightWarmth", "frontlightOn", "frontlightRestoreOnWake",
      // The toolbar reader menu is touch-only, so its style row is absent on
      // button boards; the stored value still travels with the file.
      "readerMenuStyle"};

  // Rows with no counterpart: a setting removed from SettingsList but left here would keep
  // being written to settings.json forever.
  for (const PersistedU8& p : kPersistedSettings) {
    bool boardConditional = false;
    for (const char* k : kBoardConditionalKeys) {
      if (strcmp(k, p.key) == 0) {
        boardConditional = true;
        break;
      }
    }
    if (boardConditional) continue;

    bool found = false;
    for (const SettingInfo& info : list) {
      if (info.key && strcmp(info.key, p.key) == 0) {
        found = true;
        break;
      }
    }
    if (!found) {
      LOG_ERR("CPSVFY", "'%s' is in kPersistedSettings but missing from SettingsList", p.key);
      errors++;
    }
  }

  if (errors) {
    LOG_ERR("CPSVFY", "%d persistence table mismatch(es) -- settings will not round-trip correctly", errors);
  } else {
    LOG_DBG("CPSVFY", "persistence table OK (%u/%u rows matched)", static_cast<unsigned>(matched),
            static_cast<unsigned>(std::size(kPersistedSettings)));
  }
}

#else

void verifySettingsPersistenceTable() {}

#endif
