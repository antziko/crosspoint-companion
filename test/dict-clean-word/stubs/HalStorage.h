#pragma once
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

// Host stub for the Dictionary::cleanWord tests. Adapted from the BookmarkStore
// stub (test/bookmark-store/stubs/HalStorage.h) with the extra HalFile surface
// Dictionary.cpp needs to compile: fileSize/position/seekSet.
//
// Real-file backed, but every device path is redirected under a per-test root
// directory (set via HalStorage::setRoot) so absolute "/.crosspoint/..." paths
// land in a temp dir. cleanWord itself touches no storage — this exists purely
// so the translation unit links.

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
  int read(void* buf, size_t n) {
    if (!fp_) return -1;
    return static_cast<int>(std::fread(buf, 1, n, fp_));
  }
  // Single-byte read; -1 at EOF (matches the device HalFile).
  int read() {
    if (!fp_) return -1;
    const int c = std::fgetc(fp_);
    return c == EOF ? -1 : c;
  }
  size_t write(const void* buf, size_t n) {
    if (!fp_) return 0;
    return std::fwrite(buf, 1, n, fp_);
  }
  size_t write(uint8_t b) { return write(&b, 1); }
  bool seekCur(int64_t offset) {
    if (!fp_) return false;
    return std::fseek(fp_, static_cast<long>(offset), SEEK_CUR) == 0;
  }
  bool seekSet(uint64_t offset) {
    if (!fp_) return false;
    return std::fseek(fp_, static_cast<long>(offset), SEEK_SET) == 0;
  }
  uint64_t position() {
    if (!fp_) return 0;
    const long cur = std::ftell(fp_);
    return cur < 0 ? 0 : static_cast<uint64_t>(cur);
  }
  uint64_t fileSize() {
    if (!fp_) return 0;
    const long cur = std::ftell(fp_);
    std::fseek(fp_, 0, SEEK_END);
    const long end = std::ftell(fp_);
    std::fseek(fp_, cur, SEEK_SET);
    return end < 0 ? 0 : static_cast<uint64_t>(end);
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

  void setRoot(const std::string& root) { root_ = root; }

  bool openFileForRead(const char*, const std::string& path, HalFile& file) {
    return file.openForRead(translate(path));
  }
  bool openFileForWrite(const char*, const std::string& path, HalFile& file) {
    ensureParent(translate(path));
    return file.openForWrite(translate(path));
  }
  bool openFileForAppend(const char*, const char* path, HalFile& file) {
    ensureParent(translate(path));
    return file.openForAppend(translate(path));
  }

  bool exists(const char* path) { return std::filesystem::exists(translate(path)); }
  bool remove(const char* path) {
    std::error_code ec;
    return std::filesystem::remove(translate(path), ec);
  }
  bool rename(const char* src, const char* dst) {
    std::error_code ec;
    ensureParent(translate(dst));
    std::filesystem::rename(translate(src), translate(dst), ec);
    return !ec;
  }
  bool mkdir(const char* path) {
    std::error_code ec;
    std::filesystem::create_directories(translate(path), ec);
    return !ec;
  }
  std::vector<std::string> listFiles(const char* path) {
    std::vector<std::string> out;
    std::error_code ec;
    const std::string dir = translate(path);
    if (!std::filesystem::exists(dir, ec)) return out;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
      out.push_back(e.path().filename().string());
    }
    return out;
  }

 private:
  std::string translate(const std::string& path) const {
    if (root_.empty()) return path;
    // Device paths are absolute ("/.crosspoint/..."); splice them under the root.
    return root_ + path;
  }
  void ensureParent(const std::string& fullPath) const {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(fullPath).parent_path(), ec);
  }
  std::string root_;
};

#define Storage HalStorage::getInstance()
