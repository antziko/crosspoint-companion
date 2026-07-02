#pragma once
#include <HalStorage.h>

#include <functional>
#include <string>

/**
 * HTTP client utility for fetching content and downloading files. Built on
 * esp_http_client: https is verified against the CA bundle, plain http is
 * used for local servers (transport is chosen from the URL scheme).
 */
class HttpDownloader {
 public:
  using ProgressCallback = std::function<void(size_t downloaded, size_t total)>;
  // Called with each body chunk as it arrives; return false to abort. Lets a
  // streaming parser consume the response without buffering the whole body.
  using DataCallback = std::function<bool(const uint8_t* data, size_t len)>;

  enum DownloadError {
    OK = 0,
    HTTP_ERROR,
    FILE_ERROR,
    ABORTED,
  };

  /**
   * Fetch text content from a URL with optional credentials.
   */
  static bool fetchUrl(const std::string& url, std::string& outContent, const std::string& username = "",
                       const std::string& password = "");

  static bool fetchUrl(const std::string& url, Stream& stream, const std::string& username = "",
                       const std::string& password = "");

  /**
   * Stream the response body to onData as it arrives, without buffering it.
   */
  static bool fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username = "",
                       const std::string& password = "");

  /**
   * Download a file to the SD card with optional credentials.
   *
   * If errorDetail is non-null, it receives a short human-readable failure
   * reason (e.g. "HTTP 401", "connect failed: ESP_ERR_...", "out of memory")
   * on any non-OK return, so callers can show the real cause on screen instead
   * of a generic message.
   *
   * caPemOverride (default null) pins specific root CAs (concatenated PEM) for this
   * request instead of the full CA bundle. Null = verify against the bundle (the
   * behaviour every existing caller relies on). Used by font downloads, whose GitHub
   * chain the prebuilt bundle mis-verifies (see FontDownloadCA.h).
   *
   * caPemRedirect (default null) pins a SECOND, different root for the host a 30x
   * redirect points to, so each TLS handshake parses only the single root that host
   * chains to. This halves the CA the mbedTLS arena holds during the heap-critical
   * verify on the X4 (github.com -> USERTrust ECC, then release-assets CDN -> ISRG
   * Root X1). Only honoured when caPemOverride is also set; when null, redirects are
   * followed on the same connection/cert exactly as before (OPDS/KOSync path).
   */
  static DownloadError downloadToFile(const std::string& url, const std::string& destPath,
                                      ProgressCallback progress = nullptr, bool* cancelFlag = nullptr,
                                      const std::string& username = "", const std::string& password = "",
                                      std::string* errorDetail = nullptr, const char* caPemOverride = nullptr,
                                      const char* caPemRedirect = nullptr);
};
