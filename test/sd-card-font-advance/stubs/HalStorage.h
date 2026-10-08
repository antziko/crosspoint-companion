#pragma once
// Host stub: one in-memory font file (sdFontTestFile) behind the HalFile calls SdCardFont makes.
// Also carries the Arduino ESP/millis globals SdCardFont.cpp reaches through the real HalStorage.h.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

inline std::vector<uint8_t> sdFontTestFile;

struct SdFontTestEsp {
  size_t getFreeHeap() const { return 200 * 1024; }
  size_t getMaxAllocHeap() const { return 200 * 1024; }
};
inline SdFontTestEsp ESP;
inline uint32_t millis() { return 0; }

class HalFile {
 public:
  bool seekSet(size_t position) {
    position_ = position;
    return opened_ && position <= sdFontTestFile.size();
  }
  int read(void* output, size_t count) {
    if (!opened_ || position_ > sdFontTestFile.size()) return 0;
    count = std::min(count, sdFontTestFile.size() - position_);
    std::memcpy(output, sdFontTestFile.data() + position_, count);
    position_ += count;
    return static_cast<int>(count);
  }
  bool isOpen() const { return opened_; }
  void close() { opened_ = false; }
  void open() {
    opened_ = true;
    position_ = 0;
  }

 private:
  size_t position_ = 0;
  bool opened_ = false;
};

class HalStorage {
 public:
  bool openFileForRead(const char*, const char*, HalFile& file) {
    file.open();
    return true;
  }
};
inline HalStorage Storage;
