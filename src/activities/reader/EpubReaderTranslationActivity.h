#pragma once

#include <I18n.h>

#include <string>

#include "activities/Activity.h"
#include "components/PanelTextPages.h"
#include "util/ButtonNavigator.h"

// crosspoint: the page's translation on a full screen of its own; the connection, progress and any
// error are shown there too. A saved network is joined without leaving it; only picking a network
// or typing a password opens the Wi-Fi list.
class EpubReaderTranslationActivity final : public Activity {
 public:
  // preTranslatedText: if non-empty, the activity shows it directly without
  // any network call (used when a translation was already extracted offline
  // during manga conversion and stored alongside the page data).
  // resumedAfterRestart: true only when setup() re-created this activity from the
  // TRANSLATE_STASH_PATH stash after a silent restart. Gates the stash-and-restart
  // fallback to one attempt -- on a pristine post-boot heap a second gate failure
  // is a real error, not fragmentation, so it must show the message instead of
  // restart-looping.
  explicit EpubReaderTranslationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string sourceText,
                                         std::string preTranslatedText = "", bool resumedAfterRestart = false,
                                         StrId title = StrId::STR_TRANSLATE_PAGE);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == WIFI_SELECTION || state == TRANSLATING; }

 private:
  enum State {
    CONNECTING,  // joining the last-used saved network
    WIFI_SELECTION,
    TRANSLATING,
    SHOWING_RESULT,
    ERROR,
  };

  State state = WIFI_SELECTION;
  std::string sourceText;
  std::string translatedText;
  std::string errorMessage;
  bool hasPreTranslation = false;
  bool resumedAfterRestart = false;
  StrId title;  // panel heading: the page, or a selection

  // Write sourceText to TRANSLATE_STASH_PATH and silent-restart into a fresh-heap
  // translation (see SilentRestart.h). Returns false if the stash could not be
  // written -- caller then falls back to the low-memory error message.
  bool stashAndRestart();

  PanelTextPages textPages;  // laid out once the translation arrives
  int currentPage = 0;
  unsigned long connectStartMs = 0;
  // crosspoint: what the reply reports it used, shown in the footer. Zero until a reply.
  uint32_t promptTokens = 0;
  uint32_t outputTokens = 0;  // the translation plus the model's thinking, both billed as output
  char modelVersion[32] = "";
  // Joins the last-used saved network without the Wi-Fi list; false when there is none.
  bool startQuickConnect();
  void pollQuickConnect();
  void startWifiSelection();
  void stepPage(int direction);
  void cancel();

  ButtonNavigator buttonNavigator;

  bool readApiKey(std::string& keyOut);
  // crosspoint: `model` null means the first GEMINI_MODELS entry; lastHttpCode is the reply's status (0 if none).
  bool callGeminiApi(const std::string& apiKey, const char* model = nullptr);
  int lastHttpCode = 0;
  uint8_t modelIndex = 0;  // the GEMINI_MODELS entry last asked
  // crosspoint: Confirm on an error screen asks again. Only once the network was brought up: a
  // low-memory or missing-key error would fail the same way.
  bool canRetry() const;
  void retry();

  void onWifiComplete(bool success);
};
