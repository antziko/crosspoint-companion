#pragma once
#include <algorithm>
#include <string>
#include <utility>

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
   * Strip the auto-epub-optimizer device tag so optimized copies hash to the same
   * key as the original (and across X3/X4 devices). Restricted to the existing
   * hardware device set — only "(X3)" and "(X4)" are recognized, so real titles
   * that happen to end in "(X<n>)" (e.g. "Mac OS X (X11)") are NOT false-stripped.
   * Removes the tag in either placement:
   *   - leading prefix  "(X3) "/"(X4) " at the very start of the name, or
   *   - trailing suffix " (X3)"/" (X4)" immediately before the final extension.
   * Both require the separating space; either or both may be present.
   *
   *   "(X4) Book.epub"  -> "Book.epub"      "Book (X4).epub"   -> "Book.epub"
   *   "(X3) Book.epub"  -> "Book.epub"      "Book.epub"        -> "Book.epub"
   *   "(X12) Book.epub" -> unchanged        "Book (X11).epub"  -> unchanged (not X3/X4)
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

    // --- Leading "(X3) "/"(X4) " prefix (exact 5 chars) ---
    if (s.size() >= 5 && s[0] == '(' && s[1] == 'X' && (s[2] == '3' || s[2] == '4') &&
        s[3] == ')' && s[4] == ' ') {
      s.erase(0, 5);  // drop "(Xn) "
    }

    // --- Trailing " (X3)"/" (X4)" (exact 5 chars) before the final extension ---
    const size_t dot = s.rfind('.');
    const size_t stemEnd = (dot == std::string::npos) ? s.size() : dot;
    if (stemEnd >= 5) {
      const size_t p = stemEnd - 5;
      if (s[p] == ' ' && s[p + 1] == '(' && s[p + 2] == 'X' &&
          (s[p + 3] == '3' || s[p + 3] == '4') && s[p + 4] == ')') {
        s = s.substr(0, p) + s.substr(stemEnd);  // drop " (Xn)", keep extension
      }
    }

    return s;
  }

  /**
   * Canonicalize "{a} - {b}" / "{b} - {a}" filenames to one ordering so an
   * author/title swap between devices shares a sync key. Operates on the stem
   * only; the extension is preserved. Fires ONLY when the stem contains exactly
   * one " - " separator — multi-dash names (e.g. subtitles "X - Y: A - B") are
   * left untouched to avoid mis-splitting and false convergence.
   *
   *   "Smith - Dune.epub" -> "Dune - Smith.epub"   (both orders -> same output)
   *   "Dune - Smith.epub" -> "Dune - Smith.epub"
   *   "Dune.epub"            -> unchanged (no separator)
   *   "A - B - C.epub"       -> unchanged (>1 separator)
   *
   * Pure string logic (defined inline, no Arduino/HAL deps) so host tests can
   * use it. Apply AFTER stripDeviceTag.
   *
   * @param basename A filename with no path component.
   * @return Normalized filename.
   */
  static inline std::string swapAuthorTitle(const std::string& basename) {
    const size_t dot = basename.rfind('.');
    const std::string stem = (dot == std::string::npos) ? basename : basename.substr(0, dot);
    const std::string ext = (dot == std::string::npos) ? "" : basename.substr(dot);
    const size_t first = stem.find(" - ");
    if (first == std::string::npos) return basename;                       // no separator
    if (stem.find(" - ", first + 3) != std::string::npos) return basename;  // >1 separator
    std::string a = stem.substr(0, first);
    std::string b = stem.substr(first + 3);
    if (a.empty() || b.empty()) return basename;  // degenerate
    if (b < a) std::swap(a, b);                    // canonical (lexicographic) order
    return a + " - " + b + ext;
  }

 private:
  // Size of each chunk to read at each offset
  static constexpr size_t CHUNK_SIZE = 1024;

  // Number of offsets to try (i = -1 to 10, so 12 offsets)
  static constexpr int OFFSET_COUNT = 12;

  // Calculate offset for index i: 1024 << (2*i)
  static size_t getOffset(int i);
};
