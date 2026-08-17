#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/UiAppHost.h"
#include "util/ButtonNavigator.h"

struct Rect;
struct ThemeMetrics;
struct WifiCredential;

// Structure to hold WiFi network information
struct WifiNetworkInfo {
  std::string ssid;
  int32_t rssi;
  bool isEncrypted;
  bool hasSavedPassword;             // Whether we have saved credentials for this network
  bool isHiddenPlaceholder = false;  // Synthetic "Add hidden network..." list entry
};

// WiFi selection states
enum class WifiSelectionState {
  AUTO_CONNECTING,    // Trying to connect to the last known network
  SCANNING,           // Scanning for networks
  NETWORK_LIST,       // Displaying available networks
  HIDDEN_SSID_ENTRY,  // Entering SSID for a hidden network
  PASSWORD_ENTRY,     // Entering password for selected network
  CONNECTING,         // Attempting to connect
  CONNECTED,          // Successfully connected
  SAVE_PROMPT,        // Asking user if they want to save the password
  CONNECTION_FAILED,  // Connection failed
  FORGET_PROMPT       // Asking user if they want to forget the network
};

/**
 * WifiSelectionActivity is responsible for scanning WiFi APs and connecting to them.
 * It will:
 * - Enter scanning mode on entry
 * - List available WiFi networks
 * - Allow selection and launch KeyboardEntryActivity for password if needed
 * - Save the password if requested
 * - Call onComplete callback when connected or cancelled
 *
 * The onComplete callback receives true if connected successfully, false if cancelled.
 */
class WifiSelectionActivity final : public Activity, private UiAppHost {
  ButtonNavigator buttonNavigator;

  WifiSelectionState state = WifiSelectionState::SCANNING;
  size_t selectedNetworkIndex = 0;
  std::vector<WifiNetworkInfo> networks;
  // Number of real (scanned) networks, excluding the synthetic hidden-network entry
  size_t realNetworkCount = 0;

  // Row buffers derived from `networks`, rebuilt only when it changes
  // (processWifiScanResults()) instead of on every repaint — buildListScreen()
  // used to re-derive a "+ * ||||" status string per network on every render
  // (cursor move, tap flash, ...).
  std::vector<std::string> networkStatuses;
  std::vector<freeink::ui::ListItem> networkRowItems;
  void rebuildNetworkRowItems();

  // Selected network for connection
  std::string selectedSSID;
  bool selectedRequiresPassword = false;

  // Connection result
  std::string connectedIP;
  std::string connectionError;

  // Password to potentially save (from keyboard or saved credentials)
  std::string enteredPassword;

  // Cached MAC address string for display
  std::string cachedMacAddress;

  // Whether network was connected using a saved password (skip save prompt)
  bool usedSavedPassword = false;

  // Whether to attempt auto-connect on entry
  const bool allowAutoConnect;

  // Whether we are attempting to auto-connect or auto-scan saved networks.
  bool autoConnecting = false;

  // True when the user stopped auto-connect and asked to see the scan result.
  bool manualNetworkListRequested = false;

  // Saved SSIDs already attempted during the current auto-connect session.
  std::vector<std::string> autoAttemptedSsids;

  // Save/forget prompt selection (0 = Yes, 1 = No)
  int savePromptSelection = 0;
  int forgetPromptSelection = 0;

  // Connection timeout
  static constexpr unsigned long CONNECTION_TIMEOUT_MS = 15000;
  static constexpr unsigned long AUTO_CONNECTION_TIMEOUT_MS = 7000;
  unsigned long connectionStartTime = 0;

  // Scan timeout. Every other wait on this screen is bounded, but the scan was
  // not: arduino-esp32's own guard is _scanTimeout = 60 s (WiFiScan.cpp), long
  // enough that a stalled scan reads as a hang on "Finding saved Wi-Fi...".
  static constexpr unsigned long SCAN_TIMEOUT_MS = 12000;
  unsigned long scanStartTime = 0;

  // Heap floors for a *scan* (not for the radio), in two tiers, because what a scan
  // still has to pay for depends on whether the driver is already up. esp_wifi + lwip
  // init costs ~53 KB measured (three runs: entered this screen at 60492/60524/60164,
  // reached startWifiScan() at 6880/7124/6368), and the scan peaks ~14 KB on top of
  // that — the one run that entered at 74112 reached its scan with ~20.7 KB and
  // bottomed at 7008.
  //
  // A single floor could not express this. onEnter() tries the last-known SSID first,
  // so by the time anything reaches a scan the radio is usually already up and only
  // that ~14 KB is outstanding; 68 KB is the right cold number and an unreachable one
  // warm, which is why every "Show networks" on the sync path answered "Not enough
  // memory" (see WIFI "scan skipped ... auto=0" in opds_debug.txt).
  //
  // Largest-block is a sanity check, not the deciding term: the scan's own allocations
  // are many small ones plus a single count*sizeof(wifi_ap_record_t) array (~2-3 KB for
  // 30 APs), so the warm figure only has to sit above every refused sample (2.0-5.4 KB)
  // without gating on a contiguous block the scan never asks for.
  //
  // First estimates, same standing as kMinFreeForDict in EpubReaderActivity — tune from
  // the logged "scan skipped" values.
  static constexpr size_t SCAN_MIN_FREE_HEAP_COLD = 68 * 1024;
  static constexpr size_t SCAN_MIN_LARGEST_BLOCK_COLD = 20 * 1024;
  static constexpr size_t SCAN_MIN_FREE_HEAP_WARM = 20 * 1024;
  static constexpr size_t SCAN_MIN_LARGEST_BLOCK_WARM = 6 * 1024;
  static bool hasHeapForScan();

  // Set when the screen gave up for lack of heap rather than because a network
  // misbehaved; dismissing that error leaves instead of offering to forget a
  // credential that is perfectly good.
  bool lowMemoryAbort = false;
  void failWithLowMemory();

  // The UiAppHost app hosts the network list and the save/forget prompts
  // (themed rows and dialogs, touch routing); every other state keeps its
  // legacy centered-text rendering.
  // Viewport memory (top/visibleRows) for the network list; `selected` is
  // mirrored from selectedNetworkIndex at build/move time.
  freeink::ui::ListNav listNav;

  static void listScreen(UiScreen& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onScanEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onPromptEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildListScreen(UiScreen& screen);
  void buildPromptDialog(UiScreen& screen);

  // Title + AP-count + MAC band, common to every state. Extracted so the
  // network-list rebuild pass can repaint the whole frame (see render()).
  void drawHeaderBand(const Rect& screen, const ThemeMetrics& metrics) const;
  void renderNetworkList(const Rect* screen, const ThemeMetrics* metrics);
  void renderPasswordEntry(const Rect* screen, const ThemeMetrics* metrics) const;
  void renderConnecting(const Rect* screen, const ThemeMetrics* metrics) const;
  void renderConnected(const Rect* screen, const ThemeMetrics* metrics) const;
  void renderConnectionFailed(const Rect* screen, const ThemeMetrics* metrics) const;

  void startWifiScan(bool autoScan = false);
  void processWifiScanResults();
  void appendHiddenNetworkEntry();
  void selectNetwork(int index);
  void promptHiddenSsid();
  void promptPasswordEntry();
  void attemptConnection();
  void checkConnectionStatus();
  bool tryAutoConnectCredential(const WifiCredential& cred);
  bool tryNextSavedNetworkFromScan();
  // Same idea as tryNextSavedNetworkFromScan(), but sourced from the credential
  // store instead of the scan list, for when there is no scan to draw on.
  bool tryNextSavedCredentialBlind();
  void handleAutoConnectFailure();
  void showNetworkListFromAutoConnect();
  bool hasAttemptedAutoSsid(const std::string& ssid) const;
  std::string getSignalStrengthIndicator(int32_t rssi) const;

  void onComplete(bool connected);

 public:
  explicit WifiSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool autoConnect = true);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
