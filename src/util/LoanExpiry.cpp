#include "LoanExpiry.h"

#include <BookKey.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <TrustedTime.h>

#include <string_view>

#include "BookCacheUtils.h"
#include "RecentBooksStore.h"
#include "util/RecentBooksLine.h"

namespace loanexpiry {
namespace {

// The rights file's resource id sits within its first few hundred bytes.
constexpr size_t RIGHTS_SCAN_BYTES = 2048;
constexpr size_t MAX_RESOURCE_LEN = 96;

std::string fileNameOf(const std::string& path) {
  const size_t slash = path.rfind('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// "<resource>urn:uuid:...</resource>" from the book's .rights file: the id of the
// title itself, the same for every loan of it. Empty when missing or unreadable.
std::string rightsResource(const std::string& bookPath) {
  HalFile f;
  if (!Storage.openFileForRead("LOAN", bookPath + ".rights", f)) return {};
  auto buf = makeUniqueNoThrow<char[]>(RIGHTS_SCAN_BYTES);
  if (!buf) {
    LOG_ERR("LOAN", "OOM: %u byte rights buffer", static_cast<unsigned>(RIGHTS_SCAN_BYTES));
    return {};
  }
  const int got = f.read(buf.get(), RIGHTS_SCAN_BYTES);
  if (got <= 0) return {};
  const std::string_view text(buf.get(), static_cast<size_t>(got));
  constexpr std::string_view OPEN = "<resource>";
  const size_t start = text.find(OPEN);
  if (start == std::string_view::npos) return {};
  const size_t from = start + OPEN.size();
  const size_t end = text.find('<', from);
  if (end == std::string_view::npos || end == from || end - from > MAX_RESOURCE_LEN) return {};
  return std::string(text.substr(from, end - from));
}

// "/Expired/name.epub", or "name (2).epub" and so on when that is taken.
std::string expiredDestination(const std::string& bookPath) {
  const std::string dir = std::string("/") + FOLDER_NAME;
  Storage.mkdir(dir.c_str());
  const std::string name = fileNameOf(bookPath);
  std::string dst = dir + "/" + name;
  if (!Storage.exists(dst.c_str())) return dst;
  const size_t dot = name.rfind('.');
  const std::string base = dot == std::string::npos ? name : name.substr(0, dot);
  const std::string ext = dot == std::string::npos ? "" : name.substr(dot);
  for (int suffix = 2; suffix < 100; suffix++) {
    dst = dir + "/" + base + " (" + std::to_string(suffix) + ")" + ext;
    if (!Storage.exists(dst.c_str())) return dst;
  }
  return {};
}

bool inRecents(const std::string& bookPath) {
  struct Ctx {
    const std::string& path;
    bool found;
  } ctx{bookPath, false};
  RECENT_BOOKS.scan(
      [](void* raw, const std::string_view path, uint32_t) {
        auto& c = *static_cast<Ctx*>(raw);
        c.found = path == c.path;
        return !c.found;
      },
      &ctx);
  return ctx.found;
}

}  // namespace

bool isExpired(const std::string& bookPath) {
  int64_t expiresAt = 0;
  if (!bookkey::readExpiry(bookPath, &expiresAt) || expiresAt == 0) return false;
  const int64_t now = trustedtime::trustedNow();
  return now != 0 && now > expiresAt;
}

bool inExpiredFolder(const std::string_view path) { return recentline::topFolder(path) == FOLDER_NAME; }

bool moveToExpiredFolder(const std::string& bookPath) {
  const std::string dst = expiredDestination(bookPath);
  if (dst.empty()) {
    LOG_ERR("LOAN", "No free name in /%s for %s", FOLDER_NAME, bookPath.c_str());
    return false;
  }
  if (!Storage.rename(bookPath.c_str(), dst.c_str())) {
    LOG_ERR("LOAN", "Failed to move expired loan %s -> %s", bookPath.c_str(), dst.c_str());
    return false;
  }
  LOG_INF("LOAN", "Moved expired loan %s -> %s", bookPath.c_str(), dst.c_str());
  relocateBookSidecars(bookPath, dst);
  return true;
}

bool adoptExpiredCopy(const std::string& bookPath) {
  if (inExpiredFolder(bookPath)) return false;
  if (!Storage.exists((bookPath + ".key").c_str())) return false;  // not a loan
  const std::string oldPath = std::string("/") + FOLDER_NAME + "/" + fileNameOf(bookPath);
  if (!Storage.exists(oldPath.c_str())) return false;
  // A copy already being read keeps its own place; nothing is merged.
  if (bookHasProgress(bookPath)) return false;

  const std::string resource = rightsResource(bookPath);
  if (resource.empty() || resource != rightsResource(oldPath)) return false;

  LOG_INF("LOAN", "Re-borrowed %s: adopting data from %s", bookPath.c_str(), oldPath.c_str());
  // The expired file and its key go first, so relocation only carries the cache and
  // marks, and never overwrites the new loan's key.
  Storage.remove(oldPath.c_str());
  Storage.remove((oldPath + ".key").c_str());
  Storage.remove((oldPath + ".rights").c_str());
  // Both copies may be in Recent Books; the new one's entry stays.
  if (inRecents(bookPath)) RECENT_BOOKS.removeByPath(oldPath);
  adoptBookCache(oldPath, bookPath);
  return true;
}

}  // namespace loanexpiry
