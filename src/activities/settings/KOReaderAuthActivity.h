#pragma once

#include <functional>

#include "activities/Activity.h"

/**
 * Activity for testing KOReader credentials, or — in sign-up mode — creating a
 * new account on the sync server with the entered username/password.
 * Connects to WiFi, then authenticates or registers.
 */
class KOReaderAuthActivity final : public Activity {
 public:
  enum class Mode { AUTHENTICATE, SIGN_UP };

  /**
   * @param targetServerIndex Index into KOReaderCredentialStore to make active before
   *        authenticating, or -1 (default) to authenticate without changing the active server.
   *        On success the target server stays active; on failure the previous active is restored.
   * @param mode AUTHENTICATE to validate credentials, SIGN_UP to register a new account.
   */
  explicit KOReaderAuthActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int targetServerIndex = -1,
                                Mode mode = Mode::AUTHENTICATE)
      : Activity("KOReaderAuth", renderer, mappedInput), targetServerIndex(targetServerIndex), mode(mode) {}

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
  Mode mode = Mode::AUTHENTICATE;

  void onWifiSelectionComplete(bool success);
  void performAuthentication();
};
