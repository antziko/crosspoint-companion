#include "ReaderSettingsIO.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

namespace {

// On-disk layout version for reader_settings.bin. Bump if the field layout below
// changes so older files are rejected and re-seeded.
constexpr uint8_t READER_SETTINGS_FILE_VERSION = 1;

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
  if (version != READER_SETTINGS_FILE_VERSION) {
    LOG_ERR("ERS", "reader_settings.bin: unknown version %u", version);
    return false;
  }
  serialization::readPod(f, out.fontFamily);
  serialization::readPod(f, out.fontSize);
  serialization::readPod(f, out.lineSpacing);
  serialization::readPod(f, out.paragraphAlignment);
  serialization::readPod(f, out.hyphenationEnabled);
  serialization::readPod(f, out.extraParagraphSpacing);
  f.read(reinterpret_cast<uint8_t*>(out.sdFontFamilyName), sizeof(out.sdFontFamilyName));
  out.sdFontFamilyName[sizeof(out.sdFontFamilyName) - 1] = '\0';
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
  serialization::writePod(f, ov.fontSize);
  serialization::writePod(f, ov.lineSpacing);
  serialization::writePod(f, ov.paragraphAlignment);
  serialization::writePod(f, ov.hyphenationEnabled);
  serialization::writePod(f, ov.extraParagraphSpacing);
  f.write(reinterpret_cast<const uint8_t*>(ov.sdFontFamilyName), sizeof(ov.sdFontFamilyName));
  return true;
}

}  // namespace ReaderSettingsIO
