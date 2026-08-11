#pragma once

#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

// Nothrow versions of std::make_unique. Return nullptr on allocation failure
// instead of calling abort() (the default when exceptions are disabled on ESP32).
//
// Single object:
//   auto obj = makeUniqueNoThrow<PNG>();
//   if (!obj) { LOG_ERR("TAG", "OOM"); return false; }
//
// Array:
//   auto buf = makeUniqueNoThrow<uint8_t[]>(size);
//   if (!buf) { LOG_ERR("TAG", "OOM"); return false; }
//   buf[0] = 0xFF;
//   someApi(buf.get(), size);
//

template <typename T, typename... Args>
  requires(!std::is_array_v<T>)
std::unique_ptr<T> makeUniqueNoThrow(Args&&... args) {
  return std::unique_ptr<T>(new (std::nothrow) T(std::forward<Args>(args)...));
}

template <typename T>
  requires std::is_unbounded_array_v<T>
std::unique_ptr<T> makeUniqueNoThrow(size_t count) {
  using Elem = std::remove_extent_t<T>;
  return std::unique_ptr<T>(new (std::nothrow) Elem[count]());
}

// Nothrow std::vector::reserve. With -fno-exceptions a failed reserve() calls
// std::__throw_bad_alloc() -> abort(): the device reboots instead of degrading. This probes
// the allocation reserve() is about to make using operator new(nothrow), which returns
// nullptr rather than aborting, and only calls reserve() once the block is known to exist.
//
//   if (!reserveNoThrow(myVec, n)) { LOG_ERR("TAG", "OOM"); return false; }
//
// Caveat, stated plainly: the probe is freed before reserve() re-takes it, so on a
// preempting task another allocation could win the block in between and the reserve would
// still abort. That window is a few instructions wide on a single-core MCU, and this
// converts an unconditional abort into a near-certain graceful failure — it is not a
// guarantee. Where a hard guarantee is needed, size the container once up front and never
// grow it (check size() < capacity() before each push_back).
template <typename T>
[[nodiscard]] bool reserveNoThrow(std::vector<T>& v, const size_t n) {
  if (n <= v.capacity()) return true;
  if (n > v.max_size()) return false;
  void* probe = ::operator new(n * sizeof(T), std::nothrow);
  if (!probe) return false;
  ::operator delete(probe, std::nothrow);
  v.reserve(n);
  return true;
}

// Helper struct to call a cleanup function on exit from any scope.
// Use with a lambda to avoid unnecessary allocations from std::function/std::bind:
// Example:
//   auto jpeg = makeUniqueNoThrow<JPEGDEC>();
//   ScopedCleanup cleanup{[&jpeg]{ jpeg->close(); }};
//
template <typename F>
struct [[nodiscard]] ScopedCleanup final {
  const F fn;
  explicit ScopedCleanup(F f) : fn{std::move(f)} {}
  ScopedCleanup(const ScopedCleanup&) = delete;
  ScopedCleanup& operator=(const ScopedCleanup&) = delete;
  ScopedCleanup(ScopedCleanup&&) = delete;
  ScopedCleanup& operator=(ScopedCleanup&&) = delete;
  ~ScopedCleanup() { fn(); }
};

template <typename F>
ScopedCleanup(F) -> ScopedCleanup<F>;
