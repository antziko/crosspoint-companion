#pragma once

#include <string>
#include <string_view>

// Expired library loans: the /Expired folder they are moved to, and the hand-back of
// their reading data when the same book is borrowed again.
namespace loanexpiry {

// Top-level folder name; also the Recent Books filter that lists it.
constexpr char FOLDER_NAME[] = "Expired";

// True when bookPath is a loan whose due date has passed by the trusted clock. Reads
// only the key header (display use; opening the book enforces the date itself).
bool isExpired(const std::string& bookPath);

// True when the path sits under /Expired.
bool inExpiredFolder(std::string_view path);

// Moves the book, its key/rights files and its cache into /Expired, keeping it in
// Recent Books. False (logged) when the rename fails; nothing is changed then.
bool moveToExpiredFolder(const std::string& bookPath);

// A re-borrowed loan with no progress of its own takes over the reading data of its
// expired copy (/Expired/<same file name>, same rights resource id), and that copy is
// deleted. Call before the book's cache dir is first used. True when adopted.
bool adoptExpiredCopy(const std::string& bookPath);

}  // namespace loanexpiry
