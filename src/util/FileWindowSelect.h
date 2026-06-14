#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Bounded, streaming directory-window selection. Lets FileBrowserActivity show one
// screen ("window") of a folder at a time without ever materialising the whole listing,
// so RAM is bounded by the window size K regardless of how many files the folder holds
// (the fix for OOM-aborts on very large folders). The directory is re-scanned once per
// page-turn; each scan feeds every entry to a WindowSelector, which keeps at most K
// entries in sorted order.
//
// Comparator-injected so this is pure and host-testable: the device supplies a strict
// TOTAL order over names (natural order + a byte-compare tiebreak so two distinct names
// are never "equal" — required for cursor paging to never skip or duplicate an entry).
namespace filewindow {

struct Entry {
  std::string name;
  uint32_t size = 0;
  bool isDir = false;
};

// Must be a strict total order: less(a,b) xor less(b,a) for every distinct a,b, and
// !less(a,a). The device wraps FsHelpers::naturalFileLess with a byte tiebreak to
// guarantee this even when natural order treats two names as equivalent.
using NameLess = bool (*)(const std::string&, const std::string&);

class WindowSelector {
 public:
  enum class Mode {
    First,      // K smallest names overall (first page)
    Last,       // K largest names overall (last page) — returned ascending
    After,      // K smallest names strictly greater than cursor (next page)
    AtOrAfter,  // K smallest names >= cursor (page that starts at/just after a name)
    Before,     // K largest names strictly less than cursor (previous page) — ascending
  };

  WindowSelector(Mode mode, std::string cursor, size_t k, NameLess less)
      : mode_(mode), cursor_(std::move(cursor)), k_(k), less_(less) {}

  // Feed one directory entry. O(K) worst case (K is one screen, ~8-15).
  void consider(const Entry& e);

  // The selected window, ascending by name (size <= K).
  const std::vector<Entry>& window() const { return buf_; }

  // True when at least one eligible entry was seen beyond the kept window edge — i.e.
  // there is a further page in the paging direction. Lets the caller show/hide the
  // more-pages affordance and decide wrap behaviour without a second scan.
  bool overflowed() const { return overflowed_; }

 private:
  bool eligible(const std::string& name) const;
  // Whether we keep the SMALLEST names (First/After/AtOrAfter) or the LARGEST (Last/Before).
  bool keepSmallest() const { return mode_ != Mode::Last && mode_ != Mode::Before; }

  Mode mode_;
  std::string cursor_;
  size_t k_;
  NameLess less_;
  std::vector<Entry> buf_;  // kept sorted ascending, size <= k_
  bool overflowed_ = false;
};

}  // namespace filewindow
