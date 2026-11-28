#include "DictionarySelectActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>
#include <memory>

#include "CrossPointSettings.h"
#include "DictPrepareActivity.h"
#include "I18nKeys.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/DictionaryRegistry.h"
#include "util/ListSwipeScroll.h"

// Long press threshold for viewing dictionary metadata.
static constexpr unsigned long VIEW_INFO_MS = 1000;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void DictionarySelectActivity::onEnter() {
  Activity::onEnter();

  // Suppress Confirm bleed-through only in settings mode: when launched from a list,
  // the Confirm release that opened the picker fires again in the picker's first loop.
  // In per-book mode (launched via reader menu) the menu already consumed the event.
  ignoreNextConfirmRelease = bookCachePath.empty();

  scanDictionaries();

  if (bookCachePath.empty()) {
    // Settings mode: drop a stale global path, then pre-select the language group's current default.
    Dictionary::isValidDictionary();
    settingsLangPath = resolveLangDictPath(settingsLang);
    selectedIndex = 0;
    for (int i = 0; i < static_cast<int>(dictFolders.size()); i++) {
      if (folderForIndex(i + 1) == settingsLangPath) {
        selectedIndex = i + 1;
        break;
      }
    }
  } else {
    // Per-book mode: read saved per-book path, pre-select it.
    currentBookDictPath = "";
    HalFile f;
    if (Storage.openFileForRead("DSEL", bookCachePath + "/dictionary.bin", f)) {
      const int sz = static_cast<int>(f.fileSize());
      if (sz > 0) {
        std::string path(sz, '\0');
        const int n = f.read(&path[0], sz);
        if (n > 0) {
          path.resize(n);
          currentBookDictPath = path;
        }
      }
      f.close();
    }

    selectedIndex = 0;  // default: Use Global
    if (!currentBookDictPath.empty()) {
      for (int i = 0; i < static_cast<int>(dictFolders.size()); i++) {
        if (folderForIndex(i + 1) == currentBookDictPath) {
          selectedIndex = i + 1;
          break;
        }
      }
    }

    // Build augmented "Use Global" label showing the dictionary it resolves to: the book
    // language's default when one is in force, else the global dictionary.
    // Path format: <dictRoot>/<folder>/<stem> — extract <folder>.
    const std::string globalPath = Dictionary::bookLang() != Dictionary::kNoBookLang && Dictionary::bookLangDefault()[0]
                                       ? std::string(Dictionary::bookLangDefault())
                                       : Dictionary::readDictPath();
    std::string globalFolderName;
    if (globalPath.empty()) {
      globalFolderName = tr(STR_DICT_NONE);
    } else {
      const std::string_view folder = DictionaryRegistry::folderOfBasePath(globalPath);
      globalFolderName = folder.empty() ? globalPath : std::string(folder);
    }
    useGlobalLabel = std::string(tr(STR_DICT_USE_GLOBAL)) + " (" + globalFolderName + ")";
  }

  totalItems = 1 + static_cast<int>(dictFolders.size());
  rowOffset = bookCachePath.empty() && !dictFolders.empty() ? 1 : 0;
  if (selectedIndex < rowOffset) selectedIndex = rowOffset;
  showingInfo = false;
  infoBar_.layout(renderer, mappedInput.hasTouch(), UI_10_FONT_ID, 2);
  rawBar_.layout(renderer, mappedInput.hasTouch(), UI_10_FONT_ID, 1);

  requestUpdate();
}

void DictionarySelectActivity::onExit() { Activity::onExit(); }

// ---------------------------------------------------------------------------
// SD card scan
// ---------------------------------------------------------------------------

void DictionarySelectActivity::scanDictionaries() {
  // Discovery lives in DictionaryRegistry (shared with the settings list and web UI).
  // Re-scan on every picker open (matches prior behaviour), then mirror the results into
  // the activity's parallel vectors so folderForIndex()/metadata/per-book logic is unchanged.
  dictionaryRegistry.discover();
  dictRoot = dictionaryRegistry.root();
  dictFolders.clear();
  dictStems.clear();
  const auto& entries = dictionaryRegistry.getEntries();
  dictFolders.reserve(entries.size());
  dictStems.reserve(entries.size());
  // Settings mode lists only the language group it edits. Per-book mode lists only the book's
  // language group, unless that group has nothing installed.
  const uint8_t lang = bookCachePath.empty() ? settingsLang : Dictionary::bookLang();
  const bool filter = bookCachePath.empty() || (lang != Dictionary::kNoBookLang &&
                                                dictionaryRegistry.firstIndexInLang(static_cast<DictLang>(lang)) >= 0);
  for (const auto& e : entries) {
    if (filter && e.lang != static_cast<DictLang>(lang)) continue;
    dictFolders.push_back(e.name);
    dictStems.push_back(e.stem);
  }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void DictionarySelectActivity::showInfoForSelected() {
  std::string folder = folderForIndex(selectedIndex);
  // Straight into the member: the by-value form would put a 608-byte DictInfo temporary
  // in this frame just to copy-assign it away.
  Dictionary::readInfoInto(folder.c_str(), currentInfo);
  showingInfo = true;
  showingRaw = false;
  requestUpdate();
}

std::string DictionarySelectActivity::folderForIndex(int index) const {
  if (index <= 0 || index > static_cast<int>(dictFolders.size())) return "";
  return dictRoot + "/" + dictFolders[index - 1] + "/" + dictStems[index - 1];
}

const char* DictionarySelectActivity::nameForIndex(int index) const {
  if (index == 0) return bookCachePath.empty() ? tr(STR_DICT_NONE) : useGlobalLabel.c_str();
  if (index <= static_cast<int>(dictFolders.size())) return dictFolders[index - 1].c_str();
  return "";
}

void DictionarySelectActivity::applySelection() {
  std::string folder = folderForIndex(selectedIndex);

  if (bookCachePath.empty()) {
    // Settings mode: the language group's default, which also becomes the global dictionary
    // (used where no book language is known).
    if (folder.empty()) return;
    Dictionary::saveLangDictPath(settingsLang, folder.c_str());
    settingsLangPath = folder;
    if (Dictionary::readDictPath() == folder) return;
    Dictionary::saveGlobalDictPath(folder.c_str());
  } else {
    // Per-book mode: save to book cache.
    HalFile f;
    if (Storage.openFileForWrite("DSEL", bookCachePath + "/dictionary.bin", f)) {
      f.write(reinterpret_cast<const uint8_t*>(folder.c_str()), folder.size());
      f.close();
    } else {
      LOG_ERR("DSEL", "Could not save per-book dictionary");
    }
    currentBookDictPath = folder;
  }
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void DictionarySelectActivity::loop() {
  if (showingInfo) {
    // A tap on the on-screen buttons answers as the front buttons do (touch boards only).
    int tapX = 0;
    int tapY = 0;
    const bool tapped = mappedInput.wasScreenTapped(tapX, tapY);
    if (showingRaw) {
      // Raw view: Back returns to parsed metadata view.
      if (mappedInput.wasReleased(MappedInputManager::Button::Back) || (tapped && rawBar_.hitAt(tapX, tapY) == 0)) {
        showingRaw = false;
        requestUpdate();
      }
    } else {
      // Parsed metadata view: Back exits to picker; Confirm switches to raw view.
      const int hit = tapped ? infoBar_.hitAt(tapX, tapY) : -1;
      if (mappedInput.wasReleased(MappedInputManager::Button::Back) || hit == 0) {
        showingInfo = false;
        requestUpdate();
      } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) || hit == 1) {
        showingRaw = true;
        requestUpdate();
      }
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  // Long press Confirm: show dictionary metadata (only when a real dictionary is highlighted).
  if (mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= VIEW_INFO_MS &&
      selectedIndex > 0) {
    showInfoForSelected();
    return;
  }

  // Touch hold on a dictionary row = the Confirm hold above. The X4 Pro has no Confirm pin
  // at all (BoardConfig.h, XTEINK_X4_PRO), so without this the metadata view is unreachable
  // there. Resolved before the tap: wasScreenLongPress suppresses the rest of the contact,
  // so the finger lift cannot also apply the dictionary and leave.
  int holdX = 0;
  int holdY = 0;
  if (mappedInput.wasScreenLongPress(holdX, holdY)) {
    const int heldRow = listTouch_.indexAt(renderer, holdX, holdY);
    if (heldRow >= 0 && heldRow + rowOffset > 0) {
      selectedIndex = heldRow + rowOffset;
      showInfoForSelected();
    }
    return;
  }

  // Short press Confirm: apply selection (or decompress if compressed) and exit.
  // A tap on a row selects and activates it in one go, like the FUI list screens.
  // A tap is never a hold, so it cannot reach the long-press branch above.
  int tapX = 0;
  int tapY = 0;
  const int tappedRow = mappedInput.wasScreenTapped(tapX, tapY) ? listTouch_.touchRow(renderer, tapX, tapY) : -1;
  if (tappedRow >= 0) selectedIndex = tappedRow + rowOffset;

  if ((mappedInput.wasReleased(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() < VIEW_INFO_MS) ||
      tappedRow >= 0) {
    // The release lock guards a button edge a tap did not come from.
    if (ignoreNextConfirmRelease && tappedRow < 0) {
      ignoreNextConfirmRelease = false;
      return;
    }

    // For a real dictionary entry, delegate preparation check to DictPrepareActivity.
    // It detects required steps and either runs them or exits immediately if none are needed.
    if (selectedIndex > 0) {
      std::string folder = folderForIndex(selectedIndex);
      startActivityForResultNoThrow<DictPrepareActivity>(
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              applySelection();
              finish();
            }
            // Cancelled/failed: stay in picker with the same highlighted index.
          },
          renderer, mappedInput, folder);
      return;
    }

    applySelection();
    finish();
    return;
  }

  const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false);

  buttonNavigator.onNextRelease([this] {
    selectedIndex = rowOffset + ButtonNavigator::nextIndex(selectedIndex - rowOffset, rowCount());
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectedIndex = rowOffset + ButtonNavigator::previousIndex(selectedIndex - rowOffset, rowCount());
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, pageItems] {
    selectedIndex = rowOffset + ButtonNavigator::nextPageIndex(selectedIndex - rowOffset, rowCount(), pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, pageItems] {
    selectedIndex = rowOffset + ButtonNavigator::previousPageIndex(selectedIndex - rowOffset, rowCount(), pageItems);
    requestUpdate();
  });
  // A swipe pages like a held side key, wrapping at both ends.
  if (const int step = listSwipeStep(mappedInput)) {
    const int row = selectedIndex - rowOffset;
    selectedIndex = rowOffset + (step > 0 ? ButtonNavigator::nextPageIndex(row, rowCount(), pageItems)
                                          : ButtonNavigator::previousPageIndex(row, rowCount(), pageItems));
    requestUpdate();
  }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void DictionarySelectActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();

  if (showingInfo) {
    GUI.drawHeader(renderer, UITheme::getInstance().getSafeHeaderRect(renderer), tr(STR_DICT_INFO));

    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const Rect infoArea = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
    int y = infoArea.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int x = infoArea.x + metrics.contentSidePadding;
    const int maxWidth = infoArea.width - metrics.contentSidePadding * 2;
    // Both bars share one height; nothing is drawn under them.
    const int maxY = infoArea.y + infoArea.height - metrics.verticalSpacing - infoBar_.reservedHeight();

    if (showingRaw) {
      // --- Raw view: forward-only SD streaming, character-wrapped per line ---
      char ifoPath[520];
      snprintf(ifoPath, sizeof(ifoPath), "%s.ifo", folderForIndex(selectedIndex).c_str());
      HalFile ifoFile;
      if (!Storage.openFileForRead("DSEL", ifoPath, ifoFile)) {
        renderer.drawText(UI_10_FONT_ID, x, y, tr(STR_DICT_NO_METADATA));
      } else {
        char segBuf[256];
        int segLen = 0;
        while (y + lineHeight <= maxY) {
          const int firstByte = ifoFile.read();
          if (firstByte == -1) break;
          if (firstByte == '\r') continue;
          if (firstByte == '\n') {
            if (segLen > 0) {
              segBuf[segLen] = '\0';
              renderer.drawText(UI_10_FONT_ID, x, y, segBuf);
              segLen = 0;
            }
            y += lineHeight;
            continue;
          }
          // Determine UTF-8 codepoint length from leading byte.
          int cpLen = 1;
          if ((firstByte & 0xE0) == 0xC0)
            cpLen = 2;
          else if ((firstByte & 0xF0) == 0xE0)
            cpLen = 3;
          else if ((firstByte & 0xF8) == 0xF0)
            cpLen = 4;
          char cpBuf[5];
          cpBuf[0] = static_cast<char>(firstByte);
          for (int i = 1; i < cpLen; i++) {
            const int b = ifoFile.read();
            if (b == -1) {
              cpLen = i;
              break;
            }
            cpBuf[i] = static_cast<char>(b);
          }
          // Flush segment if codepoint won't fit in segBuf.
          if (segLen + cpLen >= static_cast<int>(sizeof(segBuf)) - 1) {
            segBuf[segLen] = '\0';
            renderer.drawText(UI_10_FONT_ID, x, y, segBuf);
            y += lineHeight;
            segLen = 0;
          }
          memcpy(segBuf + segLen, cpBuf, cpLen);
          segLen += cpLen;
          segBuf[segLen] = '\0';
          // If rendered width exceeds column, wrap before this codepoint.
          if (renderer.getTextWidth(UI_10_FONT_ID, segBuf) > maxWidth) {
            segBuf[segLen - cpLen] = '\0';
            if (segLen - cpLen > 0) {
              renderer.drawText(UI_10_FONT_ID, x, y, segBuf);
              y += lineHeight;
            }
            memcpy(segBuf, cpBuf, cpLen);
            segLen = cpLen;
          }
        }
        if (segLen > 0 && y + lineHeight <= maxY) {
          segBuf[segLen] = '\0';
          renderer.drawText(UI_10_FONT_ID, x, y, segBuf);
        }
        ifoFile.close();
      }

      if (rawBar_.active()) {
        const char* barLabels[] = {tr(STR_BACK)};
        rawBar_.draw(renderer, UI_10_FONT_ID, barLabels, /*primaryIndex=*/0);
      }
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    } else {
      // --- Parsed metadata view ---
      // Short fixed-length fields use single-line truncation; long fields (name, description,
      // website, status) use character-level wrapping so no content is silently cut off (F-001).
      auto drawLine = [&](const char* label, const char* value) {
        if (value == nullptr || value[0] == '\0') return;
        char buf[128];
        snprintf(buf, sizeof(buf), "%s: %s", label, value);
        std::string line = renderer.truncatedText(UI_10_FONT_ID, buf, maxWidth);
        renderer.drawText(UI_10_FONT_ID, x, y, line.c_str());
        y += lineHeight;
      };

      // Character-level wrapping for values that may exceed one line (URLs, descriptions, etc.).
      auto drawWrapped = [&](const char* label, const char* value) {
        if (value == nullptr || value[0] == '\0') return;
        char buf[384];
        snprintf(buf, sizeof(buf), "%s: %s", label, value);
        const char* text = buf;
        size_t totalLen = strlen(text);
        char segBuf[256];
        size_t pos = 0;
        while (pos < totalLen && y + lineHeight <= maxY) {
          size_t endPos = totalLen;
          while (endPos > pos) {
            size_t segLen = endPos - pos < sizeof(segBuf) - 1 ? endPos - pos : sizeof(segBuf) - 1;
            memcpy(segBuf, text + pos, segLen);
            segBuf[segLen] = '\0';
            if (renderer.getTextWidth(UI_10_FONT_ID, segBuf) <= maxWidth) break;
            endPos--;
          }
          if (endPos == pos) endPos = pos + 1;
          size_t segLen = endPos - pos < sizeof(segBuf) - 1 ? endPos - pos : sizeof(segBuf) - 1;
          memcpy(segBuf, text + pos, segLen);
          segBuf[segLen] = '\0';
          renderer.drawText(UI_10_FONT_ID, x, y, segBuf);
          y += lineHeight;
          pos = endPos;
        }
      };

      char wordcountBuf[24];
      char synBuf[24];
      snprintf(wordcountBuf, sizeof(wordcountBuf), "%lu", static_cast<unsigned long>(currentInfo.wordcount));
      snprintf(synBuf, sizeof(synBuf), "%lu", static_cast<unsigned long>(currentInfo.altFormCount));

      if (!currentInfo.valid) {
        renderer.drawText(UI_10_FONT_ID, x, y, tr(STR_DICT_NO_METADATA));
      } else {
        drawWrapped("Name", currentInfo.bookname);
        drawLine("Words", wordcountBuf);
        if (currentInfo.hasAltForms) drawLine("Alt Forms", synBuf);
        drawLine("Date", currentInfo.date);
        drawWrapped("Website", currentInfo.website);
        drawWrapped("Description", currentInfo.description);
        drawLine("Type", currentInfo.sametypesequence);
        if (currentInfo.isCompressed) {
          drawWrapped("Status", "Compressed (.dict.dz) -- extract before use");
        }
      }

      if (infoBar_.active()) {
        const char* barLabels[] = {tr(STR_BACK), tr(STR_DICT_VIEW_RAW)};
        infoBar_.draw(renderer, UI_10_FONT_ID, barLabels, /*primaryIndex=*/1);
      }
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DICT_VIEW_RAW), "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }

    renderer.displayBuffer();
    return;
  }

  // --- Picker screen ---
  GUI.drawHeader(renderer, UITheme::getInstance().getSafeHeaderRect(renderer), I18N.get(titleId));

  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  const int contentTop = safe.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = safe.y + safe.height - contentTop - metrics.verticalSpacing;

  // Show "None found" note when no dictionaries are available
  if (dictFolders.empty()) {
    const int textY = contentTop + contentHeight / 3;
    UITheme::drawCenteredText(renderer, safe, UI_10_FONT_ID, textY, tr(STR_DICT_NONE_FOUND));
  }

  listTouch_.record(Rect{safe.x, contentTop, safe.width, contentHeight}, rowCount(), selectedIndex - rowOffset);
  GUI.drawList(
      renderer, Rect{safe.x, contentTop, safe.width, contentHeight}, rowCount(), selectedIndex - rowOffset,
      [this](int row) { return std::string(nameForIndex(row + rowOffset)); }, nullptr, nullptr,
      [this](int row) -> std::string {
        // Show "Selected" marker for the currently active dictionary.
        // In per-book mode compare against currentBookDictPath; in settings mode against the language default.
        std::string folder = folderForIndex(row + rowOffset);
        const std::string& activePath = bookCachePath.empty() ? settingsLangPath : currentBookDictPath;
        if (folder.empty() && activePath.empty()) return tr(STR_SELECTED);
        if (!folder.empty() && folder == activePath) return tr(STR_SELECTED);
        return "";
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
