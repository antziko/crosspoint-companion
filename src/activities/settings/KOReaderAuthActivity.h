#pragma once

#include <functional>

#include "activities/Activity.h"

/**
 * Activity for testing KOReader credentials.
 * Connects to WiFi and authenticates with the KOReader sync server.
 */
class KOReaderAuthActivity final : public Activity {
 public:
  /**
   * @param targetServerIndex Index into KOReaderCredentialStore to make active before
   *        authenticating, or -1 (default) to authenticate without changing the active server.
   *        On success the target server stays active; on failure the previous active is restored.
   */
  explicit KOReaderAuthActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                int targetServerIndex = -1)
      : Activity("KOReaderAuth", renderer, mappedInput), targetServerIndex(targetServerIndex) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == AUTHENTICATING; }

 private:
  enum State { WIFI_SELECTION, CONNECTING, AUTHENTICATING, SUCCESS, FAILED };

  State state = WIFI_SELECTION;
  std::string statusMessage;
  std::string errorMessage;
  int targetServerIndex;
  int previousActiveIndex = -1;

  void onWifiSelectionComplete(bool success);
  void performAuthentication();
};
