#include "KOReaderJsonIO.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include "KOReaderCredentialStore.h"

namespace KOReaderJsonIO {

bool save(const KOReaderCredentialStore& store, const char* path) {
  JsonDocument doc;
  doc["activeIndex"] = store.activeIndex;

  JsonArray arr = doc["servers"].to<JsonArray>();
  for (const auto& server : store.servers) {
    JsonObject obj = arr.add<JsonObject>();
    obj["name"] = server.name;
    obj["serverUrl"] = server.serverUrl;
    obj["username"] = server.username;
    obj["password_obf"] = obfuscation::obfuscateToBase64(server.password);
    obj["matchMethod"] = static_cast<uint8_t>(server.matchMethod);
  }

  String json;
  serializeJson(doc, json);
  return Storage.writeFile(path, json);
}

bool load(KOReaderCredentialStore& store, const char* json, bool* needsResave) {
  if (needsResave) *needsResave = false;
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("KRS", "JSON parse error: %s", error.c_str());
    return false;
  }

  store.servers.clear();
  store.servers.reserve(KOReaderCredentialStore::maxServers());

  if (doc.containsKey("servers")) {
    // New multi-server format
    JsonArray arr = doc["servers"].as<JsonArray>();
    for (JsonObject obj : arr) {
      if (store.servers.size() >= KOReaderCredentialStore::maxServers()) break;
      KOReaderSyncServer server;
      server.name = obj["name"] | std::string("");
      server.serverUrl = obj["serverUrl"] | std::string("");
      server.username = obj["username"] | std::string("");
      bool ok = false;
      server.password = obfuscation::deobfuscateFromBase64(obj["password_obf"] | "", &ok);
      if (!ok || server.password.empty()) {
        server.password = obj["password"] | std::string("");
        if (!server.password.empty() && needsResave) *needsResave = true;
      }
      uint8_t method = obj["matchMethod"] | static_cast<uint8_t>(0);
      server.matchMethod = static_cast<DocumentMatchMethod>(method);
      store.servers.push_back(std::move(server));
    }

    int loadedActive = doc["activeIndex"] | 0;
    if (store.servers.empty()) {
      store.activeIndex = -1;
    } else {
      // Clamp to valid range
      if (loadedActive < 0 || static_cast<size_t>(loadedActive) >= store.servers.size()) {
        store.activeIndex = 0;
      } else {
        store.activeIndex = loadedActive;
      }
    }
  } else {
    // Migration: old single-record format (top-level username / serverUrl / matchMethod fields)
    std::string user = doc["username"] | std::string("");
    bool ok = false;
    std::string pass = obfuscation::deobfuscateFromBase64(doc["password_obf"] | "", &ok);
    if (!ok || pass.empty()) {
      pass = doc["password"] | std::string("");
    }
    std::string serverUrl = doc["serverUrl"] | std::string("");
    uint8_t method = doc["matchMethod"] | static_cast<uint8_t>(0);

    KOReaderSyncServer server;
    // Derive a readable name from the server URL host
    if (!serverUrl.empty()) {
      std::string host = serverUrl;
      size_t protoEnd = host.find("://");
      if (protoEnd != std::string::npos) host = host.substr(protoEnd + 3);
      size_t slashPos = host.find('/');
      if (slashPos != std::string::npos) host = host.substr(0, slashPos);
      size_t colonPos = host.find(':');
      if (colonPos != std::string::npos) host = host.substr(0, colonPos);
      server.name = host.empty() ? "KOReader Sync" : host;
    } else {
      server.name = "KOReader Sync";
    }
    server.serverUrl = serverUrl;
    server.username = user;
    server.password = pass;
    server.matchMethod = static_cast<DocumentMatchMethod>(method);
    store.servers.push_back(std::move(server));
    store.activeIndex = 0;
    // Force rewrite in the new multi-server format
    if (needsResave) *needsResave = true;
  }

  LOG_DBG("KRS", "Loaded %zu KOReader sync servers, active=%d", store.servers.size(), store.activeIndex);
  return true;
}

}  // namespace KOReaderJsonIO
