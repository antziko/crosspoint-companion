#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

// Host stub for LookupHistory tests. FILE*-backed read/write (modeled on the
// reading-time-history stub) plus the remove/rename pair the streaming
// rewrite-to-temp-then-swap pattern needs.
class HalFile {
 public:
  HalFile() = default;
  ~HalFile() { close(); }
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  bool openForRead(const std::string& path) {
    close();
    fp_ = std::fopen(path.c_str(), "rb");
    return fp_ != nullptr;
  }
  bool openForWrite(const std::string& path) {
    close();
    fp_ = std::fopen(path.c_str(), "wb");
    return fp_ != nullptr;
  }
  bool openForAppend(const std::string& path) {
    close();
    fp_ = std::fopen(path.c_str(), "ab");
    return fp_ != nullptr;
  }
  int read() {
    if (!fp_) return -1;
    return std::fgetc(fp_);
  }
  int read(void* buf, size_t n) {
    if (!fp_) return -1;
    return static_cast<int>(std::fread(buf, 1, n, fp_));
  }
  size_t write(const void* buf, size_t n) {
    if (!fp_) return 0;
    return std::fwrite(buf, 1, n, fp_);
  }
  int available() {
    if (!fp_) return 0;
    const long cur = std::ftell(fp_);
    if (cur < 0) return 0;
    std::fseek(fp_, 0, SEEK_END);
    const long end = std::ftell(fp_);
    std::fseek(fp_, cur, SEEK_SET);
    return static_cast<int>(end - cur);
  }
  bool close() {
    if (fp_) {
      std::fclose(fp_);
      fp_ = nullptr;
    }
    return true;
  }

 private:
  std::FILE* fp_ = nullptr;
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }
  bool openFileForRead(const char* /*moduleName*/, const std::string& path, HalFile& file) {
    return file.openForRead(path);
  }
  bool openFileForWrite(const char* /*moduleName*/, const std::string& path, HalFile& file) {
    return file.openForWrite(path);
  }
  bool openFileForAppend(const char* /*moduleName*/, const std::string& path, HalFile& file) {
    return file.openForAppend(path);
  }
  bool remove(const char* path) { return std::remove(path) == 0; }
  bool rename(const char* oldPath, const char* newPath) { return std::rename(oldPath, newPath) == 0; }
};

#define Storage HalStorage::getInstance()
