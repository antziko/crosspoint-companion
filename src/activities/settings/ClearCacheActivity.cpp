#include "ClearCacheActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"

void ClearCacheActivity::onEnter() {
  Activity::onEnter();

  // Prune scans first and previews the result; clear-all warns before wiping.
  state = mode_ == Mode::PruneOrphans ? SCANNING : WARNING;
  requestUpdate();
}

void ClearCacheActivity::onExit() { Activity::onExit(); }

void ClearCacheActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  const bool prune = mode_ == Mode::PruneOrphans;

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 prune ? tr(STR_REMOVE_ORPHANED_CACHES) : tr(STR_CLEAR_READING_CACHE));

  if (state == WARNING) {
    // Clear-all only; prune previews via SCANNING/PREVIEW instead.
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 60, tr(STR_CLEAR_CACHE_WARNING_1), true);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 30, tr(STR_CLEAR_CACHE_WARNING_2), true,
                              EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, tr(STR_CLEAR_CACHE_WARNING_3), true);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 30, tr(STR_CLEAR_CACHE_WARNING_4), true);

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_CLEAR_BUTTON), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == SCANNING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_SCANNING_CACHES));
    renderer.displayBuffer();
    return;
  }

  if (state == PREVIEW) {
    if (orphanDirs_.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_NO_ORPHANED_CACHES), true, EpdFontFamily::BOLD);
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      renderer.displayBuffer();
      return;
    }
    std::string foundText =
        std::to_string(static_cast<int>(orphanDirs_.size())) + " " + std::string(tr(STR_ORPHANED_CACHES_FOUND));
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 15, foundText.c_str(), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 20, tr(STR_PRUNE_CACHE_WARNING_3), true);

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_REMOVE_BUTTON), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == CLEARING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_CLEARING_CACHE));
    renderer.displayBuffer();
    return;
  }

  if (state == SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20,
                              prune ? tr(STR_ORPHANS_REMOVED) : tr(STR_CACHE_CLEARED), true, EpdFontFamily::BOLD);
    std::string resultText = std::to_string(clearedCount) + " " + std::string(tr(STR_ITEMS_REMOVED));
    if (failedCount > 0) {
      resultText += ", " + std::to_string(failedCount) + " " + std::string(tr(STR_FAILED_LOWER));
    }
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, resultText.c_str());

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_CLEAR_CACHE_FAILED), true,
                              EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, tr(STR_CHECK_SERIAL_OUTPUT));

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
}

void ClearCacheActivity::clearCache() {
  LOG_DBG("CLEAR_CACHE", "Clearing cache...");

  // Open .crosspoint directory
  auto root = Storage.open("/.crosspoint");
  if (!root || !root.isDirectory()) {
    LOG_DBG("CLEAR_CACHE", "Failed to open cache directory");
    if (root) root.close();
    state = FAILED;
    requestUpdate();
    return;
  }

  clearedCount = 0;
  failedCount = 0;
  char name[128];

  // Iterate through all entries in the directory
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    String itemName(name);

    // Only delete directories matching known book cache names.
    if (file.isDirectory() && isBookCacheDirectoryName(itemName.c_str())) {
      String fullPath = "/.crosspoint/" + itemName;
      LOG_DBG("CLEAR_CACHE", "Removing cache: %s", fullPath.c_str());

      file.close();  // Close before attempting to delete

      if (Storage.removeDir(fullPath.c_str())) {
        clearedCount++;
      } else {
        LOG_ERR("CLEAR_CACHE", "Failed to remove: %s", fullPath.c_str());
        failedCount++;
      }
    } else {
      file.close();
    }
  }
  root.close();

  LOG_DBG("CLEAR_CACHE", "Cache cleared: %d removed, %d failed", clearedCount, failedCount);

  state = SUCCESS;
  requestUpdate();
}

void ClearCacheActivity::scanOrphans() {
  LOG_DBG("CLEAR_CACHE", "Scanning for orphaned caches...");

  orphanDirs_.clear();
  orphanDirs_.reserve(16);
  scanResult_ = scanOrphanCaches(orphanDirs_);

  LOG_DBG("CLEAR_CACHE", "Scan done: %d orphans, %d kept, %d skipped", scanResult_.removed, scanResult_.kept,
          scanResult_.skipped);

  state = PREVIEW;
  requestUpdate();
}

void ClearCacheActivity::removeScannedOrphans() {
  LOG_DBG("CLEAR_CACHE", "Removing %d orphaned caches...", static_cast<int>(orphanDirs_.size()));

  const CachePruneResult res = removeOrphanCaches(orphanDirs_);
  clearedCount = res.removed;
  failedCount = res.failed;

  LOG_DBG("CLEAR_CACHE", "Prune done: %d removed, %d failed", res.removed, res.failed);

  state = SUCCESS;
  requestUpdate();
}

void ClearCacheActivity::loop() {
  if (state == SCANNING) {
    // SCANNING screen is already painted by the time loop() runs; scan now, then preview.
    scanOrphans();
    return;
  }

  if (state == WARNING) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      LOG_DBG("CLEAR_CACHE", "User confirmed, starting cache clear");
      {
        RenderLock lock(*this);
        state = CLEARING;
      }
      requestUpdateAndWait();

      clearCache();
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      LOG_DBG("CLEAR_CACHE", "User cancelled");
      goBack();
    }
    return;
  }

  if (state == PREVIEW) {
    if (!orphanDirs_.empty() && mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      LOG_DBG("CLEAR_CACHE", "User confirmed, removing orphans");
      {
        RenderLock lock(*this);
        state = CLEARING;
      }
      requestUpdateAndWait();

      removeScannedOrphans();
      return;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      LOG_DBG("CLEAR_CACHE", "User cancelled");
      goBack();
    }
    return;
  }

  if (state == SUCCESS || state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }
}
