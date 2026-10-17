#include "EpubReaderTranslationActivity.h"

#include <ArduinoJson.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <I18n.h>
#include <InflateReader.h>
#include <Logging.h>
#include <SdSystemDir.h>
#include <SecureHttpClient.h>
#include <WiFi.h>
#include <esp_crt_bundle.h>

#include <algorithm>
#include <cstring>

#include "CrossPointSettings.h"
#include "HapticFeedback.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "SilentRestart.h"
#include "WifiCredentialStore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ClockSyncPolicy.h"

// crosspoint: every log line of this screen also goes to the SD debug log; a device in the field
// has no serial, and a failed translation is otherwise invisible.
#include <SdDebugLog.h>
#undef LOG_DBG
#undef LOG_INF
#undef LOG_ERR
#define LOG_DBG(origin, ...) SdDebugLog::log(origin, __VA_ARGS__)
#define LOG_INF(origin, ...) SdDebugLog::log(origin, __VA_ARGS__)
#define LOG_ERR(origin, ...) SdDebugLog::log(origin, __VA_ARGS__)

namespace {

constexpr int HTTP_BUF_SIZE = 2048;
// wolfSSL, not mbedTLS: this request used to go through esp_http_client (the project's only
// mbedTLS user), whose 16KB-per-direction record buffers are baked into the prebuilt framework
// and needed ~55KB contiguous -- measured on an X4, a handshake at 53236 bytes failed with
// ESP_ERR_HTTP_CONNECT after driving free heap to 356 bytes, so every translation from a
// reading session paid a silent restart. SecureHttpClient runs wolfSSL, which is built from
// source with our own user_settings.h (scripts/patch_wolfssl.py) and whose largest single
// allocation is the ~17KB record buffer. Floors adopted from KOReaderSyncClient, which ported
// the same way and measured handshakes succeeding inside a 43KB largest block.
constexpr uint32_t MIN_FREE_FOR_TLS = 35000;
constexpr uint32_t MIN_HEAP_FOR_TLS = 20000;
// What bringing up the WiFi/lwIP stack takes out of the LARGEST CONTIGUOUS block, not out of
// total free. Measured on device: 86004 at translation entry, 53236 left at the TLS gate --
// 32768 exactly. The margin on top covers association-time variation; too generous a value
// here costs a restart that wasn't needed, too tight costs the doubled WiFi+NTP this check
// exists to avoid. Used to decide BEFORE the connect whether one pass can work.
constexpr uint32_t WIFI_STACK_RESERVE = 36000;
// esp_wifi_init() (triggered by the first WiFi.mode(WIFI_STA) call) allocates its own TX/RX
// buffer pools, NVS state, and wpa_supplicant/RRM tables -- many small-to-medium allocations, not
// one big contiguous block, so this checks total free heap rather than getMaxAllocHeap(). Confirmed
// on a real device: entering Translation right after reading a memory-heavy CJK chapter (free heap
// down to ~37KB) crashed with a null-pointer fault inside wpa_supplicant's eloop_cancel_timeout --
// some internal allocation failed and the driver didn't null-check it before dereferencing. There's
// no public API to ask ESP-IDF's WiFi driver "do you have enough heap", so this margin is a
// conservative empirical floor above the crash point, not a documented ESP-IDF constant.
constexpr uint32_t MIN_HEAP_FOR_WIFI_INIT = 70000;
static std::string apiKeyPath() { return sdsystem::findUserFile("gemini.key"); }
// crosspoint: asked in order; the next one only when the last was out of quota (429) or busy (5xx).
// Free-tier quotas and capacity are per model. Prices are USD millicents per million tokens for the
// footer's estimate (thinking bills as output); 0 = not known. Kept to models that answer in ~3 s.
// `thinking` is the lowest level each model is known to accept: "minimal" skips thinking, whose
// tokens otherwise eat the output budget and can leave a reply with no text.
struct GeminiModel {
  const char* name;
  uint32_t priceIn;
  uint32_t priceOut;
  const char* thinking;
};
constexpr GeminiModel GEMINI_MODELS[] = {
    {"gemini-3.1-flash-lite", 25000, 150000, "minimal"},   // $0.25 / $1.50
    {"gemini-3.6-flash", 75000, 375000, "minimal"},        // $0.75 / $3.75 (through 2026)
    {"gemini-3-flash-preview", 50000, 300000, "minimal"},  // $0.50 / $3.00
    {"gemini-3.5-flash", 150000, 900000, "low"},           // $1.50 / $9.00; "minimal" untested
    {"gemini-3.8-flash", 75000, 375000, "low"},            // $0.75 / $3.75; "low" is its floor
};
constexpr uint8_t GEMINI_MODEL_COUNT = sizeof(GEMINI_MODELS) / sizeof(GEMINI_MODELS[0]);
constexpr const char* GEMINI_MODEL = GEMINI_MODELS[0].name;
// A saved network either answers within this or is out of range; the Wi-Fi list takes over then.
constexpr unsigned long QUICK_CONNECT_TIMEOUT_MS = 12000;
// Translations are prose, and a Japanese page's is read beside Japanese: the UI face carries the
// CJK fallback.
constexpr int TRANSLATION_FONT_ID = UI_12_FONT_ID;
// crosspoint: air between lines of the translation; solid UI_12 reads cramped in the panel.
constexpr int TRANSLATION_LINE_SPACING_PERCENT = 135;

// crosspoint: the translation fills the screen: text above a divider, and the model, cost and
// page counter below it.
constexpr int PAGE_MARGIN = 16;
constexpr int FOOTER_GAP = 8;

struct PageLayout {
  Rect body;
  int footerY;
};

PageLayout pageLayout(const GfxRenderer& renderer) {
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  PageLayout layout;
  layout.footerY = safe.y + safe.height - FOOTER_GAP - renderer.getLineHeight(SMALL_FONT_ID);
  const int top = safe.y + PAGE_MARGIN;
  layout.body = Rect(safe.x + PAGE_MARGIN, top, std::max(0, safe.width - 2 * PAGE_MARGIN),
                     std::max(0, layout.footerY - 2 * FOOTER_GAP - top));
  return layout;
}

}  // namespace

EpubReaderTranslationActivity::EpubReaderTranslationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                             std::string sourceText, std::string preTranslatedText,
                                                             const bool resumedAfterRestart, const StrId title)
    : Activity("Translation", renderer, mappedInput),
      sourceText(std::move(sourceText)),
      resumedAfterRestart(resumedAfterRestart),
      title(title) {
  if (!preTranslatedText.empty()) {
    translatedText = std::move(preTranslatedText);
    hasPreTranslation = true;
    state = SHOWING_RESULT;
  }
}

bool EpubReaderTranslationActivity::stashAndRestart() {
  HalFile stash;
  if (!Storage.openFileForWrite("XLAT", translateStashPath().c_str(), stash)) {
    LOG_ERR("XLAT", "Could not write translation stash; showing low-memory error instead");
    return false;
  }
  const size_t written = stash.write(reinterpret_cast<const uint8_t*>(sourceText.data()), sourceText.size());
  stash.close();
  if (written != sourceText.size()) {
    LOG_ERR("XLAT", "Short write on translation stash (%u/%u); showing low-memory error instead",
            static_cast<unsigned>(written), static_cast<unsigned>(sourceText.size()));
    Storage.remove(translateStashPath().c_str());
    return false;
  }
  LOG_DBG("XLAT", "Stashed %u bytes; restarting for a fresh heap", static_cast<unsigned>(sourceText.size()));
  silentRestartToTranslation();  // does not return
  return true;
}

void EpubReaderTranslationActivity::onEnter() {
  Activity::onEnter();

  if (hasPreTranslation) {
    const auto body = pageLayout(renderer).body;  // crosspoint
    textPages.setLineSpacingPercent(TRANSLATION_LINE_SPACING_PERCENT);
    textPages.layout(renderer, TRANSLATION_FONT_ID, translatedText, body.width, body.height);
    requestUpdate();
    return;
  }

  // The reader activity underneath is only paused, not destroyed, so its font decompressor's
  // hot-group buffer (up to tens of KB, see FontDecompressor.cpp) is still resident and dead
  // weight here -- free it before WiFi init needs the headroom, same rationale as the identical
  // call before a chapter build in EpubReaderActivity.
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->releaseAllFontMemory();
  }
  // crosspoint: the 32KB inflate window goes before the radio, as in KOReaderSyncActivity.
  // The reader re-reserves it if this returns without the exit restart.
  InflateReader::releaseWindow();

  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxAllocAtEntry = ESP.getMaxAllocHeap();
  LOG_DBG("XLAT", "Entering translation (free heap: %u, max alloc: %u)", static_cast<unsigned>(freeHeap),
          static_cast<unsigned>(maxAllocAtEntry));
  // Decide about the restart HERE, before WiFi. The old gate sat after the connect, so a heap
  // that couldn't carry TLS still paid for association and the NTP sync first (device log: 3.7s
  // + 1.2s), then restarted and paid for both a second time. Bringing WiFi up costs a
  // predictable slice of the largest block -- measured 100KB+ at entry down to 53KB at the TLS
  // gate -- so if that slice would leave us short, restarting now makes it a single payment.
  // Deliberately NOT "try TLS anyway and see": on the X3 the framebuffer is larger and the
  // margin thinner, and a handshake that runs out mid-way is a crash, not a clean error.
  const bool wontFitAfterWifi = maxAllocAtEntry < MIN_HEAP_FOR_TLS + WIFI_STACK_RESERVE;
  // With wolfSSL's much lower requirement this should now be false for any normal reading
  // session (measured entry: 86004, WiFi takes 32768, leaving 53236 against a 20000 floor) --
  // the restart becomes the exception it was meant to be rather than the rule.
  if (freeHeap < MIN_HEAP_FOR_WIFI_INIT || wontFitAfterWifi) {
    LOG_ERR("XLAT", "Heap too tight for a one-pass translation (free %u, maxAlloc %u, need %u)",
            static_cast<unsigned>(freeHeap), static_cast<unsigned>(maxAllocAtEntry),
            static_cast<unsigned>(MIN_HEAP_FOR_TLS + WIFI_STACK_RESERVE));
    // A long reading session (Word Lookup, chapter builds) can leave the heap too fragmented for
    // the WiFi/TLS stack even after everything reclaimable was freed -- but a silent restart
    // clears it completely (~110KB contiguous right after boot). Stash the text and retry once
    // on a fresh heap; only a post-restart failure is a real error worth showing.
    if (!resumedAfterRestart && stashAndRestart()) return;
    errorMessage = tr(STR_TRANSLATION_LOW_MEMORY);
    state = ERROR;
    requestUpdate();
    return;
  }

  WiFi.mode(WIFI_STA);
  if (!startQuickConnect()) startWifiSelection();
}

bool EpubReaderTranslationActivity::startQuickConnect() {
  {
    RenderLock lock(*this);  // SD access shares the SPI bus with the display
    WIFI_STORE.loadFromFile();
  }
  const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
  if (lastSsid.empty()) return false;
  const auto cred = WIFI_STORE.findCredential(lastSsid);
  if (!cred) return false;
  if (cred->password.empty()) {
    WiFi.begin(cred->ssid.c_str());
  } else {
    WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
  }
  connectStartMs = millis();
  state = CONNECTING;
  requestUpdate();
  return true;
}

void EpubReaderTranslationActivity::pollQuickConnect() {
  const wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    // Same clock rule as the Wi-Fi list: any connection is a chance to correct the time, which
    // TLS certificate checks and the reading stats both depend on.
    if (clockNeedsNtpSync() && halClock.syncFromNTP()) {  // crosspoint: util/ClockSyncPolicy.h
      noteClockSynced();
    }
    LOG_DBG("XLAT", "Saved network joined in %lums", millis() - connectStartMs);  // crosspoint
    onWifiComplete(true);
    return;
  }
  if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL ||
      millis() - connectStartMs > QUICK_CONNECT_TIMEOUT_MS) {
    LOG_DBG("XLAT", "Saved network did not answer (status %d); opening the Wi-Fi list", static_cast<int>(status));
    WiFi.disconnect(false);
    startWifiSelection();
  }
}

void EpubReaderTranslationActivity::startWifiSelection() {
  state = WIFI_SELECTION;
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiComplete(!result.isCancelled); });
}

void EpubReaderTranslationActivity::onExit() {
  Activity::onExit();

  if (!hasPreTranslation && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestartToReader();
  }
}

bool EpubReaderTranslationActivity::readApiKey(std::string& keyOut) {
  // 192, not 256: the stack budget for a local is under 256 bytes (CLAUDE.md), and this is
  // still more than three times the longest key Google issues (a current AI Studio key is ~53
  // characters, the older AIza form 39).
  char buf[192];
  size_t len = Storage.readFileToBuffer(apiKeyPath().c_str(), buf, sizeof(buf));
  if (len == 0) return false;
  // readFileToBuffer stops at bufferSize-1 and reports the truncated length, so a key longer
  // than the buffer arrives silently cut in half and is rejected by the API as malformed. A
  // full buffer now means "this is not a key" rather than "here is most of one".
  if (len >= sizeof(buf) - 1) {
    LOG_ERR("XLAT", "gemini.key is %u+ bytes; that is not an API key", static_cast<unsigned>(len));
    return false;
  }

  size_t start = 0;
  // A UTF-8 BOM is what an editor on Windows writes when the file is saved as UTF-8, and the
  // documented way to install a key is to paste it into a text file. The three bytes go out in
  // front of the key, the API rejects it, and nothing on screen suggests the file is at fault.
  if (len - start >= 3 && static_cast<unsigned char>(buf[start]) == 0xEF &&
      static_cast<unsigned char>(buf[start + 1]) == 0xBB && static_cast<unsigned char>(buf[start + 2]) == 0xBF) {
    start += 3;
  }
  // Trim whitespace/newlines from BOTH ends: a leading space or newline corrupts the key exactly
  // as invisibly as a trailing one, and only the trailing end was handled.
  while (start < len && (buf[start] == '\n' || buf[start] == '\r' || buf[start] == ' ' || buf[start] == '\t')) {
    start++;
  }
  while (len > start && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' ' || buf[len - 1] == '\t')) {
    len--;
  }
  if (len <= start) return false;

  keyOut.assign(buf + start, len - start);
  return true;
}

// What the screen says when Google refuses the request. The status alone separates the causes a
// reader can actually act on -- a rejected key, an account with no billing set up, a quota that
// has run out -- from one another and from "the network is down", which is what a single generic
// failure message left every one of them looking like. The code is appended because it is the
// one detail that makes a bug report actionable without asking for logs.
static std::string translationHttpError(const int httpCode) {
  // tr() pastes its argument into StrId::, so each case names its string directly.
  const char* base = tr(STR_TRANSLATION_FAILED);
  switch (httpCode) {
    case 400:
      base = tr(STR_TRANSLATION_KEY_REJECTED);
      break;
    case 401:
    case 403:
      base = tr(STR_TRANSLATION_ACCESS_DENIED);
      break;
    case 429:
      base = tr(STR_TRANSLATION_QUOTA);
      break;
    default:
      if (httpCode >= 500) base = tr(STR_TRANSLATION_SERVICE_DOWN);
      break;
  }
  if (httpCode < 0) base = tr(STR_TRANSLATION_NO_RESPONSE);  // crosspoint: timeout / connection lost
  std::string msg = base;
  if (httpCode > 0) {
    char code[16];
    snprintf(code, sizeof(code), " (%d)", httpCode);
    msg += code;
  }
  return msg;
}

bool EpubReaderTranslationActivity::callGeminiApi(const std::string& apiKey, const char* model) {
  if (!model) model = GEMINI_MODEL;  // crosspoint: model chain
  lastHttpCode = 0;
  std::string url = "https://generativelanguage.googleapis.com/v1beta/models/";
  url += model;
  url += ":generateContent?key=";
  url += apiKey;
  // Only the translated text. The full reply also carries the model's thought signature and
  // usage counters -- 9 KB for a page whose translation is 600 bytes -- and the HTTP client
  // gathers the body in one growing string beside the TLS session: on the device that growth
  // ran out of contiguous heap and aborted.
  // crosspoint: + usage, and why a reply ended (MAX_TOKENS, SAFETY) when it carries no text.
  url += "&$fields=candidates.content.parts.text,candidates.finishReason,usageMetadata,modelVersion";

  // TLS/HTTP client init needs one large *contiguous* buffer (record buffers, X.509 parsing,
  // etc.), so the gate must check the largest allocatable block, not total free heap -- on a
  // fragmented heap (e.g. after CJK font/vertical-text work, which this session found leaves the
  // heap more fragmented than plain-text reading) total free can look comfortably above
  // MIN_HEAP_FOR_TLS while no single block that size actually exists, silently passing this check
  // only to fail deeper inside the TLS handshake instead of with this clear message.
  // Release the glyph caches AGAIN, right at the gate. onEnter() already did it, but the WiFi
  // selection screen and the return render in between re-warm them -- device log: 112KB free at
  // entry, 51KB largest block here, 4KB short of the threshold, so every translation paid a
  // silent restart (~10s: reboot, re-enter, reconnect WiFi) instead of just calling the API.
  // Nothing is drawn between here and the request, and the reader re-warms on its next page.
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->releaseAllFontMemory();
  }

  // Both floors, for the two different failure modes: the record buffer needs one contiguous
  // block, the handshake's session object and cert-verify temps need total room. A wrong guess
  // fails soft -- wolfSSL returns MEMORY_E rather than aborting under -fno-exceptions.
  const uint32_t maxAllocHeap = ESP.getMaxAllocHeap();
  const uint32_t freeHeapNow = ESP.getFreeHeap();
  LOG_DBG("XLAT", "Calling Gemini (free: %u, max alloc: %u)", static_cast<unsigned>(freeHeapNow),
          static_cast<unsigned>(maxAllocHeap));
  if (maxAllocHeap < MIN_HEAP_FOR_TLS || freeHeapNow < MIN_FREE_FOR_TLS) {
    LOG_ERR("XLAT", "Insufficient heap for TLS: free %u (need %u), largest block %u (need %u)",
            static_cast<unsigned>(freeHeapNow), static_cast<unsigned>(MIN_FREE_FOR_TLS),
            static_cast<unsigned>(maxAllocHeap), static_cast<unsigned>(MIN_HEAP_FOR_TLS));
    // Retry once on a pristine post-restart heap; see onEnter() for the rationale.
    if (!resumedAfterRestart && stashAndRestart()) return false;
    errorMessage = tr(STR_TRANSLATION_LOW_MEMORY);
    return false;
  }

  JsonDocument reqDoc;
  auto contents = reqDoc["contents"].to<JsonArray>();
  auto part = contents.add<JsonObject>();
  auto parts = part["parts"].to<JsonArray>();
  auto textPart = parts.add<JsonObject>();
  // crosspoint: numbered, close-to-literal sentences. A fluent translation of a published book
  // reproduces its published English edition, which Gemini withholds as RECITATION.
  textPart["text"] = std::string(
                         "Translate the following text to English sentence by sentence. Number each sentence "
                         "and start it on a new line. Keep each close and literal to the original wording, "
                         "but easy to read. Return only the translation, no commentary.\n\n") +
                     sourceText;

  auto config = reqDoc["generationConfig"].to<JsonObject>();
  config["maxOutputTokens"] = 2048;
  // The model thinks before answering, and those tokens count against maxOutputTokens: at its
  // default level a long page could use up the budget and return no translation, or outlast the
  // timeout below. A translation needs little reasoning; "low" is the lowest level 3.8 accepts.
  // crosspoint: per model, from GEMINI_MODELS.
  const char* thinking = "low";
  for (const auto& m : GEMINI_MODELS) {
    if (strcmp(m.name, model) == 0) thinking = m.thinking;
  }
  config["thinkingConfig"].to<JsonObject>()["thinkingLevel"] = thinking;

  std::string body;
  serializeJson(reqDoc, body);

  freeink::SecureHttpClient http;
  http.setInsecure();  // same as KOReaderSync: the wolfSSL transport has no CA bundle wired up
  http.setTimeout(30000);
  if (!http.begin(url)) {
    LOG_ERR("XLAT", "Failed to open connection");
    errorMessage = tr(STR_TRANSLATION_FAILED);
    return false;
  }
  http.addHeader("Content-Type", "application/json");

  const int httpCode = http.POST(body);
  lastHttpCode = httpCode;  // crosspoint
  const std::string response = http.getString();
  http.end();

  LOG_DBG("XLAT", "Gemini response: HTTP %d (%u bytes)", httpCode, static_cast<unsigned>(response.size()));

  if (httpCode != 200 || response.empty()) {
    LOG_ERR("XLAT", "API call failed: http=%d", httpCode);
    errorMessage = translationHttpError(httpCode);
    return false;
  }

  JsonDocument respDoc;
  DeserializationError jsonErr = deserializeJson(respDoc, response);
  if (jsonErr) {
    LOG_ERR("XLAT", "JSON parse error: %s", jsonErr.c_str());
    errorMessage = tr(STR_TRANSLATION_FAILED);
    return false;
  }

  const char* text = respDoc["candidates"][0]["content"]["parts"][0]["text"];
  if (!text) {
    // crosspoint: name why the reply came back empty instead of a bare "failed".
    const char* finish = respDoc["candidates"][0]["finishReason"] | "?";
    LOG_ERR("XLAT", "No text in Gemini response (finish=%s)", finish);
    if (strcmp(finish, "RECITATION") == 0) {
      errorMessage = tr(STR_TRANSLATION_RECITATION);
    } else if (strcmp(finish, "SAFETY") == 0 || strcmp(finish, "PROHIBITED_CONTENT") == 0 ||
               strcmp(finish, "BLOCKLIST") == 0 || strcmp(finish, "SPII") == 0) {
      errorMessage = tr(STR_TRANSLATION_BLOCKED);
    } else if (strcmp(finish, "MAX_TOKENS") == 0) {
      errorMessage = tr(STR_TRANSLATION_TOO_LONG);
    } else {
      errorMessage = std::string(tr(STR_TRANSLATION_FAILED)) + " (" + finish + ")";
    }
    return false;
  }

  translatedText = text;
  errorMessage.clear();  // crosspoint: a failed first model left its message here
  // crosspoint: usage for the panel footer.
  promptTokens = respDoc["usageMetadata"]["promptTokenCount"] | 0u;
  outputTokens =
      (respDoc["usageMetadata"]["candidatesTokenCount"] | 0u) + (respDoc["usageMetadata"]["thoughtsTokenCount"] | 0u);
  snprintf(modelVersion, sizeof(modelVersion), "%s", respDoc["modelVersion"] | model);
  LOG_INF("XLAT", "Usage: model=%s in=%u out=%u", modelVersion, static_cast<unsigned>(promptTokens),
          static_cast<unsigned>(outputTokens));
  return true;
}

void EpubReaderTranslationActivity::onWifiComplete(bool success) {
  if (!success) {
    errorMessage = tr(STR_TRANSLATION_WIFI_FAILED);
    state = ERROR;
    requestUpdate();
    return;
  }

  std::string apiKey;
  if (!readApiKey(apiKey)) {
    LOG_ERR("XLAT", "No usable API key at %s", apiKeyPath().c_str());  // crosspoint
    errorMessage = tr(STR_TRANSLATION_NO_API_KEY);
    state = ERROR;
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    state = TRANSLATING;
  }
  requestUpdateAndWait();

  // crosspoint: out of quota or busy -> the next model, which has its own quota and capacity.
  modelIndex = 0;
  bool translated = callGeminiApi(apiKey, GEMINI_MODELS[0].name);
  while (!translated && (lastHttpCode == 429 || lastHttpCode >= 500) && modelIndex + 1 < GEMINI_MODEL_COUNT) {
    LOG_INF("XLAT", "%s answered %d; trying %s", GEMINI_MODELS[modelIndex].name, lastHttpCode,
            GEMINI_MODELS[modelIndex + 1].name);
    ++modelIndex;
    translated = callGeminiApi(apiKey, GEMINI_MODELS[modelIndex].name);
  }
  LOG_DBG("XLAT", "Result ok=%d model=%u err='%s' chars=%u", translated ? 1 : 0, static_cast<unsigned>(modelIndex),
          errorMessage.c_str(), static_cast<unsigned>(translatedText.size()));  // crosspoint
  if (translated) {
    RenderLock lock(*this);
    const auto body = pageLayout(renderer).body;  // crosspoint
    textPages.setLineSpacingPercent(TRANSLATION_LINE_SPACING_PERCENT);
    textPages.layout(renderer, TRANSLATION_FONT_ID, translatedText, body.width, body.height);
    currentPage = 0;
    state = SHOWING_RESULT;
  } else {
    RenderLock lock(*this);
    state = ERROR;
  }
  requestUpdate();
}

bool EpubReaderTranslationActivity::canRetry() const {
  return !hasPreTranslation && WiFi.getMode() != WIFI_MODE_NULL && errorMessage != tr(STR_TRANSLATION_NO_API_KEY);
}

void EpubReaderTranslationActivity::retry() {
  LOG_DBG("XLAT", "Retry after '%s'", errorMessage.c_str());
  {
    RenderLock lock(*this);  // render() reads both
    errorMessage.clear();
  }
  if (WiFi.status() == WL_CONNECTED) {
    onWifiComplete(true);
  } else {
    WiFi.disconnect(false);
    if (!startQuickConnect()) startWifiSelection();
  }
}

void EpubReaderTranslationActivity::cancel() {
  LOG_DBG("XLAT", "Closed in state %d", static_cast<int>(state));  // crosspoint
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void EpubReaderTranslationActivity::stepPage(const int direction) {
  if (state != SHOWING_RESULT) return;
  const int next = currentPage + direction;
  if (next < 0 || next >= textPages.pageCount()) return;
  currentPage = next;
  requestUpdate();
}

void EpubReaderTranslationActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    cancel();
    return;
  }
  if (state == CONNECTING) {
    pollQuickConnect();
    return;
  }

  if (state == ERROR && canRetry() && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    retry();  // crosspoint
    return;
  }

  // crosspoint: a tap in the middle third closes the page; the sides turn it as the reader does.
  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTapped(tx, ty)) {
    const int third = renderer.getScreenWidth() / 3;
    if (tx >= third && tx < renderer.getScreenWidth() - third) {
      haptic_feedback::touchAction();
      cancel();
      return;
    }
  }
  if (const int scroll = ReaderUtils::definitionScrollSwipe(mappedInput)) {
    stepPage(scroll > 0 ? 1 : -1);
    return;
  }
  const auto turn = ReaderUtils::detectTouchPageTurn(renderer, mappedInput);
  if (turn.next || turn.prev) {
    stepPage(turn.next ? 1 : -1);
    return;
  }

  buttonNavigator.onNext([this] { stepPage(1); });
  buttonNavigator.onPrevious([this] { stepPage(-1); });
}

void EpubReaderTranslationActivity::render(RenderLock&&) {
  LOG_DBG("XLAT", "Render state=%d pages=%d page=%d", static_cast<int>(state), textPages.pageCount(),
          currentPage);    // crosspoint
  renderer.clearScreen();  // crosspoint: a full page, not a panel over the reader's page
  const PageLayout page = pageLayout(renderer);

  char counter[16] = "";
  if (state == SHOWING_RESULT && textPages.pageCount() > 1) {
    snprintf(counter, sizeof(counter), "%d/%d", currentPage + 1, textPages.pageCount());
  }
  // crosspoint: the footer names the model and what the reply cost, e.g.
  // "gemini-3.8-flash | 1189 tok | ~$0.0031".
  char footer[80];
  snprintf(footer, sizeof(footer), "%s", hasPreTranslation ? "" : "Gemini");
  if (state == SHOWING_RESULT && modelVersion[0] != '\0') {
    const GeminiModel& priced = GEMINI_MODELS[modelIndex];
    const unsigned tokens = static_cast<unsigned>(promptTokens + outputTokens);
    if (priced.priceIn == 0) {
      snprintf(footer, sizeof(footer), "%s | %u tok", modelVersion, tokens);
    } else {
      const uint64_t millicents = (static_cast<uint64_t>(promptTokens) * priced.priceIn +
                                   static_cast<uint64_t>(outputTokens) * priced.priceOut) /
                                  1000000;  // 1 millicent = $0.00001
      snprintf(footer, sizeof(footer), "%s | %u tok | ~$%u.%05u", modelVersion, tokens,
               static_cast<unsigned>(millicents / 100000), static_cast<unsigned>(millicents % 100000));
    }
  }
  const int left = page.body.x;
  const int right = page.body.x + page.body.width;
  renderer.drawLine(left, page.footerY - FOOTER_GAP, right, page.footerY - FOOTER_GAP, 1, true);
  int footerWidth = page.body.width;
  if (counter[0] != '\0') {
    const int counterWidth = renderer.getTextWidth(SMALL_FONT_ID, counter);
    renderer.drawText(SMALL_FONT_ID, right - counterWidth, page.footerY, counter);
    footerWidth -= counterWidth + FOOTER_GAP;
  }
  if (footer[0] != '\0' && footerWidth > 0) {
    renderer.drawText(SMALL_FONT_ID, left, page.footerY,
                      renderer.truncatedText(SMALL_FONT_ID, footer, footerWidth).c_str());
  }

  if (state == SHOWING_RESULT) {
    auto* fcm = renderer.getFontCacheManager();
    auto scope = fcm->createPrewarmScope();
    textPages.draw(renderer, TRANSLATION_FONT_ID, page.body.x, page.body.y, translatedText, currentPage);
    scope.endScanAndPrewarm();
    textPages.draw(renderer, TRANSLATION_FONT_ID, page.body.x, page.body.y, translatedText, currentPage);
  } else {
    const char* status = state == CONNECTING    ? tr(STR_CONNECTING_SAVED_WIFI)
                         : state == TRANSLATING ? tr(STR_TRANSLATING)
                         : state == ERROR       ? errorMessage.c_str()
                                                : "";
    // crosspoint: status and errors in the popup font, as the reader's other progress messages.
    const EpdFontFamily::Style style =
        UITheme::getInstance().getMetrics().popupTextBold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    UITheme::drawCenteredWrappedText(renderer, page.body, uiScaleSpec().smallFontId, status, 6, true, style);
  }

  const bool paged = state == SHOWING_RESULT;
  const char* confirmLabel = state == ERROR && canRetry() ? tr(STR_RETRY) : "";  // crosspoint
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, paged && currentPage > 0 ? "<" : "",
                                            paged && currentPage + 1 < textPages.pageCount() ? ">" : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
