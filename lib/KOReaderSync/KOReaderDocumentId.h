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
   * Strip the auto-epub-optimizer tag so optimized copies hash to the same key as
   * the original. Recognized tags: "(x)" or "(X)" (the single optimized copy) and the
   * legacy per-device "(X3)"/"(X4)" (uppercase only). Any other parenthesized
   * group, e.g. "Mac OS X (X11)", is part of the title and kept.
   * Removes the tag in either placement:
   *   - leading prefix  "(x) "/"(X3) " at the very start of the name, or
   *   - trailing suffix " (x)"/" (X3)" immediately before the final extension.
   * Both require the separating space; either or both may be present.
   *
   *   "(x) Book.epub"   -> "Book.epub"      "Book (x).epub"    -> "Book.epub"
   *   "(X4) Book.epub"  -> "Book.epub"      "Book (X3).epub"   -> "Book.epub"
   *   "(X12) Book.epub" -> unchanged        "Book (X11).epub"  -> unchanged
   *   "(x)Book.epub"    -> unchanged        "Book(X4).epub"    -> unchanged (no space)
   *   "(X4) Bk (X4).epub" -> "Bk.epub"      "My Bk (A) (x).ep" -> "My Bk (A).ep"
   *
   * Both sides of a sync must apply exactly this rule: the KOReader plugin copy is
   * koplugin/crosspointsync.koplugin/cp_docid.lua, the server copy is
   * crosspoint-sync src/routes/kosync.ts.
   *
   * Pure string logic (defined inline, no Arduino/HAL deps) so host tests can
   * use it.
   *
   * @param basename A filename with no path component.
   * @return Normalized filename.
   */
  static inline std::string stripDeviceTag(const std::string& basename) {
    std::string s = basename;

    const size_t prefixLen = tagLengthAt(s, 0);
    if (prefixLen > 0 && s.size() > prefixLen && s[prefixLen] == ' ') {
      s.erase(0, prefixLen + 1);
    }

    const size_t dot = s.rfind('.');
    const size_t stemEnd = (dot == std::string::npos) ? s.size() : dot;
    for (const size_t len : {size_t{3}, size_t{4}}) {
      if (stemEnd < len + 1) continue;
      const size_t p = stemEnd - len;
      if (s[p - 1] == ' ' && tagLengthAt(s, p) == len) {
        s = s.substr(0, p - 1) + s.substr(stemEnd);  // drop " (x)", keep extension
        break;
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
    if (first == std::string::npos) return basename;                        // no separator
    if (stem.find(" - ", first + 3) != std::string::npos) return basename;  // >1 separator
    std::string a = stem.substr(0, first);
    std::string b = stem.substr(first + 3);
    if (a.empty() || b.empty()) return basename;  // degenerate
    if (b < a) std::swap(a, b);                   // canonical (lexicographic) order
    return a + " - " + b + ext;
  }

  /**
   * The canonical name a filename-mode sync key is computed from.
   *
   * stripDeviceTag -> swapAuthorTitle, twice. The repeat is not redundant: a tag can sit
   * on the title rather than on the whole name ("Author - (X4) Title.epub"), where it is
   * at neither end of the filename and survives the first strip. Canonicalizing the order
   * moves it to the front, the second strip removes it, and the second swap restores the
   * ordering that removal changed:
   *
   *   "Author - (X4) Title.epub"
   *     -> strip: unchanged        -> swap: "(X4) Title - Author.epub"
   *     -> strip: "Title - Author.epub"  -> swap: canonical order
   *
   * Converges for a tag at the start of either component, and at the end of the name.
   * A tag trailing the FIRST component ("Author (X4) - Title.epub") is still not
   * recognized: it is at neither end and no reordering brings it to one. Stripping inside
   * a component would need a wider tag rule than the deliberately narrow one above, so it
   * is left alone until such a name is actually seen.
   *
   * Both sides of a sync must apply exactly this, or they key the same book differently
   * and the failure is silent. The KOReader plugin's copy is koplugin/
   * crosspointsync.koplugin/cp_docid.lua.
   *
   * @param basename A filename with no path component.
   * @return Normalized filename.
   */
  static inline std::string canonicalFilename(const std::string& basename) {
    return swapAuthorTitle(stripDeviceTag(swapAuthorTitle(stripDeviceTag(basename))));
  }

 private:
  // Length of the tag "(x)"/"(X)" (3) or legacy "(X3)"/"(X4)" (4) starting at pos, else 0.
  // The legacy tags stay uppercase-only so no existing filename changes its sync key.
  static inline size_t tagLengthAt(const std::string& s, size_t pos) {
    if (pos + 3 > s.size() || s[pos] != '(' || (s[pos + 1] != 'X' && s[pos + 1] != 'x')) return 0;
    if (s[pos + 2] == ')') return 3;
    if (s[pos + 1] == 'X' && pos + 4 <= s.size() && (s[pos + 2] == '3' || s[pos + 2] == '4') && s[pos + 3] == ')') {
      return 4;
    }
    return 0;
  }

  // Size of each chunk to read at each offset
  static constexpr size_t CHUNK_SIZE = 1024;

  // Number of offsets to try (i = -1 to 10, so 12 offsets)
  static constexpr int OFFSET_COUNT = 12;

  // Calculate offset for index i: 1024 << (2*i)
  static size_t getOffset(int i);
};
