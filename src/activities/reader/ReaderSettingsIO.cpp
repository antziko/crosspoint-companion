#include "ReaderSettingsIO.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

namespace {

// On-disk layout version for reader_settings.bin. Bump when the field layout below
// changes. A bump that only appends a trailing field can stay readable by widening
// READER_SETTINGS_MIN_READABLE_VERSION's range; any other change must reject older
// files so they re-seed instead of being misread.
// v4: minSessionMinutes repurposed from a minutes value to a MIN_SESSION_SECONDS index;
//     bump rejects old per-book overrides so they cleanly re-seed instead of misreading.
// v5: appended per-book screenMargin. Old (v4) files are rejected and re-seed from the
//     current globals (screenMargin included), so the added field can't be misread.
// v6: appended swapWordSelectAxes. Read-compatible with v5 -- the field is the last one
//     on disk and defaults to 0, so a v5 file is loaded as-is rather than being rejected
//     and re-seeded, which would throw away the book's font/margin choices.
constexpr uint8_t READER_SETTINGS_FILE_VERSION = 6;
// Oldest layout load() still understands; everything from here up is read field by field.
constexpr uint8_t READER_SETTINGS_MIN_READABLE_VERSION = 5;

// Relative path inside the epub cache dir (epub_<hash>/).
constexpr char READER_SETTINGS_FILENAME[] = "/reader_settings.bin";

}  // namespace

namespace ReaderSettingsIO {

bool load(const std::string& cachePath, CrossPointSettings::ReaderOverride& out) {
  HalFile f;
  if (!Storage.openFileForRead("ERS", cachePath + READER_SETTINGS_FILENAME, f)) {
    return false;
  }
  uint8_t version;
  serialization::readPod(f, version);
  if (version < READER_SETTINGS_MIN_READABLE_VERSION || version > READER_SETTINGS_FILE_VERSION) {
    LOG_ERR("ERS", "reader_settings.bin: unknown version %u", version);
    return false;
  }
  serialization::readPod(f, out.fontFamily);
  serialization::readPod(f, out.fontPointSize);
  // Reader size is a point size since 1.5. A per-book file written by 1.4 holds the
  // old 0..3 SMALL/MEDIUM/LARGE/EXTRA_LARGE slot; that range renders at no font, so
  // it is unambiguous — fold it up to the point size it meant (12,14,16,18).
  if (out.fontPointSize <= CrossPointSettings::LEGACY_FONT_SIZE_MAX) {
    out.fontPointSize = 12 + out.fontPointSize * 2;
  }
  serialization::readPod(f, out.lineSpacing);
  serialization::readPod(f, out.paragraphAlignment);
  serialization::readPod(f, out.hyphenationEnabled);
  serialization::readPod(f, out.extraParagraphSpacing);
  f.read(reinterpret_cast<uint8_t*>(out.sdFontFamilyName), sizeof(out.sdFontFamilyName));
  out.sdFontFamilyName[sizeof(out.sdFontFamilyName) - 1] = '\0';
  serialization::readPod(f, out.minSessionMinutes);
  serialization::readPod(f, out.screenMargin);
  // Absent before v6; assign explicitly so a reused `out` cannot carry a stale value.
  out.swapWordSelectAxes = 0;
  if (version >= 6) {
    serialization::readPod(f, out.swapWordSelectAxes);
  }
  out.active = true;
  return true;
}

bool write(const std::string& cachePath, const CrossPointSettings::ReaderOverride& ov) {
  HalFile f;
  if (!Storage.openFileForWrite("ERS", cachePath + READER_SETTINGS_FILENAME, f)) {
    LOG_ERR("ERS", "Failed to open reader_settings.bin for write");
    return false;
  }
  serialization::writePod(f, READER_SETTINGS_FILE_VERSION);
  serialization::writePod(f, ov.fontFamily);
  serialization::writePod(f, ov.fontPointSize);
  serialization::writePod(f, ov.lineSpacing);
  serialization::writePod(f, ov.paragraphAlignment);
  serialization::writePod(f, ov.hyphenationEnabled);
  serialization::writePod(f, ov.extraParagraphSpacing);
  f.write(reinterpret_cast<const uint8_t*>(ov.sdFontFamilyName), sizeof(ov.sdFontFamilyName));
  serialization::writePod(f, ov.minSessionMinutes);
  serialization::writePod(f, ov.screenMargin);
  serialization::writePod(f, ov.swapWordSelectAxes);
  return true;
}

}  // namespace ReaderSettingsIO
