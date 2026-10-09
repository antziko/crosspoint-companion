#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

// Host stub for RecentBooksStore tests: device paths map under root(), and
// files are plain FILE*s.
class HalFile {
 public:
  HalFile() = default;
  ~HalFile() { close(); }
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  bool open(const std::string& path, const char* mode) {
    close();
    fp_ = std::fopen(path.c_str(), mode);
    return fp_ != nullptr;
  }
  int read(void* buf, size_t n) { return fp_ ? static_cast<int>(std::fread(buf, 1, n, fp_)) : -1; }
  size_t write(const void* buf, size_t n) { return fp_ ? std::fwrite(buf, 1, n, fp_) : 0; }
  bool seek(size_t pos) { return fp_ && std::fseek(fp_, static_cast<long>(pos), SEEK_SET) == 0; }
  void flush() {
    if (fp_) std::fflush(fp_);
  }
  void close() {
    if (fp_) std::fclose(fp_);
    fp_ = nullptr;
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
  static std::string& root() {
    static std::string dir;
    return dir;
  }
  static std::string real(const std::string& path) { return root() + path; }

  bool exists(const char* path) {
    std::FILE* fp = std::fopen(real(path).c_str(), "rb");
    if (fp) std::fclose(fp);
    return fp != nullptr;
  }
  bool openFileForRead(const char*, const std::string& path, HalFile& file) { return file.open(real(path), "rb"); }
  bool openFileForWrite(const char*, const std::string& path, HalFile& file) { return file.open(real(path), "wb"); }
  bool remove(const char* path) { return std::remove(real(path).c_str()) == 0; }
  bool replaceFile(const char* tmpPath, const char* path) {
    std::remove(real(path).c_str());
    return std::rename(real(tmpPath).c_str(), real(path).c_str()) == 0;
  }
  bool readFileToString(const char*, const std::string& path, size_t cap, std::string& out) {
    out.clear();
    std::FILE* fp = std::fopen(real(path).c_str(), "rb");
    if (!fp) return false;
    char buf[256];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0 && out.size() < cap) out.append(buf, n);
    std::fclose(fp);
    return out.size() <= cap;
  }
};

#define Storage HalStorage::getInstance()
