#include "PersistableStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace {

// Streams serializeJson() straight to an open file instead of materialising the whole
// document in a String first.
//
// Two reasons it has to buffer rather than handing HalFile to serializeJson directly:
//   1. HalFile overrides only write(uint8_t). Arduino's Print::write(const uint8_t*, size_t)
//      default loops over that, and every HalFile call takes storageMutex — so a ~2 KB
//      settings document would mean ~2000 mutex takes and 2000 single-byte SdFat writes.
//   2. The String it replaces was one contiguous ~2 KB allocation grown by realloc, on the
//      same save path that already aborts on OOM (see SettingsPersistence.h). This buffer is
//      a fixed 128 bytes on the caller's stack — no heap at all.
class BufferedFileWriter : public Print {
 public:
  explicit BufferedFileWriter(HalFile& file) : file_(file) {}

  size_t write(const uint8_t b) override { return write(&b, 1); }

  size_t write(const uint8_t* data, size_t size) override {
    const size_t total = size;
    while (size > 0) {
      if (used_ == sizeof(buf_) && !flushBuffer()) return total - size;
      const size_t chunk = std::min(size, sizeof(buf_) - used_);
      memcpy(buf_ + used_, data, chunk);
      used_ += chunk;
      data += chunk;
      size -= chunk;
    }
    return total;
  }

  // Writes out whatever is still buffered. Must be called before the file closes.
  bool finish() { return ok_ && flushBuffer(); }

 private:
  bool flushBuffer() {
    if (used_ == 0) return ok_;
    if (file_.write(buf_, used_) != used_) {
      ok_ = false;
      return false;
    }
    used_ = 0;
    return true;
  }

  HalFile& file_;
  uint8_t buf_[128];
  size_t used_ = 0;
  bool ok_ = true;
};

}  // namespace

bool PersistableStoreBase::writeDocToFile(const char* path, const JsonDocument& doc) {
  Storage.mkdir("/.crosspoint");

  HalFile file;
  // openFileForWrite passes O_TRUNC, so an existing shorter document cannot leave a tail
  // of the previous one behind.
  if (!Storage.openFileForWrite("PERSIST", path, file)) {
    LOG_ERR("PERSIST", "Failed to open %s for write", path);
    return false;
  }

  BufferedFileWriter writer(file);
  serializeJson(doc, writer);
  if (!writer.finish()) {
    LOG_ERR("PERSIST", "Failed to write %s", path);
    return false;
  }
  file.flush();
  return true;
}

bool PersistableStoreBase::readDocFromFile(const char* path, JsonDocument& doc) {
  if (!Storage.exists(path)) {
    return false;  // Expected on first boot — not an error.
  }
  String json = Storage.readFile(path);
  if (json.isEmpty()) {
    LOG_ERR("PERSIST", "Failed to read %s (empty)", path);
    return false;
  }
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("PERSIST", "JSON parse error in %s: %s", path, error.c_str());
    return false;
  }
  return true;
}

std::string PersistableStoreBase::extractPassword(JsonVariantConst doc, bool& needsResave) {
  bool valid = false;
  return extractPassword(doc, needsResave, std::numeric_limits<size_t>::max(), valid);
}

std::string PersistableStoreBase::extractPassword(JsonVariantConst doc, bool& needsResave, const size_t maxLength,
                                                  bool& valid) {
  valid = true;
  bool ok = false;
  bool tooLong = false;
  std::string pass = obfuscation::deobfuscateFromBase64(doc["password_obf"] | "", maxLength, &ok, &tooLong);
  if (tooLong) {
    valid = false;
    return "";
  }
  if (!ok) {
    // Deobfuscation failed — fall back to legacy plaintext password.
    const char* legacyPassword = doc["password"] | "";
    const size_t legacyLength = strlen(legacyPassword);
    if (legacyLength > maxLength) {
      valid = false;
      return "";
    }
    pass.assign(legacyPassword, legacyLength);
    if (!pass.empty()) needsResave = true;
  }
  // A successfully decoded empty string is a legitimate value; preserve as-is.
  return pass;
}
