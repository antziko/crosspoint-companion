#pragma once

#include <I18n.h>

#include <string>

#include "activities/Activity.h"
#include "components/PanelTextPages.h"
#include "util/ButtonNavigator.h"

// The page's translation in the floating dictionary panel, drawn over the page: the connection,
// progress and any error are shown inside the panel too. A saved network is joined from the panel
// itself; only picking a network or typing a password opens the full-screen Wi-Fi list, after
// which the page behind the panel is restored from the copy saved on entry.
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
    CONNECTING,  // joining the last-used saved network, shown in the panel
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
  // crosspoint: what the reply reports it used, shown in the panel footer. Zero until a reply.
  uint32_t promptTokens = 0;
  uint32_t outputTokens = 0;  // the translation plus the model's thinking, both billed as output
  char modelVersion[32] = "";
  // The page behind the panel must be copied back from SD before the next render: set after the
  // full-screen Wi-Fi list, and on a restarted translation, whose framebuffer starts blank.
  bool restoreBackgroundPending = false;

  // The framebuffer (the reader's page) saved to SD on entry and read back after anything that
  // drew over it. Written straight from and into the framebuffer, so no heap is borrowed.
  static bool saveBackground(const GfxRenderer& renderer);
  static bool restoreBackground(const GfxRenderer& renderer);
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
  uint8_t modelIndex = 0;       // the GEMINI_MODELS entry last asked
  bool lastRecitation = false;  // the last reply was withheld as matching published text
  bool freeWording = false;     // ask for a translation in the model's own plain wording
  // crosspoint: the panel is on the glass, so a repaint drives only the panel (and the hint band
  // when its labels change); the X3 FAST bank pumps every unchanged page pixel one way.
  bool panelOnGlass = false;
  uint8_t lastHintKey = 0xFF;
  // crosspoint: Confirm on an error screen asks again. Only once the network was brought up: a
  // low-memory or missing-key error would fail the same way.
  bool canRetry() const;
  void retry();

  void onWifiComplete(bool success);
};
