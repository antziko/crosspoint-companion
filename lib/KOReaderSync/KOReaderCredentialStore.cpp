#include "KOReaderCredentialStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <MD5Builder.h>
#include <ObfuscationUtils.h>
#include <Serialization.h>

#include "KOReaderJsonIO.h"

// Initialize the static instance
KOReaderCredentialStore KOReaderCredentialStore::instance;

namespace {
// File format version (for binary migration)
constexpr uint8_t KOREADER_FILE_VERSION = 1;

// File paths
constexpr char KOREADER_FILE_BIN[] = "/.crosspoint/koreader.bin";
constexpr char KOREADER_FILE_JSON[] = "/.crosspoint/koreader.json";
constexpr char KOREADER_FILE_BAK[] = "/.crosspoint/koreader.bin.bak";

// Default sync server URL
constexpr char DEFAULT_SERVER_URL[] = "https://sync.koreader.rocks:443";

// Legacy obfuscation key - "KOReader" in ASCII (only used for binary migration)
constexpr uint8_t LEGACY_OBFUSCATION_KEY[] = {0x4B, 0x4F, 0x52, 0x65, 0x61, 0x64, 0x65, 0x72};
constexpr size_t LEGACY_KEY_LENGTH = sizeof(LEGACY_OBFUSCATION_KEY);

// Returned by const-ref getters when no active server exists
static const std::string EMPTY_STRING;

void legacyDeobfuscate(std::string& data) {
  for (size_t i = 0; i < data.size(); i++) {
    data[i] ^= LEGACY_OBFUSCATION_KEY[i % LEGACY_KEY_LENGTH];
  }
}
}  // namespace

bool KOReaderCredentialStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  return KOReaderJsonIO::save(*this, KOREADER_FILE_JSON);
}

bool KOReaderCredentialStore::loadFromFile() {
  // Try JSON first
  if (Storage.exists(KOREADER_FILE_JSON)) {
    String json = Storage.readFile(KOREADER_FILE_JSON);
    if (!json.isEmpty()) {
      bool resave = false;
      bool result = KOReaderJsonIO::load(*this, json.c_str(), &resave);
      if (result && resave) {
        saveToFile();
        LOG_DBG("KRS", "Resaved KOReader credentials to update format");
      }
      return result;
    }
  }

  // Fall back to binary migration
  if (Storage.exists(KOREADER_FILE_BIN)) {
    if (loadFromBinaryFile()) {
      if (saveToFile()) {
        Storage.rename(KOREADER_FILE_BIN, KOREADER_FILE_BAK);
        LOG_DBG("KRS", "Migrated koreader.bin to koreader.json");
        return true;
      } else {
        LOG_ERR("KRS", "Failed to save KOReader credentials during migration");
        return false;
      }
    }
  }

  LOG_DBG("KRS", "No credentials file found");
  return false;
}

bool KOReaderCredentialStore::loadFromBinaryFile() {
  HalFile file;
  if (!Storage.openFileForRead("KRS", KOREADER_FILE_BIN, file)) {
    return false;
  }

  uint8_t version;
  serialization::readPod(file, version);
  if (version != KOREADER_FILE_VERSION) {
    LOG_DBG("KRS", "Unknown file version: %u", version);
    return false;
  }

  KOReaderSyncServer server;
  server.name = "KOReader Sync";  // default name for migrated single server

  if (file.available()) {
    serialization::readString(file, server.username);
  }

  if (file.available()) {
    serialization::readString(file, server.password);
    legacyDeobfuscate(server.password);
  }

  if (file.available()) {
    serialization::readString(file, server.serverUrl);
  }

  if (file.available()) {
    uint8_t method;
    serialization::readPod(file, method);
    server.matchMethod = static_cast<DocumentMatchMethod>(method);
  }

  servers.clear();
  servers.reserve(MAX_SERVERS);
  servers.push_back(std::move(server));
  activeIndex = 0;

  LOG_DBG("KRS", "Loaded KOReader credentials from binary for user: %s", servers[0].username.c_str());
  return true;
}

// --- Multi-server CRUD ---

bool KOReaderCredentialStore::addServer(const KOReaderSyncServer& server) {
  if (servers.size() >= MAX_SERVERS) {
    LOG_DBG("KRS", "Cannot add more servers, limit of %zu reached", MAX_SERVERS);
    return false;
  }
  servers.push_back(server);
  // First server added into empty store becomes the active server
  if (activeIndex < 0) {
    activeIndex = 0;
  }
  LOG_DBG("KRS", "Added KOReader sync server: %s", server.name.c_str());
  return saveToFile();
}

bool KOReaderCredentialStore::updateServer(size_t index, const KOReaderSyncServer& server) {
  if (index >= servers.size()) {
    return false;
  }
  servers[index] = server;
  LOG_DBG("KRS", "Updated KOReader sync server at index %zu", index);
  return saveToFile();
}

bool KOReaderCredentialStore::removeServer(size_t index) {
  if (index >= servers.size()) {
    return false;
  }
  // Always-one-default invariant: refuse to remove the last server
  if (servers.size() == 1) {
    LOG_DBG("KRS", "Cannot remove the last sync server");
    return false;
  }
  LOG_DBG("KRS", "Removed KOReader sync server: %s", servers[index].name.c_str());
  servers.erase(servers.begin() + static_cast<ptrdiff_t>(index));
  // Fix up activeIndex
  if (activeIndex > static_cast<int>(index)) {
    activeIndex--;
  } else if (activeIndex == static_cast<int>(index)) {
    // Removed the active server: clamp to first
    activeIndex = 0;
  }
  return saveToFile();
}

const KOReaderSyncServer* KOReaderCredentialStore::getServer(size_t index) const {
  if (index >= servers.size()) {
    return nullptr;
  }
  return &servers[index];
}

void KOReaderCredentialStore::setActiveIndex(int index) {
  if (index < 0 || static_cast<size_t>(index) >= servers.size()) {
    LOG_DBG("KRS", "setActiveIndex: index %d out of range", index);
    return;
  }
  activeIndex = index;
  LOG_DBG("KRS", "Active KOReader sync server: %s", servers[index].name.c_str());
}

// --- Legacy active-server accessors ---

const std::string& KOReaderCredentialStore::getUsername() const {
  if (activeIndex < 0 || static_cast<size_t>(activeIndex) >= servers.size()) return EMPTY_STRING;
  return servers[activeIndex].username;
}

const std::string& KOReaderCredentialStore::getPassword() const {
  if (activeIndex < 0 || static_cast<size_t>(activeIndex) >= servers.size()) return EMPTY_STRING;
  return servers[activeIndex].password;
}

std::string KOReaderCredentialStore::getMd5Password() const {
  const std::string& password = getPassword();
  if (password.empty()) {
    return "";
  }
  MD5Builder md5;
  md5.begin();
  md5.add(password.c_str());
  md5.calculate();
  return md5.toString().c_str();
}

bool KOReaderCredentialStore::hasCredentials() const {
  if (activeIndex < 0 || static_cast<size_t>(activeIndex) >= servers.size()) return false;
  const auto& s = servers[activeIndex];
  return !s.username.empty() && !s.password.empty();
}

void KOReaderCredentialStore::clearCredentials() {
  if (activeIndex < 0 || static_cast<size_t>(activeIndex) >= servers.size()) return;
  servers[activeIndex].username.clear();
  servers[activeIndex].password.clear();
  saveToFile();
  LOG_DBG("KRS", "Cleared credentials for active server");
}

void KOReaderCredentialStore::setCredentials(const std::string& user, const std::string& pass) {
  if (activeIndex < 0) {
    // No servers yet: create a default server to hold these credentials
    KOReaderSyncServer server;
    server.name = "KOReader Sync";
    server.username = user;
    server.password = pass;
    servers.reserve(MAX_SERVERS);
    servers.push_back(std::move(server));
    activeIndex = 0;
    LOG_DBG("KRS", "Created default server and set credentials for user: %s", user.c_str());
    return;
  }
  if (static_cast<size_t>(activeIndex) >= servers.size()) return;
  servers[activeIndex].username = user;
  servers[activeIndex].password = pass;
  LOG_DBG("KRS", "Set credentials for active server, user: %s", user.c_str());
}

void KOReaderCredentialStore::setServerUrl(const std::string& url) {
  if (activeIndex < 0 || static_cast<size_t>(activeIndex) >= servers.size()) return;
  servers[activeIndex].serverUrl = url;
  LOG_DBG("KRS", "Set server URL for active server: %s", url.empty() ? "(default)" : url.c_str());
}

const std::string& KOReaderCredentialStore::getServerUrl() const {
  if (activeIndex < 0 || static_cast<size_t>(activeIndex) >= servers.size()) return EMPTY_STRING;
  return servers[activeIndex].serverUrl;
}

std::string KOReaderCredentialStore::getBaseUrl() const {
  const std::string& serverUrl = getServerUrl();
  std::string url;
  if (serverUrl.empty()) {
    url = DEFAULT_SERVER_URL;
  } else if (serverUrl.find("://") == std::string::npos) {
    // Normalize URL: add http:// if no protocol specified (local servers typically don't have SSL)
    url = "http://" + serverUrl;
  } else {
    url = serverUrl;
  }
  // Strip trailing slashes to avoid double-slash in API paths
  while (!url.empty() && url.back() == '/') {
    url.pop_back();
  }
  return url;
}

void KOReaderCredentialStore::setMatchMethod(DocumentMatchMethod method) {
  if (activeIndex < 0 || static_cast<size_t>(activeIndex) >= servers.size()) return;
  servers[activeIndex].matchMethod = method;
  LOG_DBG("KRS", "Set match method for active server: %s",
          method == DocumentMatchMethod::FILENAME ? "Filename" : "Binary");
}

DocumentMatchMethod KOReaderCredentialStore::getMatchMethod() const {
  if (activeIndex < 0 || static_cast<size_t>(activeIndex) >= servers.size()) return DocumentMatchMethod::FILENAME;
  return servers[activeIndex].matchMethod;
}
