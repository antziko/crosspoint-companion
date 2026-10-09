#pragma once

#include <cstddef>
#include <cstdint>

// Due-date text for a protected book on loan (Epub::getLoanExpiresAt()).
namespace loandue {

// True when the loan ends within the reminder window and has not ended yet.
bool dueSoon(int64_t expiresAt);

// "Due 21 Oct · 6d 7h" (under a day: "Due 21 Oct · 6h") into buf. False when the book is not on
// loan, the loan has ended, or there is no trusted clock to count from.
bool describe(int64_t expiresAt, char* buf, size_t size);

}  // namespace loandue
