#include "HalStorage.h"

#include <FS.h>  // need to be included before SdFat.h for compatibility with FS.h's File class
#include <Logging.h>
#include <Memory.h>
#include <SDCardManager.h>
#include <esp_heap_caps.h>
#if FREEINK_CAP_USB_MSC
#include <UsbMassStorage.h>
#endif

#include <cassert>
#include <cstring>

#define SDCard SDCardManager::getInstance()

namespace {
#if FREEINK_CAP_USB_MSC
freeink::UsbMassStorage usbMassStorage;
#endif
}  // namespace

HalStorage HalStorage::instance;

HalStorage::HalStorage() {
  // Recursive so the same task can re-enter StorageLock without self-deadlock.
  // openFileForRead/Write take the lock and then assign to a HalFile&
  // out-param; if that out-param already held an Impl, its destructor takes
  // the lock again to close the prior FsFile under serialization (see
  // HalFile::Impl::~Impl below). Priority inheritance still applies to
  // recursive mutexes.
  storageMutex = xSemaphoreCreateRecursiveMutex();
  assert(storageMutex != nullptr);
}

// begin() and ready() are only called from setup, no need to acquire mutex for them

bool HalStorage::begin() { return SDCard.begin(); }

bool HalStorage::ready() const { return SDCard.ready(); }

// For the rest of the methods, we acquire the mutex to ensure thread safety

class HalStorage::StorageLock {
 public:
  StorageLock() { xSemaphoreTakeRecursive(HalStorage::getInstance().storageMutex, portMAX_DELAY); }
  ~StorageLock() { xSemaphoreGiveRecursive(HalStorage::getInstance().storageMutex); }
};

namespace {
// SD write tripwire state (see HalStorage::guardTripped). The ring holds the most recent
// write-type operations so the report names what ran just before the damage appeared.
struct GuardOp {
  uint32_t ms;
  char op[8];
  char task[8];
  char path[40];
};
constexpr int GUARD_RING = 16;
GuardOp guardRing[GUARD_RING] = {};
int guardRingNext = 0;
constexpr unsigned long GUARD_CHECK_INTERVAL_MS = 1000;
constexpr int GUARD_ROOT_ENTRIES = 64;
constexpr int GUARD_TRIP_BAD_ENTRIES = 4;
constexpr char GUARD_REPORT_PATH[] = "/.crosspoint/sd_guard.txt";

// A FAT directory entry as SdFat or any FAT driver would have written it. Deleted (0xE5)
// entries keep stale bytes and are accepted as-is.
bool guardEntryValid(const uint8_t* e) {
  if (e[0] == 0xE5) return true;
  if (e[11] == 0x0F) {  // long-name fragment
    const uint8_t ord = e[0] & 0x1F;
    return ord >= 1 && ord <= 20 && e[12] == 0 && e[26] == 0 && e[27] == 0;
  }
  if (e[11] & 0xC0) return false;
  static constexpr char ILLEGAL[] = "\"*+,/:;<=>?[\\]|";
  for (int i = 0; i < 11; i++) {
    const uint8_t c = e[i];
    if (i == 0 && c == 0x05) continue;  // escaped 0xE5 first byte
    if (c < 0x20 || (c < 0x80 && strchr(ILLEGAL, static_cast<char>(c)) != nullptr)) return false;
  }
  return true;
}
}  // namespace

void HalStorage::guardNoteWrite(const char* op, const char* path) {
  GuardOp& g = guardRing[guardRingNext];
  guardRingNext = (guardRingNext + 1) % GUARD_RING;
  g.ms = millis();
  snprintf(g.op, sizeof(g.op), "%s", op);
  snprintf(g.task, sizeof(g.task), "%s", pcTaskGetName(nullptr));
  // Keep the tail of long paths: the file name is the useful part.
  const size_t len = path ? strlen(path) : 0;
  snprintf(g.path, sizeof(g.path), "%s",
           len >= sizeof(g.path) ? path + len - (sizeof(g.path) - 1) : (path ? path : ""));
  if (guardTripped_ || guardDisabled_ || millis() - guardLastCheckMs_ < GUARD_CHECK_INTERVAL_MS) return;
  guardLastCheckMs_ = millis();
  guardCheckRoot(op);
}

void HalStorage::guardCheckRoot(const char* trigger) {
  // exFAT root entries follow a different format; the check is FAT12/16/32 only.
  if (guardIsExFat()) {
    guardDisabled_ = true;
    return;
  }
  // Static, not stack: this runs on whichever task wrote, some with 4 KB stacks. StorageLock
  // (held by every caller) serialises use.
  static FsFile dir;
  static uint8_t e[32];
  static uint8_t firstBad[32];
  dir = SDCard.open("/", O_RDONLY);
  if (!dir) {
    LOG_ERR("SDGUARD", "root unreadable; check skipped");
    return;
  }
  int badCount = 0;
  int firstBadIndex = -1;
  for (int i = 0; i < GUARD_ROOT_ENTRIES; i++) {
    if (dir.read(e, sizeof(e)) != static_cast<int>(sizeof(e))) break;
    if (e[0] == 0x00) break;  // end of directory
    if (guardEntryValid(e)) continue;
    if (badCount++ == 0) {
      firstBadIndex = i;
      memcpy(firstBad, e, sizeof(firstBad));
    }
  }
  dir.close();
  // Both overwrites seen so far left 12-14 consecutive invalid entries. One odd entry is more
  // likely a legal name this validator does not know, and is only logged.
  if (badCount == 0) return;
  if (badCount < GUARD_TRIP_BAD_ENTRIES) {
    if (!guardWarned_) {
      guardWarned_ = true;
      LOG_ERR("SDGUARD", "root entry %d looks invalid (%d bad); not tripping", firstBadIndex, badCount);
    }
    return;
  }
  LOG_ERR("SDGUARD", "%d invalid root entries from %d after %s; SD writes stopped", badCount, firstBadIndex, trigger);
  guardWriteReport(trigger, firstBadIndex, badCount, firstBad);
  guardTripped_ = true;
}

// Written while writes are still allowed, straight through SdFat (the HAL write paths would
// re-enter the guard).
void HalStorage::guardWriteReport(const char* trigger, const int firstBadIndex, const int badCount,
                                  const uint8_t* entry) {
  static FsFile rep;
  static char line[160];
  rep = SDCard.open(GUARD_REPORT_PATH, O_RDWR | O_CREAT | O_APPEND);
  if (!rep) return;
  int n = snprintf(line, sizeof(line),
                   "[%lu] TRIPPED %d invalid root entries from %d, check after '%s' (task %s)\nentry:", millis(),
                   badCount, firstBadIndex, trigger, pcTaskGetName(nullptr));
  rep.write(line, n);
  for (int b = 0; b < 32; b++) {
    n = snprintf(line, sizeof(line), " %02x", entry[b]);
    rep.write(line, n);
  }
  rep.write("\nrecent writes (oldest first):\n", 32);
  for (int k = 0; k < GUARD_RING; k++) {
    const GuardOp& g = guardRing[(guardRingNext + k) % GUARD_RING];
    if (g.ms == 0) continue;
    n = snprintf(line, sizeof(line), "  [%lu] %-7s %-7s %s\n", static_cast<unsigned long>(g.ms), g.task, g.op, g.path);
    rep.write(line, n);
  }
  rep.close();
}

// Sector 0 is either the volume boot record ("EXFAT   " at offset 3) or an MBR whose first
// partition is type 0x07 (exFAT/NTFS). Read once; the card does not change format mid-session.
bool HalStorage::guardIsExFat() {
  static int8_t cached = -1;
  if (cached >= 0) return cached == 1;
  auto* dev = SDCard.rawBlockDevice();
  static uint8_t sector[512];
  if (dev == nullptr || !dev->readSector(0, sector)) return false;
  const bool vbr = memcmp(sector + 3, "EXFAT   ", 8) == 0;
  const bool mbr = sector[510] == 0x55 && sector[511] == 0xAA && sector[0x1C2] == 0x07;
  cached = (vbr || mbr) ? 1 : 0;
  return cached == 1;
}

bool HalStorage::guardRefuse(const char* op, const char* path) const {
  if (!guardTripped_) return false;
  LOG_ERR("SDGUARD", "refused %s %s (SD writes stopped)", op, path ? path : "");
  return true;
}

bool HalStorage::remount() {
  StorageLock lock;
  const bool mounted = SDCard.remount();
  if (mounted) {
    LOG_INF("SD", "Card remounted");
  } else {
    LOG_ERR("SD", "Card remount failed");
  }
  return mounted;
}

#if FREEINK_CAP_USB_MSC && !FREEINK_SD_SDMMC
#error "USB Drive requires an SDMMC-backed storage profile"
#endif

bool HalStorage::beginUsbDrive() {
#if FREEINK_CAP_USB_MSC
  StorageLock lock;
  auto* const blockDevice = SDCard.detachFilesystemForRawAccess();
  if (!blockDevice) {
    LOG_ERR("USB", "USB Drive requires a mounted SDMMC filesystem");
    return false;
  }

  if (!usbMassStorage.begin(blockDevice)) {
    LOG_ERR("USB", "USB Drive MSC initialization failed");
    if (!SDCard.begin()) {
      LOG_ERR("USB", "Unable to remount SD card after USB Drive startup failure");
    }
    return false;
  }
  return true;
#else
  return false;
#endif
}

bool HalStorage::disconnectUsbDriveHost() {
#if FREEINK_CAP_USB_MSC
  StorageLock lock;
  return usbMassStorage.disconnectHost();
#else
  return false;
#endif
}

void HalStorage::endUsbDrive() {
#if FREEINK_CAP_USB_MSC
  StorageLock lock;
  usbMassStorage.end();
#endif
}

UsbDriveState HalStorage::usbDriveState() const {
#if FREEINK_CAP_USB_MSC
  StorageLock lock;
  switch (usbMassStorage.state()) {
    case freeink::UsbMassStorageState::WaitingForHost:
      return UsbDriveState::WaitingForHost;
    case freeink::UsbMassStorageState::Connected:
    case freeink::UsbMassStorageState::Accessed:
      return UsbDriveState::Connected;
    case freeink::UsbMassStorageState::Ejected:
      return UsbDriveState::Ejected;
    case freeink::UsbMassStorageState::Disconnected:
      return UsbDriveState::Disconnected;
    case freeink::UsbMassStorageState::IoError:
      return UsbDriveState::IoError;
    case freeink::UsbMassStorageState::Idle:
      break;
  }
#endif
  return UsbDriveState::Unsupported;
}

void HalStorage::prepareForDeepSleep() {
  StorageLock lock;
  SDCard.shutdown();
}

#define HAL_STORAGE_WRAPPED_CALL(method, ...) \
  HalStorage::StorageLock lock;               \
  return SDCard.method(__VA_ARGS__);

std::vector<String> HalStorage::listFiles(const char* path, int maxFiles) {
  HAL_STORAGE_WRAPPED_CALL(listFiles, path, maxFiles);
}

String HalStorage::readFile(const char* path) { HAL_STORAGE_WRAPPED_CALL(readFile, path); }

bool HalStorage::readFileToStream(const char* path, Print& out, size_t chunkSize) {
  HAL_STORAGE_WRAPPED_CALL(readFileToStream, path, out, chunkSize);
}

size_t HalStorage::readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes) {
  HAL_STORAGE_WRAPPED_CALL(readFileToBuffer, path, buffer, bufferSize, maxBytes);
}

// Composed of already-locked HalStorage/HalFile operations; needs no
// StorageLock of its own.
bool HalStorage::readFileToString(const char* moduleName, const std::string& path, size_t cap, std::string& out) {
  out.clear();
  HalFile file;
  if (!openFileForRead(moduleName, path, file)) return false;
  if (file.isDirectory()) return false;
  const size_t size = file.fileSize();
  if (size == 0 || size > cap) return false;
  // string growth is a bare allocation under -fno-exceptions; probe first so
  // a large file on a fragmented heap fails soft instead of abort()ing.
  if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < size + 512) {
    LOG_ERR(moduleName, "readFileToString OOM: %u bytes for %s", static_cast<unsigned>(size), path.c_str());
    return false;
  }
  out.resize(size);
  return file.read(out.data(), size) == static_cast<int>(size);
}

bool HalStorage::writeFile(const char* path, const String& content) {
  StorageLock lock;
  if (guardRefuse("write", path)) return false;
  const bool ok = SDCard.writeFile(path, content);
  guardNoteWrite("write", path);
  return ok;
}

bool HalStorage::ensureDirectoryExists(const char* path) {
  StorageLock lock;
  if (SDCard.exists(path)) return true;
  if (guardRefuse("mkdir", path)) return false;
  const bool ok = SDCard.ensureDirectoryExists(path);
  guardNoteWrite("mkdir", path);
  return ok;
}

class HalFile::Impl {
 public:
  Impl(FsFile&& fsFile) : file(std::move(fsFile)) {}
  // SdFat is not thread-safe; FsFile::close() touches SD/SPI and must run
  // under StorageLock or it races SdSpiCard::m_spiActive across tasks and
  // trips FreeRTOS's xTaskPriorityDisinherit assert. The FsFile member
  // destructor (DESTRUCTOR_CLOSES_FILE=1) will close() again after the lock
  // releases, but close() on an already-closed FsFile is a no-op. See SdFat
  // issue #518 and the HAL note in CLAUDE.md.
  ~Impl() {
    HalStorage::StorageLock lock;
    file.close();
  }
  FsFile file;
};

HalFile::HalFile() = default;
HalFile::HalFile(std::unique_ptr<Impl> impl) : impl(std::move(impl)) {}
HalFile::~HalFile() = default;
HalFile::HalFile(HalFile&&) = default;
HalFile& HalFile::operator=(HalFile&&) = default;

HalFile HalStorage::open(const char* path, const oflag_t oflag) {
  StorageLock lock;  // ensure thread safety for the duration of this function
  const bool writes = (oflag & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND)) != 0;
  if (writes && guardRefuse("open", path)) return HalFile();
  HalFile f(makeUniqueNoThrow<HalFile::Impl>(SDCard.open(path, oflag)));
  if (writes) guardNoteWrite("open", path);
  return f;
}

bool HalStorage::mkdir(const char* path, const bool pFlag) {
  StorageLock lock;
  if (guardRefuse("mkdir", path)) return false;
  const bool ok = SDCard.mkdir(path, pFlag);
  guardNoteWrite("mkdir", path);
  return ok;
}

bool HalStorage::exists(const char* path) { HAL_STORAGE_WRAPPED_CALL(exists, path); }

bool HalStorage::remove(const char* path) {
  StorageLock lock;
  if (guardRefuse("remove", path)) return false;
  const bool ok = SDCard.remove(path);
  guardNoteWrite("remove", path);
  return ok;
}
bool HalStorage::rename(const char* oldPath, const char* newPath) {
  StorageLock lock;
  if (guardRefuse("rename", newPath)) return false;
  const bool ok = SDCard.rename(oldPath, newPath);
  guardNoteWrite("rename", newPath);
  return ok;
}

bool HalStorage::replaceFile(const char* tmpPath, const char* path) {
  StorageLock lock;
  if (guardRefuse("replace", path)) return false;
  const bool ok = SDCard.replaceFile(tmpPath, path);
  guardNoteWrite("replace", path);
  return ok;
}

bool HalStorage::rmdir(const char* path) {
  StorageLock lock;
  if (guardRefuse("rmdir", path)) return false;
  const bool ok = SDCard.rmdir(path);
  guardNoteWrite("rmdir", path);
  return ok;
}

bool HalStorage::openFileForRead(const char* moduleName, const char* path, HalFile& file) {
  StorageLock lock;  // ensure thread safety for the duration of this function
  FsFile fsFile;
  bool ok = SDCard.openFileForRead(moduleName, path, fsFile);
  // A failed Impl allocation must read as a failed open: the HalFile would otherwise be
  // returned with a null impl (isOpen() == false) while `ok` still said the open succeeded.
  auto impl = makeUniqueNoThrow<HalFile::Impl>(std::move(fsFile));
  if (!impl) {
    LOG_ERR("HalStorage", "OOM allocating file handle for %s", path);
    ok = false;
  }
  file = HalFile(std::move(impl));
  return ok;
}

bool HalStorage::openFileForRead(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForRead(const char* moduleName, const String& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const char* path, HalFile& file) {
  StorageLock lock;  // ensure thread safety for the duration of this function
  if (guardRefuse("openW", path)) {
    file = HalFile();
    return false;
  }
  guardNoteWrite("openW", path);
  FsFile fsFile;
  bool ok = SDCard.openFileForWrite(moduleName, path, fsFile);
  // A failed Impl allocation must read as a failed open: the HalFile would otherwise be
  // returned with a null impl (isOpen() == false) while `ok` still said the open succeeded.
  auto impl = makeUniqueNoThrow<HalFile::Impl>(std::move(fsFile));
  if (!impl) {
    LOG_ERR("HalStorage", "OOM allocating file handle for %s", path);
    ok = false;
  }
  file = HalFile(std::move(impl));
  return ok;
}

bool HalStorage::openFileForWrite(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForAppend(const char* moduleName, const char* path, HalFile& file) {
  StorageLock lock;  // ensure thread safety for the duration of this function
  if (guardRefuse("openA", path)) {
    file = HalFile();
    return false;
  }
  guardNoteWrite("openA", path);
  // freeink's SDCardManager has no openFileForAppend() (openFileForWrite truncates
  // with O_TRUNC); open with append flags via the generic open() instead.
  FsFile fsFile = SDCard.open(path, O_RDWR | O_CREAT | O_APPEND);
  bool ok = static_cast<bool>(fsFile);
  if (!ok) {
    LOG_ERR(moduleName, "Failed to open file for append: %s", path);
  }
  // See openFileForRead: a null impl must report as a failed open, not a successful one.
  auto impl = makeUniqueNoThrow<HalFile::Impl>(std::move(fsFile));
  if (!impl) {
    LOG_ERR("HalStorage", "OOM allocating file handle for %s", path);
    ok = false;
  }
  file = HalFile(std::move(impl));
  return ok;
}

bool HalStorage::openFileForWrite(const char* moduleName, const String& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::removeDir(const char* path) {
  StorageLock lock;
  if (guardRefuse("rmtree", path)) return false;
  const bool ok = SDCard.removeDir(path);
  guardNoteWrite("rmtree", path);
  return ok;
}

// HalFile implementation
// Allow doing file operations while ensuring thread safety via HalStorage's mutex.
// Please keep the list below in sync with the HalFile.h header

#define HAL_FILE_WRAPPED_CALL(method, ...) \
  HalStorage::StorageLock lock;            \
  assert(impl != nullptr);                 \
  return impl->file.method(__VA_ARGS__);

#define HAL_FILE_FORWARD_CALL(method, ...) \
  assert(impl != nullptr);                 \
  return impl->file.method(__VA_ARGS__);

// Writes are refused once the SD tripwire has fired (see HalStorage::guardTripped).
#define HAL_FILE_GUARDED_WRITE(fail, method, ...) \
  HalStorage::StorageLock lock;                   \
  assert(impl != nullptr);                        \
  if (Storage.guardTripped()) return fail;        \
  return impl->file.method(__VA_ARGS__);

void HalFile::flush() {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  impl->file.flush();
  Storage.guardNoteWrite("flush", "");
}
size_t HalFile::getName(char* name, size_t len) { HAL_FILE_WRAPPED_CALL(getName, name, len); }
size_t HalFile::size() { HAL_FILE_FORWARD_CALL(size, ); }              // already thread-safe, no need to wrap
size_t HalFile::fileSize() { HAL_FILE_FORWARD_CALL(fileSize, ); }      // already thread-safe, no need to wrap
uint64_t HalFile::fileSize64() { HAL_FILE_FORWARD_CALL(fileSize, ); }  // already thread-safe, no need to wrap
bool HalFile::seek(size_t pos) { HAL_FILE_WRAPPED_CALL(seekSet, pos); }
bool HalFile::seek64(uint64_t pos) { HAL_FILE_WRAPPED_CALL(seekSet, pos); }
bool HalFile::seekCur(int64_t offset) { HAL_FILE_WRAPPED_CALL(seekCur, offset); }
bool HalFile::seekSet(size_t offset) { HAL_FILE_WRAPPED_CALL(seekSet, offset); }
bool HalFile::truncate(const uint64_t length) { HAL_FILE_GUARDED_WRITE(false, truncate, length); }
int HalFile::available() const { HAL_FILE_WRAPPED_CALL(available, ); }
size_t HalFile::position() const { HAL_FILE_WRAPPED_CALL(position, ); }
int HalFile::read(void* buf, size_t count) { HAL_FILE_WRAPPED_CALL(read, buf, count); }
int HalFile::read() { HAL_FILE_WRAPPED_CALL(read, ); }

size_t HalFile::write(const uint8_t* buf, size_t count) { HAL_FILE_GUARDED_WRITE(0, write, buf, count); }
size_t HalFile::write(const void* buf, size_t count) { HAL_FILE_GUARDED_WRITE(0, write, buf, count); }
size_t HalFile::write(uint8_t b) { HAL_FILE_GUARDED_WRITE(0, write, b); }
bool HalFile::rename(const char* newPath) { HAL_FILE_GUARDED_WRITE(false, rename, newPath); }
bool HalFile::isDirectory() const { HAL_FILE_FORWARD_CALL(isDirectory, ); }  // already thread-safe, no need to wrap
void HalFile::rewindDirectory() { HAL_FILE_WRAPPED_CALL(rewindDirectory, ); }
// The one method that tolerates a null impl, deliberately. Every other call above asserts
// because reading or seeking a handle that was never opened is a caller bug with no sane
// answer. "Close something already closed" has one: do nothing. SdFat agrees — FsBaseFile
// ::close() returns true when neither underlying file is open (FsFile.cpp:58-63) — and no
// caller in this repo inspects the return value.
//
// Asserting here cost a shipped reboot: openLookupCtx released three never-opened handles
// and panicked on every dictionary lookup (fixed in 9ff55eff). The assert turned a benign
// no-op into a hard reset in the field, since no environment defines NDEBUG.
//
// Note this only makes close() SAFE, not preferable: it leaves the Impl allocated. To
// actually release a member handle, move-assign a fresh one (`f = HalFile()`), which frees
// the Impl and closes the FsFile via ~Impl under the same lock.
bool HalFile::close() {
  if (!impl) return true;
  HalStorage::StorageLock lock;
  // A close syncs the file's directory entry: the moment a damaged cached directory sector
  // would reach the card, so it is a check point for the tripwire.
  const bool wasWritable = impl->file.isWritable();
  const bool ok = impl->file.close();
  if (wasWritable) Storage.guardNoteWrite("close", "");
  return ok;
}
HalFile HalFile::openNextFile() {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  return HalFile(makeUniqueNoThrow<Impl>(impl->file.openNextFile()));
}
bool HalFile::isOpen() const { return impl != nullptr && impl->file.isOpen(); }  // already thread-safe, no need to wrap
HalFile::operator bool() const { return isOpen(); }
