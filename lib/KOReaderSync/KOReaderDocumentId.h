#pragma once
#include <string>

/**
 * Calculate KOReader document ID (partial MD5 hash).
 *
 * KOReader identifies documents using a partial MD5 hash of the file content.
 * The algorithm reads 1024 bytes at specific offsets and computes the MD5 hash
 * of the concatenated data.
 *
 * Offsets are calculated as: 1024 << (2*i) for i = -1 to 10
 * Producing: 256, 1024, 4096, 16384, 65536, 262144, 1048576, 4194304,
 *            16777216, 67108864, 268435456, 1073741824 bytes
 *
 * If an offset is beyond the file size, it is skipped.
 */
class KOReaderDocumentId {
 public:
  /**
   * Calculate the KOReader document hash for a file (binary/content-based).
   *
   * @param filePath Path to the file (typically an EPUB)
   * @return 32-character lowercase hex string, or empty string on failure
   */
  static std::string calculate(const std::string& filePath);

  /**
   * Calculate document hash from filename only (filename-based sync mode).
   * This is simpler and works when files have the same name across devices.
   *
   * @param filePath Path to the file (only the filename portion is used)
   * @return 32-character lowercase hex MD5 of the filename
   */
  static std::string calculateFromFilename(const std::string& filePath);

  /**
   * Strip the auto-epub-optimizer device tag "(X<digits>)" so optimized copies
   * hash to the same key as the original (and across X3/X4 devices). Removes the
   * tag in either placement:
   *   - leading prefix  "(X<digits>) " at the very start of the name, or
   *   - trailing suffix " (X<digits>)" immediately before the final extension.
   * Both require the separating space; either or both may be present.
   *
   *   "(X4) Book.epub"  -> "Book.epub"      "Book (X4).epub"   -> "Book.epub"
   *   "(X12) Book.epub" -> "Book.epub"      "Book.epub"        -> "Book.epub"
   *   "(X4)Book.epub"   -> unchanged        "Book(X4).epub"    -> unchanged (no space)
   *   "(X4) Bk (X4).epub" -> "Bk.epub"      "My Bk (A) (X4).ep"-> "My Bk (A).ep"
   *
   * Pure string logic (defined inline, no Arduino/HAL deps) so host tests can
   * use it.
   *
   * @param basename A filename with no path component.
   * @return Normalized filename.
   */
  static inline std::string stripDeviceTag(const std::string& basename) {
    std::string s = basename;

    // --- Leading "(X<digits>) " prefix ---
    if (s.size() >= 2 && s[0] == '(' && s[1] == 'X') {
      size_t j = 2;
      while (j < s.size() && s[j] >= '0' && s[j] <= '9') ++j;
      if (j > 2 && j + 1 < s.size() && s[j] == ')' && s[j + 1] == ' ') {
        s.erase(0, j + 2);  // drop "(Xd) "
      }
    }

    // --- Trailing " (X<digits>)" immediately before the final extension ---
    const size_t dot = s.rfind('.');
    const size_t stemEnd = (dot == std::string::npos) ? s.size() : dot;
    size_t p = stemEnd;
    bool match = (p > 0 && s[p - 1] == ')');
    if (match) {
      --p;  // consumed ')'
      size_t digits = 0;
      while (p > 0 && s[p - 1] >= '0' && s[p - 1] <= '9') {
        --p;
        ++digits;
      }
      match = digits > 0 && p > 0 && s[p - 1] == 'X';
      if (match) {
        --p;  // consumed 'X'
        match = p > 0 && s[p - 1] == '(';
      }
      if (match) {
        --p;  // consumed '('
        match = p > 0 && s[p - 1] == ' ';
      }
      if (match) {
        --p;                                     // consumed ' '
        s = s.substr(0, p) + s.substr(stemEnd);  // drop " (Xd)", keep extension
      }
    }

    return s;
  }

 private:
  // Size of each chunk to read at each offset
  static constexpr size_t CHUNK_SIZE = 1024;

  // Number of offsets to try (i = -1 to 10, so 12 offsets)
  static constexpr int OFFSET_COUNT = 12;

  // Calculate offset for index i: 1024 << (2*i)
  static size_t getOffset(int i);
};
