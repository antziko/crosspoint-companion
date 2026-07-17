#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>
#include <vector>

// Document matching method for KOReader sync
enum class DocumentMatchMethod : uint8_t {
  FILENAME = 0,  // Match by filename (simpler, works across different file sources)
  BINARY = 1,    // Match by partial MD5 of file content (more accurate, but files must be identical)
};

// How manual "Sync Progress" resolves differences after fetching remote progress (#2192).
enum class KOReaderSyncBehavior : uint8_t {
  ASK_EVERY_TIME = 0,  // Always show the Apply/Upload choice (this branch's default).
  SMART = 1,           // Auto-resolve simple cases using the furthest progress.
};

/**
 * Per-server configuration for a KOReader sync server.
 * Password is kept plaintext in RAM; XOR-obfuscated with device MAC on disk.
 */
struct KOReaderSyncServer {
  std::string name;
  std::string serverUrl;  // empty = use default sync.koreader.rocks
  std::string username;
  std::string password;
  DocumentMatchMethod matchMethod = DocumentMatchMethod::FILENAME;
  bool sendMetadata = false;  // Send document metadata (filename/title/authors) with progress sync (#1820)
  // Default ASK_EVERY_TIME preserves this branch's always-prompt sync flow; Smart is opt-in (#2192).
  KOReaderSyncBehavior syncBehavior = KOReaderSyncBehavior::ASK_EVERY_TIME;
};

/**
 * Singleton class for storing KOReader sync server configurations on the SD card.
 * Supports multiple servers; exactly one is the "active" server used for all sync operations.
 * Passwords are XOR-obfuscated with the device's unique hardware MAC address
 * and base64-encoded before writing to JSON (not cryptographically secure,
 * but prevents casual reading and ties credentials to the specific device).
 *
 * Invariant: activeIndex is always valid (0..count-1) whenever count >= 1.
 * The last remaining server cannot be deleted.
 */
class KOReaderCredentialStore : public PersistableStore<KOReaderCredentialStore> {
 private:
  std::vector<KOReaderSyncServer> servers;
  int activeIndex = -1;  // -1 only when count == 0

  static constexpr size_t MAX_SERVERS = 8;

  // Private constructor for singleton
  KOReaderCredentialStore() = default;
  ~KOReaderCredentialStore() = default;

  friend class PersistableStore<KOReaderCredentialStore>;

  // One-time migration of the legacy koreader.bin binary format into `servers`.
  bool loadFromBinaryFile();

 public:
  static const char* getFilePath() { return "/.crosspoint/koreader.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);
  static constexpr size_t maxServers() { return MAX_SERVERS; }

  // Overrides PersistableStore::loadFromFile to add a one-time migration from the
  // legacy koreader.bin binary format when no koreader.json exists yet.
  bool loadFromFile();

  // --- Multi-server CRUD ---
  // Returns false if at capacity. First addServer into empty store sets activeIndex = 0.
  bool addServer(const KOReaderSyncServer& server);
  bool updateServer(size_t index, const KOReaderSyncServer& server);
  // Returns false if index is invalid or this is the last remaining server.
  bool removeServer(size_t index);

  const std::vector<KOReaderSyncServer>& getServers() const { return servers; }
  const KOReaderSyncServer* getServer(size_t index) const;
  size_t getCount() const { return servers.size(); }
  bool hasServers() const { return !servers.empty(); }

  // Active server management. setActiveIndex does nothing if index out of range.
  void setActiveIndex(int index);
  int getActiveIndex() const { return activeIndex; }

  // --- Legacy active-server accessors (unchanged signatures; used by sync consumers and web UI) ---
  // All return empty/default when no active server (count == 0).
  const std::string& getUsername() const;
  const std::string& getPassword() const;
  std::string getMd5Password() const;
  bool hasCredentials() const;

  // Clears username+password on the active server only.
  void clearCredentials();

  // setCredentials / setServerUrl / setMatchMethod operate on the active server.
  // If no servers exist yet, setCredentials creates a default server first.
  void setCredentials(const std::string& user, const std::string& pass);
  void setServerUrl(const std::string& url);
  const std::string& getServerUrl() const;

  // Get base URL for API calls (protocol normalization + default fallback).
  std::string getBaseUrl() const;

  void setMatchMethod(DocumentMatchMethod method);
  DocumentMatchMethod getMatchMethod() const;

  // sendMetadata operates on the active server (mirrors setMatchMethod/getMatchMethod).
  void setSendMetadata(bool enabled);
  bool getSendMetadata() const;

  // syncBehavior operates on the active server (mirrors setSendMetadata/getSendMetadata).
  void setSyncBehavior(KOReaderSyncBehavior behavior);
  KOReaderSyncBehavior getSyncBehavior() const;
};

// Helper macro to access credential store
#define KOREADER_STORE KOReaderCredentialStore::getInstance()
