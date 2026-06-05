#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Document matching method for KOReader sync
enum class DocumentMatchMethod : uint8_t {
  FILENAME = 0,  // Match by filename (simpler, works across different file sources)
  BINARY = 1,    // Match by partial MD5 of file content (more accurate, but files must be identical)
};

/**
 * Per-server configuration for a KOReader sync server.
 * Password is kept plaintext in RAM; XOR-obfuscated with device MAC on disk.
 */
struct KOReaderSyncServer {
  std::string name;
  std::string serverUrl;   // empty = use default sync.koreader.rocks
  std::string username;
  std::string password;
  DocumentMatchMethod matchMethod = DocumentMatchMethod::FILENAME;
};

class KOReaderCredentialStore;

namespace KOReaderJsonIO {
bool save(const KOReaderCredentialStore& store, const char* path);
bool load(KOReaderCredentialStore& store, const char* json, bool* needsResave);
}  // namespace KOReaderJsonIO

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
class KOReaderCredentialStore {
 private:
  static KOReaderCredentialStore instance;
  std::vector<KOReaderSyncServer> servers;
  int activeIndex = -1;  // -1 only when count == 0

  static constexpr size_t MAX_SERVERS = 8;

  // Private constructor for singleton
  KOReaderCredentialStore() = default;

  bool loadFromBinaryFile();

  friend bool KOReaderJsonIO::save(const KOReaderCredentialStore&, const char*);
  friend bool KOReaderJsonIO::load(KOReaderCredentialStore&, const char*, bool*);

 public:
  // Delete copy constructor and assignment
  KOReaderCredentialStore(const KOReaderCredentialStore&) = delete;
  KOReaderCredentialStore& operator=(const KOReaderCredentialStore&) = delete;

  // Get singleton instance
  static KOReaderCredentialStore& getInstance() { return instance; }
  static constexpr size_t maxServers() { return MAX_SERVERS; }

  // Save/load from SD card
  bool saveToFile() const;
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
};

// Helper macro to access credential store
#define KOREADER_STORE KOReaderCredentialStore::getInstance()
