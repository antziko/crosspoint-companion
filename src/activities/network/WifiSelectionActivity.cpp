#include "WifiSelectionActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <Logging.h>
#include <SdDebugLog.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_wifi.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "WifiCredentialStore.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_ROW = 1;
constexpr fui::ActionId ACTION_SCAN = 2;
constexpr fui::ActionId ACTION_PROMPT = 3;
}  // namespace

WifiSelectionActivity::WifiSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             const bool autoConnect)
    : Activity("WifiSelection", renderer, mappedInput), UiAppHost(renderer), allowAutoConnect(autoConnect) {}

void WifiSelectionActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<WifiSelectionActivity*>(user);
  if (self->state != WifiSelectionState::NETWORK_LIST) return;
  if (event.value < 0 || event.value >= static_cast<int16_t>(self->networks.size())) return;
  self->selectedNetworkIndex = static_cast<size_t>(event.value);
  // Long-press a saved network to forget it (mirrors the Left-button hold in loop()).
  if (event.longPress) {
    if (self->networks[self->selectedNetworkIndex].hasSavedPassword) {
      self->selectedSSID = self->networks[self->selectedNetworkIndex].ssid;
      self->state = WifiSelectionState::FORGET_PROMPT;
      self->forgetPromptSelection = 0;  // Default to "Cancel"
      self->app.clearTapFlash();
      self->requestUpdate();
    }
    return;
  }
  // Selection leaves this screen (password entry / connecting); a lingering
  // flash would gray an unrelated row.
  self->app.clearTapFlash();
  self->selectNetwork(static_cast<int>(self->selectedNetworkIndex));
}

void WifiSelectionActivity::onScanEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<WifiSelectionActivity*>(user);
  if (self->state != WifiSelectionState::NETWORK_LIST) return;
  self->app.clearTapFlash();  // the scan screen replaces this one
  self->startWifiScan();
}

void WifiSelectionActivity::onPromptEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<WifiSelectionActivity*>(user);
  if (self->state == WifiSelectionState::SAVE_PROMPT) {
    self->savePromptSelection = event.value;
    self->app.clearTapFlash();  // the action leaves this screen
    if (self->savePromptSelection == 0) {
      RenderLock lock(*self);
      WIFI_STORE.addCredential(self->selectedSSID, self->enteredPassword);
    }
    self->onComplete(true);
    return;
  }
  if (self->state == WifiSelectionState::FORGET_PROMPT) {
    self->forgetPromptSelection = event.value;
    self->app.clearTapFlash();  // the action leaves this screen
    if (self->forgetPromptSelection == 1) {
      RenderLock lock(*self);
      WIFI_STORE.removeCredential(self->selectedSSID);
      const auto network = find_if(self->networks.begin(), self->networks.end(),
                                   [self](const WifiNetworkInfo& net) { return net.ssid == self->selectedSSID; });
      if (network != self->networks.end()) {
        network->hasSavedPassword = false;
      }
    }
    self->startWifiScan();
  }
}

void WifiSelectionActivity::onEnter() {
  Activity::onEnter();

  // Load saved WiFi credentials - SD card operations need lock as we use SPI
  // for both
  {
    RenderLock lock(*this);
    WIFI_STORE.loadFromFile();
  }

  // Reset state
  selectedNetworkIndex = 0;
  networks.clear();
  networkStatuses.clear();
  networkRowItems.clear();
  realNetworkCount = 0;
  state = WifiSelectionState::SCANNING;
  selectedSSID.clear();
  connectedIP.clear();
  connectionError.clear();
  enteredPassword.clear();
  usedSavedPassword = false;
  savePromptSelection = 0;
  forgetPromptSelection = 0;
  autoConnecting = false;
  manualNetworkListRequested = false;
  lowMemoryAbort = false;
  scanStartTime = 0;
  autoAttemptedSsids.clear();
  const size_t savedCredentialCount = WIFI_STORE.getCredentialCount();
  autoAttemptedSsids.reserve(savedCredentialCount);

  // Cache MAC address for display. Read the hardware-derived station MAC directly:
  // WiFi.macAddress() depends on the STA netif already existing, but this screen is
  // often entered while WiFi is still off, which yields all zeroes.
  uint8_t mac[6] = {};
  char macStr[64];
  const esp_err_t macResult = esp_read_mac(mac, ESP_MAC_WIFI_STA);
  if (macResult == ESP_OK) {
    snprintf(macStr, sizeof(macStr), "%s %02x-%02x-%02x-%02x-%02x-%02x", tr(STR_MAC_ADDRESS), mac[0], mac[1], mac[2],
             mac[3], mac[4], mac[5]);
  } else {
    LOG_ERR("WIFI", "Failed to read station MAC (err=%d)", static_cast<int>(macResult));
    snprintf(macStr, sizeof(macStr), "%s --", tr(STR_MAC_ADDRESS));
  }
  cachedMacAddress = std::string(macStr);

  listNav.reset();
  resetUi();
  app.on(ACTION_ROW, &WifiSelectionActivity::onRowEvent, this);
  app.on(ACTION_SCAN, &WifiSelectionActivity::onScanEvent, this);
  app.on(ACTION_PROMPT, &WifiSelectionActivity::onPromptEvent, this);
  app.setScreen(&WifiSelectionActivity::listScreen, this);

  // If WiFi is already up (e.g. a prior network activity left the link
  // connected), reuse it and skip the scan/selection list entirely. popActivity()
  // is deferred (sets a pending Pop handled next loop), so finishing from onEnter
  // is safe and won't delete this activity mid-entry.
  if (WiFi.status() == WL_CONNECTED) {
    const IPAddress ip = WiFi.localIP();
    if (ip != IPAddress(0, 0, 0, 0)) {
      selectedSSID = WiFi.SSID().c_str();
      connectedIP = ip.toString().c_str();
      state = WifiSelectionState::CONNECTED;
      LOG_DBG("WIFI", "Already connected to %s (%s); skipping selection list", selectedSSID.c_str(),
              connectedIP.c_str());
      requestUpdate();
      onComplete(true);
      return;
    }
  }

  // Trigger first update to show scanning message
  requestUpdate();

  // Attempt to auto-connect to known networks. Try the last successful
  // network first for speed, then scan and try any visible saved networks by
  // signal strength. The user can interrupt this and show the scan result.
  if (allowAutoConnect && savedCredentialCount != 0) {
    const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
    if (!lastSsid.empty()) {
      const auto cred = WIFI_STORE.findCredential(lastSsid);
      if (cred && tryAutoConnectCredential(*cred)) {
        return;
      }
    }

    startWifiScan(true);
    return;
  }

  // Trigger first update to show scanning message
  requestUpdate();

  // Always present the scanned network list so the user picks a network.
  // Saved networks still connect with one tap (selectNetwork reuses the
  // stored password); we no longer silently auto-connect to the last SSID.
  startWifiScan();
}

void WifiSelectionActivity::onExit() {
  Activity::onExit();

  LOG_DBG("WIFI", "Free heap at onExit start: %d bytes", ESP.getFreeHeap());

  // Stop any ongoing WiFi scan
  LOG_DBG("WIFI", "Deleting WiFi scan...");
  WiFi.scanDelete();
  LOG_DBG("WIFI", "Free heap after scanDelete: %d bytes", ESP.getFreeHeap());

  // Note: We do NOT disconnect WiFi here - the parent activity
  // (CrossPointWebServerActivity) manages WiFi connection state. We just clean
  // up the scan and task.

  LOG_DBG("WIFI", "Free heap at onExit end: %d bytes", ESP.getFreeHeap());
}

bool WifiSelectionActivity::hasHeapForScan() {
  return ESP.getFreeHeap() >= SCAN_MIN_FREE_HEAP &&
         heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) >= SCAN_MIN_LARGEST_BLOCK;
}

void WifiSelectionActivity::failWithLowMemory() {
  lowMemoryAbort = true;
  autoConnecting = false;
  manualNetworkListRequested = false;
  connectionError = tr(STR_ERROR_LOW_MEMORY);
  state = WifiSelectionState::CONNECTION_FAILED;
  requestUpdate();
}

void WifiSelectionActivity::startWifiScan(const bool autoScan) {
  autoConnecting = autoScan;
  manualNetworkListRequested = false;
  listNav.reset();
  state = WifiSelectionState::SCANNING;
  networks.clear();
  requestUpdate();

  // Gate the scan, not the radio. Joining a known SSID needs none of the scan's
  // per-AP buffers, so an auto-connect run below the floor walks the stored
  // credentials blind rather than giving up — that is the KOReader-sync path,
  // which arrives with the least headroom and never needed a network list.
  // Only an explicit "show me what's out there" has to fail here.
  if (!hasHeapForScan()) {
    const unsigned freeHeap = ESP.getFreeHeap();
    const unsigned largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    LOG_ERR("WIFI", "Scan skipped: low heap (free=%u largest=%u)", freeHeap, largest);
    SdDebugLog::log("WIFI", "scan skipped: low heap free=%u largest=%u auto=%d", freeHeap, largest,
                    static_cast<int>(autoScan));
    if (autoScan && tryNextSavedCredentialBlind()) return;
    failWithLowMemory();
    return;
  }

  // Set WiFi mode to station
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  // Start async scan
  scanStartTime = millis();
  if (WiFi.scanNetworks(true) == WIFI_SCAN_FAILED) {  // true = async scan
    // Nothing to do beyond the log: scanComplete() reports WIFI_SCAN_FAILED on
    // the next loop and processWifiScanResults() takes the failure path.
    LOG_ERR("WIFI", "scanNetworks() refused to start (free=%u)", (unsigned)ESP.getFreeHeap());
  }
}

void WifiSelectionActivity::processWifiScanResults() {
  const int16_t scanResult = WiFi.scanComplete();

  if (scanResult == WIFI_SCAN_RUNNING && millis() - scanStartTime <= SCAN_TIMEOUT_MS) {
    // Scan still in progress, still within budget
    return;
  }

  // Failed, or still running past our own budget. Left to arduino-esp32 this
  // would sit on WIFI_SCAN_RUNNING until its 60 s _scanTimeout, which is what
  // parked the sync path on "Finding saved Wi-Fi..." for a quarter minute.
  if (scanResult == WIFI_SCAN_FAILED || scanResult == WIFI_SCAN_RUNNING) {
    if (scanResult == WIFI_SCAN_RUNNING) {
      LOG_ERR("WIFI", "Scan timed out after %lums", SCAN_TIMEOUT_MS);
      SdDebugLog::log("WIFI", "scan timeout free=%u largest=%u", (unsigned)ESP.getFreeHeap(),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
      // Abort the driver-side scan, not just the Arduino bookkeeping: scanDelete()
      // clears WIFI_SCANNING_BIT (so a late _scanDone() no-ops and frees nothing
      // we still point at), but the IDF scan would keep running and make the
      // WiFi.begin() in the blind-credential fallback below fail with
      // ESP_ERR_WIFI_STATE.
      esp_wifi_scan_stop();
    }
    WiFi.scanDelete();  // drop any partial driver buffers before falling back
    networks.clear();
    realNetworkCount = 0;
    // An auto-connect run still has stored credentials it can join without a
    // scan; only fall through to the (now empty) list when none are left.
    if (autoConnecting && !manualNetworkListRequested && tryNextSavedCredentialBlind()) {
      return;
    }
    appendHiddenNetworkEntry();
    rebuildNetworkRowItems();
    autoConnecting = false;
    manualNetworkListRequested = false;
    state = WifiSelectionState::NETWORK_LIST;
    selectedNetworkIndex = 0;
    requestUpdate();
    return;
  }

  // Scan complete. Deduplicate by SSID (keep strongest signal) directly in
  // `networks` with a linear scan instead of a std::map. This path can run with
  // a badly fragmented heap (e.g. after the home-screen cover-render churn): the
  // old std::map + per-entry copy allocated an RB-tree node and a duplicate SSID
  // string per network, and aborted under OOM during the result processing /
  // sort (crash: __throw_out_of_range from libstdc++ containers, free heap
  // minEver ~916 B). A reserved vector with move-construction roughly halves the
  // allocations, and MAX_NETWORKS bounds the list (and its heap use) in noisy RF.
  static constexpr size_t MAX_NETWORKS = 40;

  networks.clear();
  // reserve() aborts under -fno-exceptions, so size the reservation to what the
  // heap can actually hand back in one block rather than to the AP count. The
  // /2 leaves room for each entry's SSID string, which allocates separately.
  // A crowded band on a tight heap then yields a shorter list instead of a
  // reboot; the loop below is bounded by the same figure so no push_back can
  // grow past it.
  const size_t seenAps = static_cast<size_t>(std::max<int16_t>(scanResult, 0));
  const size_t affordable = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / (sizeof(WifiNetworkInfo) * 2);
  const size_t networkCapacity = std::min({seenAps, MAX_NETWORKS, affordable});
  if (networkCapacity < seenAps) {
    LOG_ERR("WIFI", "Scan list capped at %u of %u APs (largest=%u)", (unsigned)networkCapacity, (unsigned)seenAps,
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  }
  networks.reserve(networkCapacity);

  for (int i = 0; i < scanResult; i++) {
    std::string ssid = WiFi.SSID(i).c_str();
    // Skip hidden networks (empty SSID)
    if (ssid.empty()) {
      continue;
    }
    const int32_t rssi = WiFi.RSSI(i);

    // Already seen this SSID? Keep the stronger signal (and its encryption flag).
    auto existing =
        std::find_if(networks.begin(), networks.end(), [&ssid](const WifiNetworkInfo& n) { return n.ssid == ssid; });
    if (existing != networks.end()) {
      if (rssi > existing->rssi) {
        existing->rssi = rssi;
        existing->isEncrypted = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
      }
      continue;
    }

    // New SSID. Stop adding once the reserved capacity is full, but keep scanning
    // so existing entries can still be upgraded to a stronger signal above.
    // Bounded by networkCapacity, not MAX_NETWORKS: growing past the reservation
    // would reallocate, and that reallocation is the abort we just sized around.
    if (networks.size() >= networkCapacity) {
      continue;
    }

    WifiNetworkInfo network;
    network.rssi = rssi;
    network.isEncrypted = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    network.hasSavedPassword = WIFI_STORE.hasSavedCredential(ssid);
    network.ssid = std::move(ssid);
    networks.push_back(std::move(network));
  }

  // Free the driver's scan buffers before sorting to reclaim heap headroom for
  // the sort (the previous abort site).
  WiFi.scanDelete();

  // Sort: saved-password networks first, then by signal strength (strongest first)
  std::sort(networks.begin(), networks.end(), [](const WifiNetworkInfo& a, const WifiNetworkInfo& b) {
    if (a.hasSavedPassword != b.hasSavedPassword) {
      return a.hasSavedPassword;
    }
    return a.rssi > b.rssi;
  });

  realNetworkCount = networks.size();
  appendHiddenNetworkEntry();
  rebuildNetworkRowItems();

  WiFi.scanDelete();

  if (autoConnecting && !manualNetworkListRequested && tryNextSavedNetworkFromScan()) {
    return;
  }

  autoConnecting = false;
  manualNetworkListRequested = false;
  state = WifiSelectionState::NETWORK_LIST;
  selectedNetworkIndex = 0;
  requestUpdate();
}

void WifiSelectionActivity::appendHiddenNetworkEntry() {
  // Synthetic list entry that lets the user type an SSID that is not broadcast.
  // ESP32 can join hidden APs as long as the SSID is supplied to WiFi.begin().
  WifiNetworkInfo placeholder;
  placeholder.rssi = 0;
  placeholder.isEncrypted = true;  // Treated as encrypted; an empty password still connects open APs
  placeholder.hasSavedPassword = false;
  placeholder.isHiddenPlaceholder = true;
  networks.push_back(std::move(placeholder));
}

// Derives networkStatuses/networkRowItems from `networks`. Called whenever
// `networks` changes (both branches of processWifiScanResults()) so
// buildListScreen() reuses the cached rows on every repaint instead of
// re-deriving a "+ * ||||" status string per network each time.
void WifiSelectionActivity::rebuildNetworkRowItems() {
  networkStatuses.assign(networks.size(), std::string());
  networkRowItems.clear();
  networkRowItems.reserve(networks.size());
  for (size_t i = 0; i < networks.size(); i++) {
    const auto& network = networks[i];
    if (!network.isHiddenPlaceholder) {
      networkStatuses[i] = std::string(network.hasSavedPassword ? "+ " : "") + (network.isEncrypted ? "* " : "") +
                           getSignalStrengthIndicator(network.rssi);
    }
    fui::ListItem item;
    item.label = network.isHiddenPlaceholder ? tr(STR_ADD_HIDDEN_NETWORK) : network.ssid.c_str();
    if (!networkStatuses[i].empty()) item.value = networkStatuses[i].c_str();
    item.actionValue = static_cast<int16_t>(i);
    networkRowItems.push_back(item);
  }
}

void WifiSelectionActivity::selectNetwork(const int index) {
  if (index < 0 || index >= static_cast<int>(networks.size())) {
    return;
  }

  const auto& network = networks[index];

  // Synthetic "Add hidden network..." entry: prompt the user to type the SSID first
  if (network.isHiddenPlaceholder) {
    promptHiddenSsid();
    return;
  }

  selectedSSID = network.ssid;
  selectedRequiresPassword = network.isEncrypted;
  usedSavedPassword = false;
  enteredPassword.clear();
  autoConnecting = false;

  // Check if we have saved credentials for this network
  const auto savedCred = WIFI_STORE.findCredential(selectedSSID);
  if (savedCred && !savedCred->password.empty()) {
    // Use saved password - connect directly
    enteredPassword = savedCred->password;
    usedSavedPassword = true;
    LOG_DBG("WiFi", "Using saved password for %s, length: %zu", selectedSSID.c_str(), enteredPassword.size());
    attemptConnection();
    return;
  }

  if (selectedRequiresPassword) {
    promptPasswordEntry();
  } else {
    // Connect directly for open networks
    attemptConnection();
  }
}

void WifiSelectionActivity::promptPasswordEntry() {
  // Show password entry
  state = WifiSelectionState::PASSWORD_ENTRY;
  // Don't allow screen updates while changing activity
  startActivityForResultNoThrow<KeyboardEntryActivity>(
      [this](const ActivityResult& result) {
        if (result.isCancelled) {
          state = WifiSelectionState::NETWORK_LIST;
        } else {
          enteredPassword = std::get<KeyboardResult>(result.data).text;
          // state will be updated in next loop iteration
        }
      },
      renderer, mappedInput, tr(STR_ENTER_WIFI_PASSWORD),
      "",  // No initial text
      64,  // Max password length
      InputType::Password);
}

void WifiSelectionActivity::promptHiddenSsid() {
  selectedSSID.clear();
  selectedRequiresPassword = true;  // Hidden networks are usually encrypted; empty password still joins open APs
  usedSavedPassword = false;
  enteredPassword.clear();
  autoConnecting = false;

  // Suppress rendering during the activity transition (see render()).
  state = WifiSelectionState::HIDDEN_SSID_ENTRY;
  startActivityForResultNoThrow<KeyboardEntryActivity>(
      [this](const ActivityResult& result) {
        if (result.isCancelled) {
          state = WifiSelectionState::NETWORK_LIST;
          return;
        }
        selectedSSID = std::get<KeyboardResult>(result.data).text;
        if (selectedSSID.empty()) {
          state = WifiSelectionState::NETWORK_LIST;
        }
        // Otherwise stay in HIDDEN_SSID_ENTRY; loop() continues the flow.
      },
      renderer, mappedInput, tr(STR_ENTER_WIFI_SSID),
      "",  // No initial text
      32,  // Max SSID length (IEEE 802.11: 32 bytes)
      InputType::Text);
}

bool WifiSelectionActivity::hasAttemptedAutoSsid(const std::string& ssid) const {
  return std::find(autoAttemptedSsids.begin(), autoAttemptedSsids.end(), ssid) != autoAttemptedSsids.end();
}

bool WifiSelectionActivity::tryAutoConnectCredential(const WifiCredential& cred) {
  if (hasAttemptedAutoSsid(cred.ssid)) {
    return false;
  }

  LOG_DBG("WIFI", "Attempting saved network: %s", cred.ssid.c_str());
  autoAttemptedSsids.push_back(cred.ssid);
  selectedSSID = cred.ssid;
  enteredPassword = cred.password;
  selectedRequiresPassword = !cred.password.empty();
  usedSavedPassword = true;
  autoConnecting = true;
  manualNetworkListRequested = false;
  attemptConnection();
  requestUpdate();
  return true;
}

bool WifiSelectionActivity::tryNextSavedNetworkFromScan() {
  for (const auto& network : networks) {
    if (!network.hasSavedPassword || hasAttemptedAutoSsid(network.ssid)) {
      continue;
    }

    const auto cred = WIFI_STORE.findCredential(network.ssid);
    if (cred && tryAutoConnectCredential(*cred)) {
      return true;
    }
  }
  return false;
}

// Sourced from the credential store rather than the scan list, for when there is
// no scan to draw on (unaffordable, failed, or timed out). WiFi.begin() with a
// known SSID needs none of the scan's per-AP buffers, which is the whole point
// on the sync path. Ordering is store order rather than signal strength — there
// is no RSSI to sort by without a scan. tryAutoConnectCredential() skips SSIDs
// already tried this session, so the walk always terminates.
bool WifiSelectionActivity::tryNextSavedCredentialBlind() {
  const size_t count = WIFI_STORE.getCredentialCount();
  for (size_t i = 0; i < count; i++) {
    const auto cred = WIFI_STORE.getCredentialAt(i);
    if (cred && tryAutoConnectCredential(*cred)) {
      return true;
    }
  }
  return false;
}

void WifiSelectionActivity::handleAutoConnectFailure() {
  LOG_DBG("WIFI", "Saved network failed: %s", selectedSSID.c_str());
  WiFi.disconnect();

  if (!networks.empty()) {
    if (tryNextSavedNetworkFromScan()) {
      return;
    }
    autoConnecting = false;
    state = WifiSelectionState::NETWORK_LIST;
    selectedNetworkIndex = 0;
    requestUpdate();
    return;
  }

  startWifiScan(true);
}

void WifiSelectionActivity::showNetworkListFromAutoConnect() {
  LOG_DBG("WIFI", "User requested manual network list");
  WiFi.disconnect();
  autoConnecting = false;
  manualNetworkListRequested = true;

  if (networks.empty()) {
    startWifiScan(false);
    return;
  }

  state = WifiSelectionState::NETWORK_LIST;
  selectedNetworkIndex = 0;
  requestUpdate();
}

void WifiSelectionActivity::attemptConnection() {
  state = autoConnecting ? WifiSelectionState::AUTO_CONNECTING : WifiSelectionState::CONNECTING;
  connectionStartTime = millis();
  connectedIP.clear();
  connectionError.clear();
  requestUpdate();

  WiFi.persistent(false);  // Credentials are managed by WifiCredentialStore; suppress SDK NVS auto-connect
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);  // Abort any in-progress SDK auto-connect and clear NVS-saved SSID
  delay(100);

  // Scan all channels so networks with multiple APs use the strongest matching
  // BSSID instead of the first match found by the framework's default fast scan.
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);

  // Set hostname so routers show "CrossPoint-Reader-AABBCCDDEEFF" instead of "esp32-XXXXXXXXXXXX"
  uint8_t mac[6] = {};
  const esp_err_t macResult = esp_read_mac(mac, ESP_MAC_WIFI_STA);
  if (macResult == ESP_OK) {
    char hostname[sizeof("CrossPoint-Reader-") + 12];
    snprintf(hostname, sizeof(hostname), "CrossPoint-Reader-%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5]);
    WiFi.setHostname(hostname);
  } else {
    LOG_ERR("WIFI", "Failed to read station MAC for hostname (err=%d)", static_cast<int>(macResult));
  }

  if (selectedRequiresPassword && !enteredPassword.empty()) {
    WiFi.begin(selectedSSID.c_str(), enteredPassword.c_str());
  } else {
    WiFi.begin(selectedSSID.c_str());
  }
}

void WifiSelectionActivity::checkConnectionStatus() {
  if (state != WifiSelectionState::CONNECTING && state != WifiSelectionState::AUTO_CONNECTING) {
    return;
  }

  const wl_status_t status = WiFi.status();

  if (status == WL_CONNECTED) {
    // Successfully connected
    IPAddress ip = WiFi.localIP();
    char ipStr[16];
    snprintf(ipStr, sizeof(ipStr), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
    connectedIP = ipStr;
    autoConnecting = false;

#if defined(ENABLE_SERIAL_LOG) && LOG_LEVEL >= 2
    uint8_t connectedBssid[6] = {};
    WiFi.BSSID(connectedBssid);
    LOG_DBG("WIFI", "Connected BSSID: %02x:%02x:%02x:%02x:%02x:%02x, channel: %d, RSSI: %d dBm",
            static_cast<unsigned>(connectedBssid[0]), static_cast<unsigned>(connectedBssid[1]),
            static_cast<unsigned>(connectedBssid[2]), static_cast<unsigned>(connectedBssid[3]),
            static_cast<unsigned>(connectedBssid[4]), static_cast<unsigned>(connectedBssid[5]), WiFi.channel(),
            WiFi.RSSI());
#endif

    // X3: sync once (DS3231 persists across power cycles; ~2 ppm drift is negligible).
    // X4: sync on every WiFi connect — no hardware RTC, so time is lost on each deep sleep.
    {
      const bool shouldSync = halClock.hasHardwareRtc() ? !SETTINGS.clockHasBeenSynced : !halClock.isSystemTimeValid();
      if (shouldSync && halClock.syncFromNTP()) {
        if (halClock.hasHardwareRtc()) {
          SETTINGS.clockHasBeenSynced = 1;
          SETTINGS.saveToFile();
        }
      }
    }

    // Save this as the last connected network - SD card operations need lock as
    // we use SPI for both
    {
      RenderLock lock(*this);
      WIFI_STORE.setLastConnectedSsid(selectedSSID);
    }

    // If we entered a new password, ask if user wants to save it
    // Otherwise, immediately complete so parent can start web server
    if (!usedSavedPassword && !enteredPassword.empty()) {
      state = WifiSelectionState::SAVE_PROMPT;
      savePromptSelection = 0;  // Default to "Yes"
      requestUpdate();
    } else {
      // Using saved password or open network - complete immediately
      LOG_DBG("WIFI",
              "Connected with saved/open credentials, "
              "completing immediately");
      onComplete(true);
    }
    return;
  }

  if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) {
    connectionError = tr(STR_ERROR_GENERAL_FAILURE);
    if (status == WL_NO_SSID_AVAIL) {
      connectionError = tr(STR_ERROR_NETWORK_NOT_FOUND);
    }
    if (autoConnecting) {
      handleAutoConnectFailure();
      return;
    }
    state = WifiSelectionState::CONNECTION_FAILED;
    requestUpdate();
    return;
  }

  // Check for timeout
  const unsigned long timeoutMs = autoConnecting ? AUTO_CONNECTION_TIMEOUT_MS : CONNECTION_TIMEOUT_MS;
  if (millis() - connectionStartTime > timeoutMs) {
    WiFi.disconnect();
    connectionError = tr(STR_ERROR_CONNECTION_TIMEOUT);
    if (autoConnecting) {
      handleAutoConnectFailure();
      return;
    }
    state = WifiSelectionState::CONNECTION_FAILED;
    requestUpdate();
    return;
  }
}

void WifiSelectionActivity::loop() {
  // Check scan progress
  if (state == WifiSelectionState::SCANNING) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      WiFi.scanDelete();
      onComplete(false);
      return;
    }
    if (autoConnecting && mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      autoConnecting = false;
      manualNetworkListRequested = true;
      requestUpdate();
    }
    processWifiScanResults();
    return;
  }

  // Check connection progress
  if (state == WifiSelectionState::CONNECTING || state == WifiSelectionState::AUTO_CONNECTING) {
    if (state == WifiSelectionState::AUTO_CONNECTING) {
      if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        WiFi.disconnect();
        onComplete(false);
        return;
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        showNetworkListFromAutoConnect();
        return;
      }
    }
    checkConnectionStatus();
    return;
  }

  // Reached once the hidden-network SSID has been entered (and was non-empty).
  if (state == WifiSelectionState::HIDDEN_SSID_ENTRY) {
    const auto savedCred = WIFI_STORE.findCredential(selectedSSID);
    if (savedCred && !savedCred->password.empty()) {
      // We already know this hidden network - connect with the saved password
      enteredPassword = savedCred->password;
      usedSavedPassword = true;
      LOG_DBG("WiFi", "Using saved password for hidden network %s", selectedSSID.c_str());
      attemptConnection();
    } else {
      // Prompt for the password (empty password connects to open hidden APs)
      promptPasswordEntry();
    }
    return;
  }

  if (state == WifiSelectionState::PASSWORD_ENTRY) {
    // Reach here once password entry finished in subactivity
    attemptConnection();
    return;
  }

  // Handle save prompt state
  if (state == WifiSelectionState::SAVE_PROMPT) {
    // Touch goes through the FreeInkApp: render() registered the dialog
    // button hit rects; route the snapshot and let onPromptEvent dispatch.
    const auto route = routeTouch(mappedInput);
    if (route.routed && app.invalidated()) requestUpdate();
    if (route) return;  // dispatched to onPromptEvent

    if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
        mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      if (savePromptSelection > 0) {
        savePromptSelection--;
        requestUpdate();
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
               mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      if (savePromptSelection < 1) {
        savePromptSelection++;
        requestUpdate();
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (savePromptSelection == 0) {
        // User chose "Yes" - save the password
        RenderLock lock(*this);
        WIFI_STORE.addCredential(selectedSSID, enteredPassword);
      }
      // Complete - parent will start web server
      onComplete(true);
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      // Skip saving, complete anyway
      onComplete(true);
    }
    return;
  }

  // Handle forget prompt state (connection failed with saved credentials)
  if (state == WifiSelectionState::FORGET_PROMPT) {
    // Touch goes through the FreeInkApp: render() registered the dialog
    // button hit rects; route the snapshot and let onPromptEvent dispatch.
    const auto route = routeTouch(mappedInput);
    if (route.routed && app.invalidated()) requestUpdate();
    if (route) return;  // dispatched to onPromptEvent

    if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
        mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      if (forgetPromptSelection > 0) {
        forgetPromptSelection--;
        requestUpdate();
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
               mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      if (forgetPromptSelection < 1) {
        forgetPromptSelection++;
        requestUpdate();
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (forgetPromptSelection == 1) {
        RenderLock lock(*this);
        // User chose "Forget network" - forget the network
        WIFI_STORE.removeCredential(selectedSSID);
        // Update the network list to reflect the change
        const auto network = find_if(networks.begin(), networks.end(),
                                     [this](const WifiNetworkInfo& net) { return net.ssid == selectedSSID; });
        if (network != networks.end()) {
          network->hasSavedPassword = false;
        }
      }
      // Go back to network list (whether Cancel or Forget network was selected)
      startWifiScan();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      // Skip forgetting, go back to network list
      startWifiScan();
    }
    return;
  }

  // Handle connected state (should not normally be reached - connection
  // completes immediately)
  if (state == WifiSelectionState::CONNECTED) {
    // Safety fallback - immediately complete
    onComplete(true);
    return;
  }

  // Handle connection failed state
  if (state == WifiSelectionState::CONNECTION_FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      // A low-heap abort is not the network's fault: leave rather than offer to
      // forget a credential that works, and hand the failure to the caller so
      // the sync/browse screen behind us can report it.
      if (lowMemoryAbort) {
        onComplete(false);
        return;
      }
      // If we were auto-connecting or using a saved credential, offer to forget
      // the network
      if (autoConnecting || usedSavedPassword) {
        autoConnecting = false;
        state = WifiSelectionState::FORGET_PROMPT;
        forgetPromptSelection = 0;  // Default to "Cancel"
      } else {
        // Go back to network list on failure for non-saved credentials
        state = WifiSelectionState::NETWORK_LIST;
      }
      requestUpdate();
      return;
    }
  }

  // Handle network list state
  if (state == WifiSelectionState::NETWORK_LIST) {
    // Check for Back button to exit (cancel)
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      onComplete(false);
      return;
    }

    // Check for Confirm button to select network or rescan
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (!networks.empty()) {
        selectNetwork(selectedNetworkIndex);
      } else {
        startWifiScan();
      }
      return;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      startWifiScan();
      return;
    }

    const bool leftPressed = mappedInput.wasPressed(MappedInputManager::Button::Left);
    if (leftPressed) {
      const bool hasSavedPassword = !networks.empty() && networks[selectedNetworkIndex].hasSavedPassword;
      if (hasSavedPassword) {
        selectedSSID = networks[selectedNetworkIndex].ssid;
        state = WifiSelectionState::FORGET_PROMPT;
        forgetPromptSelection = 0;  // Default to "Cancel"
        requestUpdate();
        return;
      }
    }

    // Touch goes through the FreeInkApp: render() registered the row hit
    // rects; route the snapshot and let onRowEvent dispatch. Long-press on a
    // network row fires "forget" while the finger is down.
    const auto route = routeTouch(mappedInput, /*withLongPress=*/true);
    if (route.routed && app.invalidated()) requestUpdate();
    if (route) return;  // dispatched to onRowEvent

    if (!networks.empty()) {
      // Swipes scroll the viewport; the selection stays put and button
      // navigation pulls the view back to it.
      const auto swipe = mappedInput.wasSwipe();
      if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
        const int delta = swipe == MappedInputManager::SwipeDir::Up ? listNav.visibleRows : -listNav.visibleRows;
        if (listNav.scrollBy(delta, static_cast<int>(networks.size()))) requestUpdate();
        return;
      }
    }

    const auto moveSelection = [this](const int index) {
      selectedNetworkIndex = static_cast<size_t>(index);
      listNav.selected = index;
      listNav.follow(static_cast<int>(networks.size()));
      requestUpdate();
    };
    buttonNavigator.onNext(
        [this, &moveSelection] { moveSelection(ButtonNavigator::nextIndex(selectedNetworkIndex, networks.size())); });
    buttonNavigator.onPrevious([this, &moveSelection] {
      moveSelection(ButtonNavigator::previousIndex(selectedNetworkIndex, networks.size()));
    });
  }
}

std::string WifiSelectionActivity::getSignalStrengthIndicator(const int32_t rssi) const {
  // Convert RSSI to signal bars representation
  if (rssi >= -50) {
    return "||||";  // Excellent
  }
  if (rssi >= -60) {
    return " |||";  // Good
  }
  if (rssi >= -70) {
    return "  ||";  // Fair
  }
  return "   |";  // Very weak
}

void WifiSelectionActivity::render(RenderLock&&) {
  // Don't render if we're in a keyboard-entry state - we're just transitioning
  // from the keyboard subactivity back to the main activity
  if (state == WifiSelectionState::PASSWORD_ENTRY || state == WifiSelectionState::HIDDEN_SSID_ENTRY) {
    return;
  }

  renderer.clearScreen();

  auto& theme = UITheme::getInstance();
  auto metrics = theme.getMetrics();
  Rect screen = theme.getScreenSafeArea(renderer, true, false);

  // Draw header
  // STR_NETWORKS_FOUND is ~37 bytes once the Arabic translation is substituted,
  // so 32 truncated it. See ClockSyncActivity for the same class of bug.
  char countStr[64];
  snprintf(countStr, sizeof(countStr), tr(STR_NETWORKS_FOUND), realNetworkCount);
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_WIFI_NETWORKS), countStr);
  GUI.drawSubHeader(
      renderer,
      Rect{screen.x, screen.y + metrics.topPadding + metrics.headerHeight, screen.width, metrics.tabBarHeight},
      cachedMacAddress.c_str());

  switch (state) {
    case WifiSelectionState::AUTO_CONNECTING:
      renderConnecting(&screen, &metrics);
      break;
    case WifiSelectionState::SCANNING:
      renderConnecting(&screen, &metrics);  // Reuse connecting screen with different message
      break;
    case WifiSelectionState::NETWORK_LIST:
      renderNetworkList(&screen, &metrics);
      break;
    case WifiSelectionState::HIDDEN_SSID_ENTRY:
      // Transitioning to/from the SSID keyboard subactivity - nothing to draw
      break;
    case WifiSelectionState::CONNECTING:
      renderConnecting(&screen, &metrics);
      break;
    case WifiSelectionState::CONNECTED:
      renderConnected(&screen, &metrics);
      break;
    case WifiSelectionState::SAVE_PROMPT:
    case WifiSelectionState::FORGET_PROMPT: {
      // The app's screen builder draws the option dialog panel itself.
      renderUi();
      const auto labels =
          mappedInput.mapLabels(state == WifiSelectionState::SAVE_PROMPT ? tr(STR_CANCEL) : tr(STR_BACK),
                                tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
    case WifiSelectionState::CONNECTION_FAILED:
      renderConnectionFailed(&screen, &metrics);
      break;
  }

  renderer.displayBuffer();
}

void WifiSelectionActivity::listScreen(UiScreen& screen, void* user) {
  static_cast<WifiSelectionActivity*>(user)->buildListScreen(screen);
}

void WifiSelectionActivity::buildListScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  // Content below the header + MAC sub-band, above the legend line.
  screen.setContentMargin(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight +
                           metrics.verticalSpacing),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height) + metrics.verticalSpacing * 2),
      static_cast<int16_t>(safe.x)});

  if (state == WifiSelectionState::SAVE_PROMPT || state == WifiSelectionState::FORGET_PROMPT) {
    buildPromptDialog(screen);
    return;
  }

  if (networks.empty()) {
    screen.centeredText(tr(STR_NO_NETWORKS), screen.theme().bodyText);
    if (mappedInput.hasTouch()) {
      // Touch has no OK button to rescan with; offer the retry on screen instead
      // of the "Press OK" hint renderNetworkList draws for button boards.
      const auto& theme = screen.theme();
      const fui::Rect body = screen.body();
      const int16_t buttonWidth = static_cast<int16_t>(body.width / 2);
      const fui::Rect buttonRect{static_cast<int16_t>(body.x + (body.width - buttonWidth) / 2),
                                 static_cast<int16_t>(body.y + body.height * 2 / 3), buttonWidth, theme.rowHeight};
      fui::ButtonProps scan;
      scan.label = tr(STR_RETRY);
      scan.action = ACTION_SCAN;
      scan.inputMask = fui::InputTouch;
      scan.text = theme.bodyText;
      fui::button(screen.frame(), buttonRect, scan);
    }
    return;
  }

  // networkStatuses/networkRowItems are built once per processWifiScanResults()
  // call (see rebuildNetworkRowItems()) and reused here on every repaint.
  fui::ListProps props;
  props.items = networkRowItems.data();
  props.count = static_cast<uint16_t>(networkRowItems.size());
  props.action = ACTION_ROW;
  // Tap opens; long-press a saved network forgets it (physical buttons stay in loop()).
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  props.valueInset = 8;  // air between the signal bars and the row edge
  // Long SSIDs wrap onto a second line inside the row (two body lines always
  // fit the theme row height) instead of truncating; the trailing value is
  // just the short status glyphs, so skip the balanced 60%-band wrap cap.
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 2;
  props.balanceWrappedLabelWithValue = false;
  listNav.selected = static_cast<int>(selectedNetworkIndex);
  int16_t rowHeight = screen.theme().rowHeight;
  if (!mappedInput.hasTouch()) {
    // Non-touch hardware (X3/X4) keeps the original, denser row height
    // instead of FreeInkUI's touch-target-sized default (see
    // UiListActivity::syncListViewport; this screen predates that base and
    // syncs its own viewport directly).
    rowHeight = static_cast<int16_t>(metrics.listRowHeight);
    props.rowHeight = rowHeight;
  }
  listNav.syncToProps(screen.body(), rowHeight, screen.theme().listRowGap, static_cast<int>(networks.size()), props);
  screen.list(props);
}

void WifiSelectionActivity::buildPromptDialog(UiScreen& screen) {
  const bool isForget = state == WifiSelectionState::FORGET_PROMPT;

  // Owned for the duration of the draw; the dialog wraps long SSIDs itself.
  const std::string ssidInfo = std::string(tr(STR_NETWORK_PREFIX)) + selectedSSID;

  const int selection = isForget ? forgetPromptSelection : savePromptSelection;
  fui::DialogOption options[2];
  options[0].label = isForget ? tr(STR_CANCEL) : tr(STR_YES);
  options[1].label = isForget ? tr(STR_FORGET_NETWORK) : tr(STR_NO);
  for (int i = 0; i < 2; i++) {
    options[i].action = ACTION_PROMPT;
    options[i].value = static_cast<int16_t>(i);
    options[i].state = selection == i ? fui::StateFocused : fui::StateNormal;
  }

  fui::OptionDialogProps props;
  props.title = isForget ? tr(STR_FORGET_NETWORK) : tr(STR_CONNECTED);
  props.headline = ssidInfo.c_str();
  props.message = isForget ? tr(STR_FORGET_AND_REMOVE) : tr(STR_SAVE_PASSWORD);
  props.options = options;
  props.optionCount = 2;
  // Stacked full-width options (matching OptionPopup): side-by-side halves
  // truncate the long "Forget network" label.
  props.verticalOptions = true;
  props.titleText = screen.theme().smallText;
  props.titleText.bold = true;
  // TextStyle defaults to maxLines=1 (ellipsis truncation); let the SSID
  // headline and the question wrap. optionDialogHeight measures with the same
  // styles, so the dialog grows to fit the wrapped lines.
  props.headlineText = screen.theme().bodyText;
  props.headlineText.maxLines = 2;
  props.messageText = screen.theme().smallText;
  props.messageText.maxLines = 3;
  props.buttonText = screen.theme().smallText;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  // defaultPopupStyles() (fui::optionDialog's fallback when styles is left
  // unset) has no border; opt one in explicitly using the theme's popup frame
  // metrics, matching OptionPopup::render().
  const auto& metrics = UITheme::getInstance().getMetrics();
  props.styles = fui::defaultPopupStyles();
  props.styles.normal.border = fui::Paint::solid(fui::Color::Black);
  props.styles.normal.borderWidth = static_cast<uint8_t>(metrics.popupFrameThickness);
  props.styles.normal.radius = static_cast<uint8_t>(metrics.popupCornerRadius);
  props.styles.selected = props.styles.normal;
  props.styles.focused = props.styles.normal;
  props.styles.active = props.styles.normal;
  props.styles.disabled = props.styles.normal;

  const fui::Rect body = screen.body();
  int16_t width = static_cast<int16_t>(renderer.getScreenWidth() * 3 / 4);
  if (width > body.width) width = body.width;
  const int16_t height = fui::optionDialogHeight(screen.target(), props, width);
  fui::optionDialog(screen.frame(), fui::centeredRect(body, fui::Size{width, height}), props);
}

void WifiSelectionActivity::renderNetworkList(const Rect* screen, const ThemeMetrics* metrics) {
  renderUi();
  if (networks.empty() && !mappedInput.hasTouch()) {
    // Below the centered "no networks" line the app drew. Touch boards get an
    // on-screen Retry button from the screen builder instead of this hint.
    const auto height = renderer.getLineHeight(UI_10_FONT_ID);
    const auto top = screen->y + (screen->height - height) / 2;
    UITheme::drawCenteredText(renderer, *screen, SMALL_FONT_ID, top + height + 10, tr(STR_PRESS_OK_SCAN));
  }

  GUI.drawHelpText(renderer,
                   Rect{screen->x, screen->y + screen->height - metrics->contentSidePadding - 15, screen->width, 20},
                   tr(STR_NETWORK_LEGEND));

  const bool hasSavedPassword = !networks.empty() && networks[selectedNetworkIndex].hasSavedPassword;
  const char* forgetLabel = hasSavedPassword ? tr(STR_FORGET_BUTTON) : "";

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONNECT), forgetLabel, tr(STR_RETRY));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void WifiSelectionActivity::renderConnecting(const Rect* screen, const ThemeMetrics* metrics) const {
  constexpr int MAX_STATUS_LINES = 2;
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = screen->y + (screen->height - height) / 2;
  const int statusX = screen->x + metrics->contentSidePadding;
  const int statusWidth = screen->width - metrics->contentSidePadding * 2;

  if (state == WifiSelectionState::SCANNING) {
    const char* statusText = autoConnecting ? tr(STR_FINDING_SAVED_WIFI) : tr(STR_SCANNING);
    const Rect statusBounds{statusX, screen->y, statusWidth, screen->height};
    UITheme::drawCenteredWrappedText(renderer, statusBounds, UI_10_FONT_ID, statusText, MAX_STATUS_LINES);
    if (autoConnecting) {
      const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_SHOW_NETWORKS), "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
  } else {
    const char* statusText = autoConnecting ? tr(STR_CONNECTING_SAVED_WIFI) : tr(STR_CONNECTING);
    const Rect statusBounds{statusX, screen->y, statusWidth, top - metrics->verticalSpacing - screen->y};
    UITheme::drawCenteredWrappedText(renderer, statusBounds, UI_12_FONT_ID, statusText, MAX_STATUS_LINES, true,
                                     EpdFontFamily::BOLD, UITheme::TextVerticalAlignment::BOTTOM);

    std::string ssidInfo = std::string(tr(STR_TO_PREFIX)) + selectedSSID;
    if (ssidInfo.length() > 25) {
      ssidInfo.replace(22, ssidInfo.length() - 22, "...");
    }
    UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top, ssidInfo.c_str());
    if (autoConnecting) {
      const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_SHOW_NETWORKS), "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
  }
}

void WifiSelectionActivity::renderConnected(const Rect* screen, const ThemeMetrics* metrics) const {
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = screen->y + (screen->height - height * 4) / 2;

  UITheme::drawCenteredText(renderer, *screen, UI_12_FONT_ID, top - 30, tr(STR_CONNECTED), true, EpdFontFamily::BOLD);

  std::string ssidInfo = std::string(tr(STR_NETWORK_PREFIX)) + selectedSSID;
  if (ssidInfo.length() > 28) {
    ssidInfo.replace(25, ssidInfo.length() - 25, "...");
  }
  UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top + 10, ssidInfo.c_str());

  const std::string ipInfo = std::string(tr(STR_IP_ADDRESS_PREFIX)) + connectedIP;
  UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top + 40, ipInfo.c_str());

  // Use centralized button hints
  const auto labels = mappedInput.mapLabels("", tr(STR_DONE), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void WifiSelectionActivity::renderConnectionFailed(const Rect* screen, const ThemeMetrics* metrics) const {
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = screen->y + (screen->height - height * 2) / 2;

  UITheme::drawCenteredText(renderer, *screen, UI_12_FONT_ID, top - 20, tr(STR_CONNECTION_FAILED), true,
                            EpdFontFamily::BOLD);
  UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top + 20, connectionError.c_str());

  // Use centralized button hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DONE), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void WifiSelectionActivity::onComplete(const bool connected) {
  ActivityResult result;
  result.isCancelled = !connected;
  if (connected) {
    result.data = WifiResult{true, selectedSSID, connectedIP};
  }
  setResult(std::move(result));
  finish();
}
