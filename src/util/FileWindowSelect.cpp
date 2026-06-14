#include "FileWindowSelect.h"

#include <algorithm>

namespace filewindow {

bool WindowSelector::eligible(const std::string& name) const {
  switch (mode_) {
    case Mode::First:
    case Mode::Last:
      return true;
    case Mode::After:
      return less_(cursor_, name);  // name > cursor
    case Mode::AtOrAfter:
      return !less_(name, cursor_);  // name >= cursor
    case Mode::Before:
      return less_(name, cursor_);  // name < cursor
  }
  return false;
}

void WindowSelector::consider(const Entry& e) {
  if (!eligible(e.name)) return;

  // Find the sorted insertion point (buf_ is kept ascending by name).
  const auto pos = std::lower_bound(buf_.begin(), buf_.end(), e.name,
                                    [this](const Entry& x, const std::string& n) { return less_(x.name, n); });

  if (buf_.size() < k_) {
    buf_.insert(pos, e);
    return;
  }

  if (keepSmallest()) {
    // Want the K smallest: replace the current largest (back) iff this is smaller.
    if (less_(e.name, buf_.back().name)) {
      buf_.insert(pos, e);
      buf_.pop_back();  // drop the largest; an entry fell outside the window
      overflowed_ = true;
    } else {
      overflowed_ = true;  // an eligible entry exists past the window's far edge
    }
  } else {
    // Want the K largest: replace the current smallest (front) iff this is larger.
    if (less_(buf_.front().name, e.name)) {
      buf_.insert(pos, e);
      buf_.erase(buf_.begin());  // drop the smallest
      overflowed_ = true;
    } else {
      overflowed_ = true;
    }
  }
}

}  // namespace filewindow
