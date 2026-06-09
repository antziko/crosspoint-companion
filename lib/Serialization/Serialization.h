#pragma once
#include <HalStorage.h>

#include <iostream>

namespace serialization {
template <typename T>
void writePod(std::ostream& os, const T& value) {
  os.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
void writePod(HalFile& file, const T& value) {
  file.write(reinterpret_cast<const uint8_t*>(&value), sizeof(T));
}

template <typename T>
void readPod(std::istream& is, T& value) {
  is.read(reinterpret_cast<char*>(&value), sizeof(T));
}

template <typename T>
void readPod(HalFile& file, T& value) {
  file.read(reinterpret_cast<uint8_t*>(&value), sizeof(T));
}

inline void writeString(std::ostream& os, const std::string& s) {
  const uint32_t len = s.size();
  writePod(os, len);
  os.write(s.data(), len);
}

inline void writeString(HalFile& file, const std::string& s) {
  const uint32_t len = s.size();
  writePod(file, len);
  file.write(reinterpret_cast<const uint8_t*>(s.data()), len);
}

// Returns false (and leaves s empty) if the length prefix is corrupt -- a
// serialized string can never be longer than the bytes remaining in the
// stream. Without this bound, a garbage length aborts the firmware
// (std::length_error / std::bad_alloc -> terminate under -fno-exceptions).
inline bool readString(std::istream& is, std::string& s) {
  uint32_t len;
  readPod(is, len);
  const std::streampos cur = is.tellg();
  is.seekg(0, std::ios::end);
  const std::streampos end = is.tellg();
  is.seekg(cur);
  if (cur < 0 || end < 0 || len > static_cast<uint32_t>(end - cur)) {
    s.clear();
    return false;
  }
  s.resize(len);
  is.read(&s[0], len);
  return true;
}

inline bool readString(HalFile& file, std::string& s) {
  uint32_t len;
  readPod(file, len);
  const int avail = file.available();
  if (avail < 0 || len > static_cast<uint32_t>(avail)) {
    s.clear();
    return false;
  }
  s.resize(len);
  file.read(&s[0], len);
  return true;
}
}  // namespace serialization
