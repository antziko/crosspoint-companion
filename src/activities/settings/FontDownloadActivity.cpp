#include "FontDownloadActivity.h"

#include <ArduinoJson.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <SdDebugLog.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_rom_crc.h>

#include <algorithm>
#include <cstring>

#include "FontDownloadCA.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace fui = freeink::ui;

// The manifest is fetched to SD rather than held in RAM, and KEPT after parsing: the download
// path releases the parsed tables to free the contiguous block the TLS handshake needs, then
// re-parses from here instead of re-fetching. Removed in onExit().
static constexpr const char* MANIFEST_TMP = "/fonts_manifest.tmp";

FontDownloadActivity::FontDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("FontDownload", renderer, mappedInput), fontInstaller_(sdFontSystem.registry()) {}

void FontDownloadActivity::activateIndex(const int index) {
  switch (state_) {
    case GROUP_LIST:
      app.clearTapFlash();
      enterGroup(index);
      requestUpdate();
      return;
    case FAMILY_LIST:
      nav.selected = index;
      // Activation starts a download or opens the delete prompt; a lingering
      // flash would gray an unrelated row.
      app.clearTapFlash();
      activateSelected();  // ends with requestUpdateAndWait itself
      return;
    case WIFI_SELECTION:
    case LOADING_MANIFEST:
    case DOWNLOADING:
    case COMPLETE:
    case ERROR:
      return;
  }
}

fui::ListNav& FontDownloadActivity::activeNav() { return state_ == GROUP_LIST ? groupNav_ : nav; }

void FontDownloadActivity::onBackButton() {
  if (state_ != FAMILY_LIST || !hasGroupScreen()) {
    finish();
    return;
  }

  closeRouting();
  {
    RenderLock lock(*this);
    state_ = GROUP_LIST;
    rowsDirty_ = true;
  }
  requestUpdate();
}

// --- Lifecycle ---

void FontDownloadActivity::onEnter() {
  UiListActivity::onEnter();

  // Font-download troubleshooting: own the SD trace for the lifetime of this activity,
  // the same way OpdsBookBrowserActivity does for its feed + book download. Both go
  // through HttpDownloader::runGet, so the HTTP/FONT lines below are the whole story.
  //
  // Wired explicitly rather than relying on Activity::onEnter's logHeap(), which also
  // flips the trace on but only under TRACE_HEAP — that covers the `default` and
  // `memtrace` envs and nothing else, so a release-build repro would silently capture
  // nothing. clear() first so the capture starts at this screen instead of being buried
  // under every previous activity's MEM lines; it discards the previous /opds_debug.txt.
  SdDebugLog::clear();
  SdDebugLog::setEnabled(true);
  // Anchor line: if /opds_debug.txt is missing or empty after a run, the master switch
  // (SETTINGS.sdCardLogging) is off — not the download path failing to reach any code.
  SdDebugLog::log("FONT", "screen enter, manifest=%s", FONT_MANIFEST_URL);

  // Reclaim the font heap before the radio comes up. The Wi-Fi driver's own allocations
  // plus the scan list leave only a few KB free, and the picker started below still needs
  // a contiguous block for its activity object; a capture caught that allocation failing
  // at 1112 bytes free / 628 largest, which is what makes this screen occasionally do
  // nothing until a reboot. releaseCache() rather than the fuller unload done before the
  // manifest fetch: the mini arenas and ~3KB kern tables it returns are already far more
  // than that allocation needs, and it leaves the SD font resident so a CJK SSID still
  // renders in the picker.
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->releaseCache();
  }

  WiFi.mode(WIFI_STA);
  startActivityForResultNoThrow<WifiSelectionActivity>(
      [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); }, renderer, mappedInput);
}

void FontDownloadActivity::onExit() {
  Activity::onExit();

  // After Activity::onExit(), whose logHeap() re-enables the trace under TRACE_HEAP —
  // otherwise the rest of the session would keep appending. Every log() line is
  // force-synced as it is written, so the silent restart below cannot truncate the
  // capture.
  SdDebugLog::log("FONT", "screen exit");
  SdDebugLog::setEnabled(false);

  // Kept for the duration of the screen so a download can re-parse it instead of re-fetching.
  Storage.remove(MANIFEST_TMP);

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestartToSettings(/*Reader=*/1);
  }
}

void FontDownloadActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    finish();
    return;
  }

  {
    RenderLock lock(*this);
    state_ = LOADING_MANIFEST;
  }
  requestUpdateAndWait();

  if (!fetchAndParseManifest()) {
    {
      RenderLock lock(*this);
      state_ = ERROR;
    }
    return;
  }

  if (!hasGroupScreen()) buildFilteredIndices(0);

  {
    RenderLock lock(*this);
    rowsDirty_ = true;  // families_ just loaded
    if (hasGroupScreen()) {
      groupNav_.reset();
      state_ = GROUP_LIST;
    } else {
      nav.reset();
      state_ = FAMILY_LIST;
    }
  }
}

// --- Manifest fetching ---

bool FontDownloadActivity::fetchAndParseManifest() {
  // Capture the real failure reason (HTTP code / connect error) so a USB-locked X3 —
  // which can't see the LOG_ERR serial output — can still diagnose "Manage Fonts" from
  // the SD debug log. The GitHub release URL 302-redirects to a CDN host; a second TLS
  // handshake there is the prime X3 failure suspect.
  // Reclaim the decompressed-glyph cache before the TLS handshake. The settings menus
  // populate it heavily with malloc'd glyph-page buffers; the github HTTPS handshake
  // needs a large *contiguous* block for the mbedtls record buffers (in ~8.5K + out
  // ~4.4K), and on the heap-tight X3/X4 the font path reaches it with the largest free
  // block already down at ~20K — below what the two allocs plus the pinned-cert parse
  // need, so ssl_setup OOMs (-0x7F00). Freeing the cache lifts the largest free block
  // back over the line; it repopulates lazily on the next render. The GET-start heap
  // line in HttpDownloader (SD log) shows the recovered largest8 for verification.
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->clearCache();
  }
  // Also unload the resident SD font family's interval/kern/glyph-metadata tables (~10KB+).
  // github's 302 carries a 3.5KB content-security-policy header that esp_http_client
  // accumulates while the ~26KB mbedtls arena is still live; without this reclaim the
  // largest free block (~5.8KB) can't take it and it OOM-asserts in http_utils. A reboot
  // (silentRestartToSettings on exit) restores the font, so unloading here is free.
  sdFontSystem.unloadFonts(renderer);

  std::string errorDetail;
  auto result = HttpDownloader::downloadToFile(FONT_MANIFEST_URL, MANIFEST_TMP, nullptr, nullptr, "", "", &errorDetail,
                                               FONT_CA_GITHUB_PEM, FONT_CA_ASSETS_PEM);
  if (result != HttpDownloader::OK) {
    LOG_ERR("FONT", "Failed to fetch manifest from %s", FONT_MANIFEST_URL);
    SdDebugLog::log("FONT", "MANIFEST fetch FAILED: err=%d detail=\"%s\" url=%s", static_cast<int>(result),
                    errorDetail.c_str(), FONT_MANIFEST_URL);
    errorMessage_ = "Failed to fetch font list";
    Storage.remove(MANIFEST_TMP);
    return false;
  }

  return parseManifestFile();
}

bool FontDownloadActivity::parseManifestFile() {
  // Scoped so the JsonDocument's variant pools — the largest single structure on this path —
  // are returned BEFORE the row vectors below are sized. The document is dead weight by then:
  // everything it holds has already been copied into families_/scriptGroupLabels_. Leaving it
  // alive to the end of the function is what put a 1,960-byte reserve against a 1,268-byte
  // largest block and abort()ed the device (X3, 09-07 capture).
  {
    // HTTP client is now closed — TLS buffers freed. Parse JSON from file.
    HalFile manifestFile;
    if (!Storage.openFileForRead("FONT", MANIFEST_TMP, manifestFile)) {
      LOG_ERR("FONT", "Failed to open temp manifest");
      SdDebugLog::log("FONT", "MANIFEST open FAILED: %s", MANIFEST_TMP);
      Storage.remove(MANIFEST_TMP);
      errorMessage_ = "Failed to read font list";
      return false;
    }

    const size_t manifestBytes = manifestFile.fileSize();
    // Second reclaim, after the transfer rather than before it: rendering the "Loading" frames
    // between the pre-handshake reclaim above and this point repopulates the glyph arenas, and
    // the pools deserializeJson is about to take need contiguous space. releaseCache() rather
    // than the clearCache() used pre-handshake — that one is sized to leave the TLS record
    // buffers room, a constraint that no longer applies once the client is closed.
    if (auto* fcm = renderer.getFontCacheManager()) {
      fcm->releaseCache();
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, manifestFile);
    manifestFile.close();

    if (err) {
      Storage.remove(MANIFEST_TMP);
      LOG_ERR("FONT", "Manifest parse error: %s", err.c_str());
      SdDebugLog::log("FONT", "MANIFEST parse FAILED: %s (downloaded %u bytes)", err.c_str(),
                      static_cast<unsigned>(manifestBytes));
      errorMessage_ = "Invalid font manifest";
      return false;
    }

    int version = doc["version"] | 0;
    if (version != FONTS_MANIFEST_VERSION) {
      LOG_ERR("FONT", "Unsupported manifest version: %d", version);
      SdDebugLog::log("FONT", "MANIFEST version mismatch: got=%d want=%d", version, FONTS_MANIFEST_VERSION);
      errorMessage_ = "Unsupported manifest version";
      return false;
    }

    baseUrl_ = doc["baseUrl"] | "";
    families_.clear();
    scriptGroupLabels_.clear();
    filteredIndices_.clear();
    fontInstaller_.refreshRegistry();

    JsonArray groupsArr = doc["scriptGroups"].as<JsonArray>();
    const size_t groupCount = std::min(groupsArr.size(), MAX_SCRIPT_GROUPS);
    // Every reserve on this path is nothrow: the radio is up, so the largest free block is
    // structurally small, and a bare std::vector::reserve that cannot be met calls the throwing
    // operator new -> abort(). Failing the parse shows "Out of memory" and keeps the device up.
    if (!reserveNoThrow(scriptGroupLabels_, groupCount)) {
      LOG_ERR("FONT", "OOM: %zu script group labels", groupCount);
      errorMessage_ = tr(STR_MEMORY_ERROR);
      return false;
    }
    if (groupsArr.size() > MAX_SCRIPT_GROUPS) {
      LOG_ERR("FONT", "Manifest declares more than %zu script groups; extra groups ignored", MAX_SCRIPT_GROUPS);
    }
    for (size_t groupIndex = 0; groupIndex < groupCount; groupIndex++) {
      JsonObject groupObj = groupsArr[groupIndex].as<JsonObject>();
      const char* tag = groupObj["tag"] | "";
      const char* label = groupObj["label"] | "";
      if (*tag == '\0' || *label == '\0') {
        LOG_ERR("FONT", "Malformed script group at index %zu", groupIndex);
        errorMessage_ = "Invalid font manifest";
        return false;
      }
      scriptGroupLabels_.push_back(label);
    }

    JsonArray familiesArr = doc["families"].as<JsonArray>();
    if (!reserveNoThrow(families_, familiesArr.size()) || !reserveNoThrow(filteredIndices_, familiesArr.size())) {
      LOG_ERR("FONT", "OOM: %zu families", familiesArr.size());
      errorMessage_ = tr(STR_MEMORY_ERROR);
      return false;
    }

    for (JsonObject fObj : familiesArr) {
      ManifestFamily family;
      family.name = fObj["name"] | "";
      family.description = fObj["description"] | "";

      // The manifest's "styles" array is deliberately not read. Nothing on this screen shows a
      // family's style list, and parsing it cost one vector allocation per family held for the
      // whole life of the screen — through every per-file TLS handshake, which needs contiguity
      // more than it needs anything else here.

      for (JsonVariant script : fObj["scripts"].as<JsonArray>()) {
        const char* familyTag = script.as<const char*>();
        if (!familyTag) continue;
        for (size_t groupIndex = 0; groupIndex < scriptGroupLabels_.size(); groupIndex++) {
          JsonObject groupObj = groupsArr[groupIndex].as<JsonObject>();
          const char* groupTag = groupObj["tag"] | "";
          if (std::strcmp(familyTag, groupTag) == 0) {
            family.scriptMask |= uint32_t{1} << groupIndex;
            break;
          }
        }
      }

      family.totalSize = 0;
      const JsonArray filesArr = fObj["files"].as<JsonArray>();
      if (!reserveNoThrow(family.files, filesArr.size())) {
        LOG_ERR("FONT", "OOM: %zu files for %s", filesArr.size(), family.name.c_str());
        errorMessage_ = tr(STR_MEMORY_ERROR);
        return false;
      }
      for (JsonObject fileObj : filesArr) {
        ManifestFile file;
        file.name = fileObj["name"] | "";
        file.size = fileObj["size"] | 0;

        if (!fileObj["crc32"].is<uint32_t>()) {
          LOG_ERR("FONT", "Malformed manifest file entry: missing or invalid crc32 for %s", file.name.c_str());
          SdDebugLog::log("FONT", "MANIFEST malformed: missing crc32 for %s", file.name.c_str());
          errorMessage_ = "Invalid font manifest";
          return false;
        }
        file.crc32 = fileObj["crc32"].as<uint32_t>();

        family.totalSize += file.size;
        family.files.push_back(std::move(file));
      }

      family.installed = fontInstaller_.isFamilyInstalled(family.name.c_str());

      // Detect updates by comparing manifest file sizes with files on disk.
      // Not a checksum, but a size mismatch reliably indicates a rebuild in practice.
      if (family.installed) {
        for (const auto& file : family.files) {
          char path[128];
          FontInstaller::buildFontPath(family.name.c_str(), file.name.c_str(), path, sizeof(path));
          HalFile f;
          if (Storage.openFileForRead("FONT", path, f)) {
            size_t actual = f.fileSize();
            f.close();
            if (actual != file.size) {
              family.hasUpdate = true;
              break;
            }
          } else {
            // File missing on disk but family dir exists — treat as update
            family.hasUpdate = true;
            break;
          }
        }
      }

      families_.push_back(std::move(family));
    }
  }  // JsonDocument + HalFile released here

  // Sized once here so the two rebuild*RowItems() paths, which run on every navigation, never
  // have to grow. This pair is what abort()ed the X3 on 09-07: rowItems_ asked for
  // 35 * sizeof(fui::ListItem) = 1,960 contiguous bytes against a 1,268-byte largest block,
  // with the JsonDocument (now released above) still holding the heap down.
  const size_t rowCapacity = std::max(families_.size() + 2, scriptGroupLabels_.size() + 1);
  if (!reserveNoThrow(rowLabels_, rowCapacity) || !reserveNoThrow(rowItems_, rowCapacity)) {
    LOG_ERR("FONT", "OOM: %zu list rows", rowCapacity);
    SdDebugLog::log("FONT", "MANIFEST rows OOM: rows=%u free=%u largest=%u", static_cast<unsigned>(rowCapacity),
                    static_cast<unsigned>(ESP.getFreeHeap()),
                    static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    errorMessage_ = tr(STR_MEMORY_ERROR);
    return false;
  }

  LOG_DBG("FONT", "Manifest loaded: %zu families, %zu script groups", families_.size(), scriptGroupLabels_.size());
  return true;
}

// --- Download ---

void FontDownloadActivity::releaseManifestWorkingSet() {
  // swap-with-empty, not clear(): clear() destroys the elements but keeps the capacity, and the
  // capacity IS the block this exists to hand back.
  std::vector<ManifestFamily>().swap(families_);
  std::vector<int>().swap(filteredIndices_);
  std::vector<std::string>().swap(scriptGroupLabels_);
  std::vector<std::string>().swap(rowLabels_);
  std::vector<fui::ListItem>().swap(rowItems_);
  // Nothing may render a list row until reloadManifestFromCache() puts the tables back; the
  // DOWNLOADING state draws from jobs_ instead, and its rows are rebuilt on the way out.
  rowsDirty_ = true;

  multi_heap_info_t info;
  heap_caps_get_info(&info, MALLOC_CAP_8BIT);
  SdDebugLog::log("FONT", "manifest released: free=%u largest=%u blocks=%u jobs=%u", (unsigned)info.total_free_bytes,
                  (unsigned)info.largest_free_block, (unsigned)info.free_blocks, (unsigned)jobs_.size());
}

bool FontDownloadActivity::reloadManifestFromCache() {
  // The just-written files change installed/hasUpdate, and the parse reads both.
  fontInstaller_.refreshRegistry();
  if (!parseManifestFile()) {
    LOG_ERR("FONT", "Manifest reload failed after download");
    return false;
  }
  // Unconditionally, and for the group the user is actually in: releaseManifestWorkingSet()
  // emptied filteredIndices_, and only enterGroup() ever refills it. Skipping this when a group
  // screen exists left the family list empty behind the "installed" screen.
  buildFilteredIndices(currentGroupIndex_);
  return true;
}

bool FontDownloadActivity::buildJobs(const bool wantUpdates) {
  std::vector<DownloadJob>().swap(jobs_);
  jobIndex_ = 0;
  if (!reserveNoThrow(jobs_, filteredIndices_.size())) {
    LOG_ERR("FONT", "OOM: %zu download jobs", filteredIndices_.size());
    errorMessage_ = tr(STR_MEMORY_ERROR);
    return false;
  }
  for (const int familyIndex : filteredIndices_) {
    const auto& family = families_[familyIndex];
    if (wantUpdates ? !family.hasUpdate : family.installed) continue;
    DownloadJob job;
    job.name = family.name;
    if (!reserveNoThrow(job.files, family.files.size())) {
      LOG_ERR("FONT", "OOM: %zu job files for %s", family.files.size(), family.name.c_str());
      errorMessage_ = tr(STR_MEMORY_ERROR);
      std::vector<DownloadJob>().swap(jobs_);
      return false;
    }
    job.files = family.files;
    jobs_.push_back(std::move(job));
  }
  return true;
}

void FontDownloadActivity::runJobs() {
  cancelRequested_ = false;
  if (jobs_.empty()) {
    RenderLock lock(*this);
    state_ = COMPLETE;
    return;
  }

  // The whole point of the job snapshot: from here to the reload there is no parsed manifest in
  // RAM, so the per-file handshakes see the largest free block the screen can offer.
  releaseManifestWorkingSet();

  bool aborted = false;
  for (jobIndex_ = 0; jobIndex_ < jobs_.size(); jobIndex_++) {
    downloadFamily(jobs_[jobIndex_]);
    // The home gesture has already finished this activity from inside the download callback;
    // there is no screen left to reload the tables for, and parsing into a dying activity
    // would just delay the transition.
    if (goHomeRequested_) return;
    if (state_ == ERROR || cancelRequested_) {
      aborted = true;
      break;
    }
  }

  // Unconditional: the ERROR state's retry and its Back both need the family list behind them,
  // and leaving the tables empty would show an empty screen with no way back to a populated one.
  if (!reloadManifestFromCache()) {
    RenderLock lock(*this);
    state_ = ERROR;
    if (errorMessage_.empty()) errorMessage_ = "Failed to read font list";
    return;
  }

  if (aborted) return;  // state_ already ERROR, or the user cancelled
  {
    RenderLock lock(*this);
    state_ = COMPLETE;
  }
}

void FontDownloadActivity::retryCurrentJob() {
  if (jobIndex_ >= jobs_.size()) return;
  // Reached from the ERROR screen, where runJobs() has already reloaded the manifest — so the
  // working set is resident again and has to come back off before this handshake, exactly as
  // it did for the first attempt.
  releaseManifestWorkingSet();
  downloadFamily(jobs_[jobIndex_]);
  const bool failed = state_ == ERROR || cancelRequested_;
  if (!reloadManifestFromCache()) {
    RenderLock lock(*this);
    state_ = ERROR;
    if (errorMessage_.empty()) errorMessage_ = "Failed to read font list";
    return;
  }
  if (failed) return;
  RenderLock lock(*this);
  state_ = COMPLETE;
}

void FontDownloadActivity::downloadAll() {
  if (!buildJobs(/*wantUpdates=*/false)) {
    RenderLock lock(*this);
    state_ = ERROR;
    return;
  }
  runJobs();
}

void FontDownloadActivity::updateAll() {
  if (!buildJobs(/*wantUpdates=*/true)) {
    RenderLock lock(*this);
    state_ = ERROR;
    return;
  }
  runJobs();
}

bool FontDownloadActivity::showDownloadAllRow() const {
  for (const int familyIndex : filteredIndices_) {
    if (!families_[familyIndex].installed) return true;
  }
  return false;
}

bool FontDownloadActivity::showUpdateAllRow() const {
  for (const int familyIndex : filteredIndices_) {
    if (families_[familyIndex].hasUpdate) return true;
  }
  return false;
}

int FontDownloadActivity::specialRowCount() const {
  return (showDownloadAllRow() ? 1 : 0) + (showUpdateAllRow() ? 1 : 0);
}

bool FontDownloadActivity::isDownloadAllRow(int index) const { return showDownloadAllRow() && index == 0; }

bool FontDownloadActivity::isUpdateAllRow(int index) const {
  return showUpdateAllRow() && index == (showDownloadAllRow() ? 1 : 0);
}

int FontDownloadActivity::listItemCount() const {
  return filteredIndices_.empty() ? 0 : static_cast<int>(filteredIndices_.size()) + specialRowCount();
}

int FontDownloadActivity::listCount() const {
  switch (state_) {
    case GROUP_LIST:
      return groupListItemCount();
    case FAMILY_LIST:
      return listItemCount();
    case WIFI_SELECTION:
    case LOADING_MANIFEST:
    case DOWNLOADING:
    case COMPLETE:
    case ERROR:
      return 0;
  }
  return 0;
}

int FontDownloadActivity::familyIndexFromList(const int listIndex) const {
  const int filteredIndex = listIndex - specialRowCount();
  if (filteredIndex < 0 || filteredIndex >= static_cast<int>(filteredIndices_.size())) return -1;
  return filteredIndices_[filteredIndex];
}

int FontDownloadActivity::groupMemberCount(const int scriptGroupIndex) const {
  if (scriptGroupIndex < 0 || scriptGroupIndex >= static_cast<int>(scriptGroupLabels_.size())) return 0;
  const uint32_t groupBit = uint32_t{1} << scriptGroupIndex;
  int count = 0;
  for (const auto& family : families_) {
    if (family.scriptMask & groupBit) count++;
  }
  return count;
}

void FontDownloadActivity::buildFilteredIndices(const int groupListIndex) {
  filteredIndices_.clear();
  // Normally a no-op — clear() keeps the capacity fetchAndParseManifest already took — so this
  // only bites if that reserve was skipped. Leaving the list empty shows an empty family list,
  // which is recoverable; growing into a throwing push_back loop is not.
  if (!reserveNoThrow(filteredIndices_, families_.size())) {
    LOG_ERR("FONT", "OOM: %zu filtered indices", families_.size());
    return;
  }
  if (groupListIndex <= 0) {
    for (int familyIndex = 0; familyIndex < static_cast<int>(families_.size()); familyIndex++) {
      filteredIndices_.push_back(familyIndex);
    }
    return;
  }

  const uint32_t groupBit = uint32_t{1} << (groupListIndex - 1);
  for (int familyIndex = 0; familyIndex < static_cast<int>(families_.size()); familyIndex++) {
    if (families_[familyIndex].scriptMask & groupBit) filteredIndices_.push_back(familyIndex);
  }
}

void FontDownloadActivity::enterGroup(const int groupListIndex) {
  closeRouting();
  currentGroupIndex_ = groupListIndex;
  buildFilteredIndices(groupListIndex);
  {
    RenderLock lock(*this);
    nav.reset();
    state_ = FAMILY_LIST;
    rowsDirty_ = true;
  }
}

size_t FontDownloadActivity::totalDownloadSize() const {
  size_t total = 0;
  for (const int familyIndex : filteredIndices_) {
    if (!families_[familyIndex].installed) total += families_[familyIndex].totalSize;
  }
  return total;
}

size_t FontDownloadActivity::totalUpdateSize() const {
  size_t total = 0;
  for (const int familyIndex : filteredIndices_) {
    if (families_[familyIndex].hasUpdate) total += families_[familyIndex].totalSize;
  }
  return total;
}

// Standard CRC32 matching zlib/Python zlib.crc32().
bool FontDownloadActivity::computeFileCrc32(const char* path, uint32_t& outCrc) {
  HalFile f;
  if (!Storage.openFileForRead("FONT", path, f)) {
    return false;
  }
  constexpr size_t BUF_SIZE = 128;
  uint8_t buf[BUF_SIZE];
  uint32_t crc = 0;
  while (f.available()) {
    const int n = f.read(buf, BUF_SIZE);
    if (n <= 0) break;
    crc = esp_rom_crc32_le(crc, buf, static_cast<uint32_t>(n));
  }
  outCrc = crc;
  return true;
}

void FontDownloadActivity::downloadFamily(const DownloadJob& family) {
  {
    RenderLock lock(*this);
    state_ = DOWNLOADING;
    fileProgress_ = 0;
    fileTotal_ = 0;
    cancelRequested_ = false;
    goHomeRequested_ = false;
  }
  requestUpdateAndWait();

  if (!fontInstaller_.ensureFamilyDir(family.name.c_str())) {
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = "Failed to create font directory";
    return;
  }

  for (size_t i = 0; i < family.files.size(); i++) {
    const auto& file = family.files[i];

    {
      RenderLock lock(*this);
      fileProgress_ = 0;
      fileTotal_ = file.size;
      lastRenderedPercent_ = PERCENT_UNRENDERED;  // per-file: progress restarts at 0
    }
    requestUpdateAndWait();

    char destPath[128];
    FontInstaller::buildFontPath(family.name.c_str(), file.name.c_str(), destPath, sizeof(destPath));

    // Resume: a size already on disk with the manifest's CRC is not fetched again. Without this
    // a family that failed on its last size re-downloaded every earlier size on the retry —
    // megabytes, and a fresh set of TLS handshakes to lose — before reaching the one that
    // failed. A stale or truncated file fails the CRC and is fetched normally.
    uint32_t installedCrc = 0;
    if (Storage.exists(destPath) && computeFileCrc32(destPath, installedCrc) && installedCrc == file.crc32) {
      LOG_DBG("FONT", "Already installed, skipping: %s", file.name.c_str());
      continue;
    }

    std::string url = baseUrl_ + file.name;

    // Same heap reclaim as the manifest fetch: free the glyph cache AND the resident SD
    // font tables so the per-file TLS handshake + redirect header parse have a large enough
    // contiguous block (the list render before this loop repopulated the cache). See
    // fetchAndParseManifest for the full rationale (github's 3.5KB CSP header).
    if (auto* fcm = renderer.getFontCacheManager()) {
      fcm->clearCache();
    }
    sdFontSystem.unloadFonts(renderer);

    // The reclaim above is measurably not the binding constraint on X3: a 09-07 capture shows
    // largest8 pinned at 7,156 across eight consecutive files while free heap sat near 28 KB,
    // and every handshake then failed with wolfSSL MEMORY_E (-125). Free-but-not-contiguous is
    // a block-count problem, so log the count the same way HomeActivity's cover pass does —
    // free/largest alone cannot distinguish "something big is held" from "the heap is in
    // pieces", and that distinction decides whether the manifest working set has to move into
    // an arena. Grep FRAG in opds_debug.txt.
    {
      multi_heap_info_t info;
      heap_caps_get_info(&info, MALLOC_CAP_8BIT);
      SdDebugLog::log("FRAG", "font-file free=%u largest=%u blocks=%u file=%s", (unsigned)info.total_free_bytes,
                      (unsigned)info.largest_free_block, (unsigned)info.free_blocks, file.name.c_str());
    }

    auto result = HttpDownloader::downloadToFile(
        url, destPath,
        [this](size_t downloaded, size_t total) {
          fileProgress_ = downloaded;
          // Keep the manifest-provided size if the server sends no Content-Length
          // (total==0); progress now fires for unknown-size downloads too.
          if (total > 0) fileTotal_ = total;
          mappedInput.update();
          if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
              mappedInput.wasPressed(MappedInputManager::Button::Back)) {
            cancelRequested_ = true;
          }
          // This update() consumes the one-shot home event before the central
          // ActivityManager dispatch can see it, so honor it here: abort the
          // download, then exit to home once the abort unwinds.
          if (mappedInput.wasHomeGesture()) {
            cancelRequested_ = true;
            goHomeRequested_ = true;
          }
          // Repaint at most once per PROGRESS_STEP_PERCENT. This callback used to request an
          // update unconditionally, i.e. as fast as the panel would accept one — ~435ms per
          // full-screen refresh, continuously, for the whole download. requestUpdate(true) is
          // non-blocking (ActivityManager.cpp:349, xTaskNotify), so the cost does not land in
          // this callback; it lands in the next socket wait, and on X3 the SD card shares the
          // display SPI bus (BoardConfig.h) so it also sits between the file writes.
          //
          // Percent-based only. A time term here would have to be a FLOOR (AND), never a
          // trigger (OR): an OR clause lets a slower transfer buy itself MORE refreshes and
          // get slower still, which is the regression fixed in 24ffd03c.
          const unsigned int pct = fileTotal_ > 0 ? static_cast<unsigned int>((fileProgress_ * 100) / fileTotal_) : 0;
          const unsigned int step = pct - (pct % PROGRESS_STEP_PERCENT);
          if (step != lastRenderedPercent_ || cancelRequested_) {
            lastRenderedPercent_ = step;
            requestUpdate(true);
          }
        },
        &cancelRequested_, "", "", nullptr, FONT_CA_GITHUB_PEM, FONT_CA_ASSETS_PEM);

    if (result == HttpDownloader::ABORTED) {
      fontInstaller_.deleteFamily(family.name.c_str());
      if (goHomeRequested_) {
        onGoHome();
        return;
      }
      {
        RenderLock lock(*this);
        state_ = FAMILY_LIST;
        // The partial family was just deleted from disk; the reload in runJobs() re-derives
        // installed/hasUpdate from what is actually there.
        rowsDirty_ = true;
      }
      return;
    }

    if (result != HttpDownloader::OK) {
      LOG_ERR("FONT", "Download failed: %s (%d)", file.name.c_str(), result);
      // Only the size that failed, never the whole family. The sizes already fetched are a
      // usable family in their own right — SdCardFontRegistry::scanRoot admits any non-empty
      // set — and discarding them meant a refused handshake on the last size threw away every
      // earlier one, so no retry ever got further than the previous attempt did.
      // The partial destination file must still go: a truncated .cpfont would be discovered
      // as a real size.
      Storage.remove(destPath);
      fontInstaller_.refreshRegistry();
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = "Download failed: " + file.name;
      return;
    }

    uint32_t actualCrc = 0;
    if (!computeFileCrc32(destPath, actualCrc)) {
      LOG_ERR("FONT", "Failed to open file for CRC check: %s", destPath);
      fontInstaller_.deleteFamily(family.name.c_str());
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = "Failed to compute checksum: " + file.name;
      return;
    }
    if (actualCrc != file.crc32) {
      LOG_ERR("FONT", "CRC32 mismatch for %s: got %08x expected %08x", file.name.c_str(), actualCrc, file.crc32);
      fontInstaller_.deleteFamily(family.name.c_str());
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = "Checksum mismatch: " + file.name;
      return;
    }
    LOG_DBG("FONT", "Downloaded %s (size=%zu crc32=%08x)", file.name.c_str(), file.size, actualCrc);

    if (!fontInstaller_.validateCpfontFile(destPath)) {
      LOG_ERR("FONT", "Invalid .cpfont: %s", destPath);
      fontInstaller_.deleteFamily(family.name.c_str());
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = "Invalid font file: " + file.name;
      return;
    }
    currentFileIndex_++;
  }

  fontInstaller_.refreshRegistry();

  {
    RenderLock lock(*this);
    state_ = COMPLETE;
  }
}

void FontDownloadActivity::promptDeleteSelectedFamily() {
  const int pendingDeleteFamilyIndex = familyIndexFromList(nav.selected);
  if (pendingDeleteFamilyIndex < 0 || pendingDeleteFamilyIndex >= static_cast<int>(families_.size())) {
    return;
  }

  std::string heading = tr(STR_DELETE);
  const auto& family = families_[pendingDeleteFamilyIndex];
  std::string body = family.name;
  startActivityForResultNoThrow<ConfirmationActivity>(
      [this](const ActivityResult& result) { onDeleteConfirmationResult(result); }, renderer, mappedInput, heading,
      body);
}

void FontDownloadActivity::onDeleteConfirmationResult(const ActivityResult& result) {
  if (result.isCancelled) {
    requestUpdate();
    return;
  }

  const int familyIndex = familyIndexFromList(nav.selected);
  if (familyIndex < 0) {
    requestUpdate();
    return;
  }
  auto& family = families_[familyIndex];

  if (fontInstaller_.deleteFamily(family.name.c_str()) != FontInstaller::Error::OK) {
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = "Failed to delete font";
  } else {
    fontInstaller_.refreshRegistry();
    family.installed = false;
    family.hasUpdate = false;
    // Unlike the other family_ mutations, this one stays in FAMILY_LIST (no
    // state_ transition to hang the rebuild off), so it must set the flag
    // directly.
    rowsDirty_ = true;
  }

  requestUpdate();
}

bool FontDownloadActivity::isSelectedFamilyDeletable() const {
  if (isDownloadAllRow(nav.selected) || isUpdateAllRow(nav.selected)) return false;
  if (nav.selected < specialRowCount() || nav.selected >= listItemCount()) return false;
  const auto& family = families_[familyIndexFromList(nav.selected)];
  return family.installed && !family.hasUpdate;
}

void FontDownloadActivity::activateSelected() {
  if (filteredIndices_.empty()) return;
  if (isDownloadAllRow(nav.selected)) {
    currentFileIndex_ = 0;
    currentFileTotal_ = 0;
    for (const int familyIndex : filteredIndices_) {
      if (!families_[familyIndex].installed) currentFileTotal_ += families_[familyIndex].files.size();
    }
    downloadAll();
  } else if (isUpdateAllRow(nav.selected)) {
    currentFileIndex_ = 0;
    currentFileTotal_ = 0;
    for (const int familyIndex : filteredIndices_) {
      if (families_[familyIndex].hasUpdate) currentFileTotal_ += families_[familyIndex].files.size();
    }
    updateAll();
  } else {
    // The special rows disappear when a download starts, so a stale selection
    // can map past the family table.
    const int familyIndex = familyIndexFromList(nav.selected);
    if (familyIndex < 0 || familyIndex >= static_cast<int>(families_.size())) return;
    auto& family = families_[familyIndex];
    if (!family.installed || family.hasUpdate) {
      currentFileIndex_ = 0;
      currentFileTotal_ = family.files.size();
      // One-family session, through the same snapshot/release/reload path as the bulk rows:
      // this is the common case, and the one where releasing the manifest buys the most.
      std::vector<DownloadJob>().swap(jobs_);
      jobIndex_ = 0;
      DownloadJob job;
      job.name = family.name;
      if (!reserveNoThrow(job.files, family.files.size()) || !reserveNoThrow(jobs_, 1)) {
        LOG_ERR("FONT", "OOM: download job for %s", family.name.c_str());
        RenderLock lock(*this);
        state_ = ERROR;
        errorMessage_ = tr(STR_MEMORY_ERROR);
        return;
      }
      job.files = family.files;
      jobs_.push_back(std::move(job));
      runJobs();
    } else {
      promptDeleteSelectedFamily();
      return;
    }
  }
  requestUpdateAndWait();
}

void FontDownloadActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints.
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (state_ == FAMILY_LIST && filteredIndices_.empty()) {
    screen.centeredText(tr(STR_NO_FONTS_AVAILABLE), screen.theme().bodyText);
    return;
  }

  if (rowsDirty_) {
    rebuildRowItems();
    rowsDirty_ = false;
  }
  applyInstalledRowDim();

  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the status and the row edge
  syncListViewport(screen, props, /*hasSubtitle=*/state_ == FAMILY_LIST);
  screen.list(props);
}

// Dims installed families, EXCEPT the one under the cursor.
//
// An installed row is drawn dimmed to say "nothing to download here" while
// staying tappable (it opens the delete prompt). The obvious way to express
// that is fui::StateDisabled on the ListItem, and it silently costs the row its
// selection highlight: list() ORs StateSelected into the row's state, but
// BoxStyle::resolve() (FreeInkUICore.h) tests StateDisabled FIRST and returns
// the disabled style without ever looking at selected. Disabled is white-on-
// black-free, selected is a full inversion, so an installed row rendered
// identically whether or not it was the cursor — on a list where every row is
// installed, nothing on screen showed where the cursor was.
//
// So the dim is a per-render decision, not a property of the row: every
// installed family gets it except the selected one, which needs its inversion.
// Cheap enough to redo on every paint (no allocation, one pass over rows that
// already exist), which is why it lives here and not in rebuildRowItems() —
// making the dim part of the rebuild would mean marking rowsDirty_ on every
// cursor move, rebuilding all the label strings for a selection change.
void FontDownloadActivity::applyInstalledRowDim() {
  // Group rows are not families, and familyIndexFromList() answers -1 for any
  // row outside the current group's filtered window.
  if (state_ != FAMILY_LIST) return;
  for (int i = specialRowCount(); i < listItemCount() && i < static_cast<int>(rowItems_.size()); i++) {
    const int familyIndex = familyIndexFromList(i);
    if (familyIndex < 0) continue;
    const auto& family = families_[familyIndex];
    const bool dim = family.installed && !family.hasUpdate && i != nav.selected;
    rowItems_[i].state = dim ? fui::StateDisabled : fui::StateNormal;
  }
}

// Rebuilds rowLabels_/rowItems_ for the visible list. Only called when
// rowsDirty_ is set (families_/state_/group changed since the last build),
// never on every repaint.
void FontDownloadActivity::rebuildRowItems() {
  switch (state_) {
    case GROUP_LIST:
      rebuildGroupRowItems();
      return;
    case FAMILY_LIST:
      rebuildFamilyRowItems();
      return;
    case WIFI_SELECTION:
    case LOADING_MANIFEST:
    case DOWNLOADING:
    case COMPLETE:
    case ERROR:
      rowLabels_.clear();
      rowItems_.clear();
      return;
  }
}

void FontDownloadActivity::rebuildGroupRowItems() {
  const int listSize = groupListItemCount();
  // Both are already at rowCapacity from the manifest parse, so these are no-ops in the normal
  // case. Checked anyway because this runs on every navigation: assign() and reserve() are both
  // throwing, and a screen that draws no rows beats one that reboots.
  if (!reserveNoThrow(rowLabels_, listSize) || !reserveNoThrow(rowItems_, listSize)) {
    LOG_ERR("FONT", "OOM: %d group rows", listSize);
    rowLabels_.clear();
    rowItems_.clear();
    return;
  }
  rowLabels_.assign(listSize, std::string());
  rowItems_.clear();
  for (int rowIndex = 0; rowIndex < listSize; rowIndex++) {
    fui::ListItem item;
    item.label = rowIndex == 0 ? tr(STR_ALL_FONTS) : scriptGroupLabels_[rowIndex - 1].c_str();
    const int memberCount = rowIndex == 0 ? static_cast<int>(families_.size()) : groupMemberCount(rowIndex - 1);
    rowLabels_[rowIndex] = std::to_string(memberCount);
    item.value = rowLabels_[rowIndex].c_str();
    item.actionValue = static_cast<int16_t>(rowIndex);
    rowItems_.push_back(item);
  }
}

void FontDownloadActivity::rebuildFamilyRowItems() {
  const int listSize = listItemCount();
  // See rebuildGroupRowItems: no-op in the normal case, guarded because it runs per navigation.
  if (!reserveNoThrow(rowLabels_, listSize) || !reserveNoThrow(rowItems_, listSize)) {
    LOG_ERR("FONT", "OOM: %d family rows", listSize);
    rowLabels_.clear();
    rowItems_.clear();
    return;
  }
  rowLabels_.assign(listSize, std::string());
  rowItems_.clear();
  for (int i = 0; i < listSize; i++) {
    fui::ListItem item;
    if (isDownloadAllRow(i)) {
      rowLabels_[i] = std::string(tr(STR_DOWNLOAD_ALL)) + " (" + formatSize(totalDownloadSize()) + ")";
      item.label = rowLabels_[i].c_str();
    } else if (isUpdateAllRow(i)) {
      rowLabels_[i] = std::string(tr(STR_UPDATE_ALL)) + " (" + formatSize(totalUpdateSize()) + ")";
      item.label = rowLabels_[i].c_str();
    } else {
      const auto& family = families_[familyIndexFromList(i)];
      item.label = family.name.c_str();
      if (!family.description.empty()) item.subtitle = family.description.c_str();
      if (family.hasUpdate) {
        item.value = tr(STR_UPDATE_AVAILABLE);
      } else if (family.installed) {
        item.value = tr(STR_INSTALLED);
        // The dim itself is applied per-render by applyInstalledRowDim(), not
        // here: it has to come off whichever row is selected. See that function.
      }
    }
    item.actionValue = static_cast<int16_t>(i);
    rowItems_.push_back(item);
  }
}

// --- Input handling ---

bool FontDownloadActivity::handleCustomInput() {
  if (state_ == GROUP_LIST || state_ == FAMILY_LIST) {
    // The base list protocol (Back/Confirm, touch routing, swipe scroll,
    // button navigation) handles both list states.
    return false;
  }

  if (state_ == COMPLETE) {
    int x = 0;
    int y = 0;
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
      {
        RenderLock lock(*this);
        state_ = FAMILY_LIST;
        rowsDirty_ = true;  // the completed download changed installed/hasUpdate
      }
      requestUpdate();
    }
  } else if (state_ == ERROR) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      {
        RenderLock lock(*this);
        state_ = FAMILY_LIST;
        rowsDirty_ = true;  // the failed download reset installed/hasUpdate
      }
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (jobIndex_ < jobs_.size()) {
        retryCurrentJob();
        requestUpdateAndWait();
        return true;
      } else {
        {
          RenderLock lock(*this);
          state_ = FAMILY_LIST;
          rowsDirty_ = true;
        }
        requestUpdate();
      }
    } else {
      int x = 0;
      int y = 0;
      if (mappedInput.wasScreenTapped(x, y)) {
        if (jobIndex_ < jobs_.size()) {
          retryCurrentJob();
          requestUpdateAndWait();
          return true;
        }
        {
          RenderLock lock(*this);
          state_ = FAMILY_LIST;
          rowsDirty_ = true;
        }
        requestUpdate();
      }
    }
  }

  return true;
}

// --- Rendering ---

std::string FontDownloadActivity::formatSize(size_t bytes) {
  char buf[32];
  if (bytes >= 1024 * 1024) {
    snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else if (bytes >= 1024) {
    snprintf(buf, sizeof(buf), "%.0f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    snprintf(buf, sizeof(buf), "%zu B", bytes);
  }
  return buf;
}

void FontDownloadActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  // Which script group the family list is showing, as the header subtitle.
  // LOCAL(feat): upstream draws its header here, unconditionally, before the
  // DOWNLOADING branch below. Doing that here would paint the themed band twice
  // and put its solid rule back on screen for the whole of a download, which is
  // exactly the dwell the plain-title branch exists to avoid. Only the subtitle
  // is computed here; the draw stays in the branch that already owns it.
  const char* headerSubtitle = nullptr;
  if (state_ == FAMILY_LIST && hasGroupScreen()) {
    const int scriptGroupIndex = groupNav_.selected - 1;
    headerSubtitle = scriptGroupIndex >= 0 && scriptGroupIndex < static_cast<int>(scriptGroupLabels_.size())
                         ? scriptGroupLabels_[scriptGroupIndex].c_str()
                         : tr(STR_ALL_FONTS);
  }

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  if (state_ == DOWNLOADING) {
    // Plain title instead of the themed header for the duration of the download. GUI.drawHeader
    // draws a solid black full-width rule under a titled band (BaseTheme.cpp:484-487 — 3px on
    // Lyra/Lyra-3/Vega, 0 on Classic/RoundedRaff) at y = topPadding + headerHeight - 3. A family
    // download holds this one layout across every file in it, and that dwell — not the frame
    // count — is what sets e-ink image sticking, which surfaces later as a faint line across the
    // sleep wallpaper. Same treatment as SdFirmwareUpdateActivity, confirmed on device.
    renderer.drawCenteredText(UI_10_FONT_ID, metrics.topPadding + (metrics.headerHeight - lineHeight) / 2,
                              tr(STR_FONT_BROWSER), true, EpdFontFamily::BOLD);
  } else {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FONT_BROWSER),
                   headerSubtitle);
  }

  const auto contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const auto centerY = (pageHeight - lineHeight) / 2;

  if (state_ == LOADING_MANIFEST) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_LOADING_FONT_LIST));
  } else if (state_ == GROUP_LIST) {
    renderUi();
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == FAMILY_LIST) {
    renderUi();

    const bool hasVisibleFamilies = !filteredIndices_.empty();
    const char* confirmLabel = !hasVisibleFamilies            ? ""
                               : isSelectedFamilyDeletable()  ? tr(STR_DELETE)
                               : isUpdateAllRow(nav.selected) ? tr(STR_UPDATE)
                                                              : tr(STR_DOWNLOAD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, hasVisibleFamilies ? tr(STR_DIR_UP) : "",
                                              hasVisibleFamilies ? tr(STR_DIR_DOWN) : "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == DOWNLOADING) {
    // jobs_ outlives the released manifest, which families_ does not.
    static const std::string kNoFamily;
    const std::string& familyName = jobIndex_ < jobs_.size() ? jobs_[jobIndex_].name : kNoFamily;

    std::string statusText = std::string(tr(STR_DOWNLOADING)) + " " + familyName + " (" +
                             std::to_string(currentFileIndex_ + 1) + "/" + std::to_string(currentFileTotal_) + ")";
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, statusText.c_str());

    float progress = 0;
    if (fileTotal_ > 0) {
      progress = static_cast<float>(fileProgress_) / static_cast<float>(fileTotal_);
    }

    int barY = centerY + metrics.verticalSpacing;
    GUI.drawProgressBar(
        renderer,
        Rect{metrics.contentSidePadding, barY, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
        static_cast<int>(progress * 100), 100);

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == COMPLETE) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_FONT_INSTALLED), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, tr(STR_FONT_INSTALL_FAILED), true,
                              EpdFontFamily::BOLD);
    if (!errorMessage_.empty()) {
      // Wrap the detail (some include a filename, e.g. "Download failed: <name>")
      // over up to 3 lines instead of a single centered line that runs off both edges.
      const auto errLines = renderer.wrappedText(UI_10_FONT_ID, errorMessage_.c_str(), pageWidth - 40, 3);
      int errY = centerY + metrics.verticalSpacing;
      for (const auto& line : errLines) {
        renderer.drawCenteredText(UI_10_FONT_ID, errY, line.c_str());
        errY += lineHeight;
      }
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
