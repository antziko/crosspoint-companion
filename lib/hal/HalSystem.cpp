#include "HalSystem.h"

#include <string>

#include "Arduino.h"
#include "HalStorage.h"
#include "Logging.h"
#include "esp_debug_helpers.h"
#include "esp_private/esp_cpu_internal.h"
#include "esp_private/esp_system_attr.h"
#include "esp_private/panic_internal.h"

#define MAX_PANIC_STACK_DEPTH 32

RTC_NOINIT_ATTR char panicMessage[256];
RTC_NOINIT_ATTR HalSystem::StackFrame panicStack[MAX_PANIC_STACK_DEPTH];

#ifdef TRACE_OOM_ALLOC
// Diagnostic (dev builds only): with -fno-exceptions a failing throwing `new`
// calls abort(), so the crash_report backtrace names the *caller* but never the
// requested size. Replace the global throwing operator new to record the size,
// caller PC, and heap state at the moment of failure into RTC_NOINIT (survives the
// panic reboot), surfaced in getPanicInfo(). The happy path is a plain malloc
// passthrough — identical cost to the default.
//
// CRITICAL: we MUST also override the *nothrow* operators here. libstdc++'s
// `operator new(size, nothrow_t)` is implemented by calling the throwing
// `operator new(size)` inside a try/catch and returning nullptr if it throws.
// Because the app is built `-fno-exceptions`, that catch is dead — so once we
// override the throwing version to abort(), the nothrow version inherits the
// abort() and `new (std::nothrow)` (hence makeUniqueNoThrow) NO LONGER returns
// null on OOM. That silently defeats every graceful low-heap fallback in the
// firmware, but ONLY in TRACE_OOM_ALLOC builds — i.e. exactly the diagnostic
// builds used to hunt OOMs, which then crash where production would not. The
// nothrow overrides below restore the contract: record the event, return null.
#include <cstdlib>
#include <new>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"

// --- Last-failure snapshot (survives the panic reboot, dumped to crash_report) ---
RTC_NOINIT_ATTR uint32_t oomSize;          // bytes requested by the failing allocation
RTC_NOINIT_ATTR uint32_t oomCallerPC;      // return address of the code doing `new`
RTC_NOINIT_ATTR uint32_t oomFreeBytes;     // total free heap at failure
RTC_NOINIT_ATTR uint32_t oomLargest;       // largest contiguous free block at failure
RTC_NOINIT_ATTR uint32_t oomWasNothrow;    // 1 if last failure was a (recoverable) nothrow new
RTC_NOINIT_ATTR uint32_t oomNothrowCount;  // cumulative recoverable nothrow OOMs this session
RTC_NOINIT_ATTR uint32_t oomThrowCount;    // cumulative fatal throwing-new OOMs this session

// --- DRAM ring of recent OOM events, drained to SD from a safe context ---
// SD I/O is NOT safe from inside operator new (the SD stack itself allocates ->
// re-entrant new; HalStorage's mutex may already be held by this task). So the
// failure path only touches RAM here; HalSystem::drainOomTrace() (called from the
// main loop) flushes pending events to /oom_trace.txt.
namespace {
struct OomEvent {
  uint32_t atMillis;
  uint32_t size;
  uint32_t callerPC;
  uint32_t freeBytes;
  uint32_t largest;
  bool nothrow;
};
constexpr uint32_t OOM_RING_N = 16;
OomEvent g_oomRing[OOM_RING_N];
uint32_t g_oomRingHead = 0;     // next slot to write
uint32_t g_oomRingPending = 0;  // unflushed events (capped at OOM_RING_N)
portMUX_TYPE g_oomMux = portMUX_INITIALIZER_UNLOCKED;
}  // namespace

static void recordOom(std::size_t size, uint32_t callerPC, bool nothrow) {
  const uint32_t freeBytes = static_cast<uint32_t>(esp_get_free_heap_size());
  const uint32_t largest = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

  oomSize = static_cast<uint32_t>(size);
  oomCallerPC = callerPC;
  oomFreeBytes = freeBytes;
  oomLargest = largest;
  oomWasNothrow = nothrow ? 1u : 0u;

  // Only the rare failure path takes the lock, so the happy alloc path is untouched.
  portENTER_CRITICAL(&g_oomMux);
  if (nothrow)
    oomNothrowCount++;
  else
    oomThrowCount++;
  OomEvent& e = g_oomRing[g_oomRingHead];
  e.atMillis = static_cast<uint32_t>(millis());
  e.size = static_cast<uint32_t>(size);
  e.callerPC = callerPC;
  e.freeBytes = freeBytes;
  e.largest = largest;
  e.nothrow = nothrow;
  g_oomRingHead = (g_oomRingHead + 1) % OOM_RING_N;
  if (g_oomRingPending < OOM_RING_N) g_oomRingPending++;
  portEXIT_CRITICAL(&g_oomMux);
}

void* operator new(std::size_t size) {
  void* p = malloc(size);
  if (p) return p;
  recordOom(size, reinterpret_cast<uint32_t>(__builtin_return_address(0)), /*nothrow=*/false);
  abort();  // mirror the -fno-exceptions default (bad_alloc -> terminate -> abort)
}

void* operator new[](std::size_t size) {
  void* p = malloc(size);
  if (p) return p;
  recordOom(size, reinterpret_cast<uint32_t>(__builtin_return_address(0)), /*nothrow=*/false);
  abort();
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  void* p = malloc(size);
  if (p) return p;
  recordOom(size, reinterpret_cast<uint32_t>(__builtin_return_address(0)), /*nothrow=*/true);
  return nullptr;  // graceful: makeUniqueNoThrow callers handle null
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  void* p = malloc(size);
  if (p) return p;
  recordOom(size, reinterpret_cast<uint32_t>(__builtin_return_address(0)), /*nothrow=*/true);
  return nullptr;
}
#endif  // TRACE_OOM_ALLOC

extern "C" {

void __real_panic_abort(const char* message);
void __real_panic_print_backtrace(const void* frame, int core);

static DRAM_ATTR const char PANIC_REASON_UNKNOWN[] = "(unknown panic reason)";
void IRAM_ATTR __wrap_panic_abort(const char* message) {
  if (!message) message = PANIC_REASON_UNKNOWN;
  // IRAM-safe bounded copy (strncpy is not IRAM-safe in panic context)
  int i = 0;
  for (; i < (int)sizeof(panicMessage) - 1 && message[i]; i++) {
    panicMessage[i] = message[i];
  }
  panicMessage[i] = '\0';

  __real_panic_abort(message);
}

void IRAM_ATTR __wrap_panic_print_backtrace(const void* frame, int core) {
  if (!frame) {
    __real_panic_print_backtrace(frame, core);
    return;
  }

#if !__riscv
  __real_panic_print_backtrace(frame, core);
  return;
#else
  for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
    panicStack[i].sp = 0;
  }

  // Copied from components/esp_system/port/arch/riscv/panic_arch.c
  uint32_t sp = (uint32_t)((RvExcFrame*)frame)->sp;
  const int per_line = 8;
  int depth = 0;
  for (int x = 0; x < 1024; x += per_line * sizeof(uint32_t)) {
    uint32_t* spp = (uint32_t*)(sp + x);
    // panic_print_hex(sp + x);
    // panic_print_str(": ");
    panicStack[depth].sp = sp + x;
    for (int y = 0; y < per_line; y++) {
      // panic_print_str("0x");
      // panic_print_hex(spp[y]);
      // panic_print_str(y == per_line - 1 ? "\r\n" : " ");
      panicStack[depth].spp[y] = spp[y];
    }

    depth++;
    if (depth >= MAX_PANIC_STACK_DEPTH) {
      break;
    }
  }

  __real_panic_print_backtrace(frame, core);
#endif
}
}

namespace HalSystem {

void begin() {
  // This is mostly for the first boot, we need to initialize the panic info and logs to empty state
  // If we reboot from a panic state, we want to keep the panic info until we successfully dump it to the SD card, use
  // `clearPanic()` to clear it after dumping
  if (!isRebootFromPanic()) {
    clearPanic();
  } else {
    // Panic reboot: preserve logs and panic info, but clamp logHead in case the
    // panic occurred before begin() ever ran (e.g. in a static constructor).
    // If logHead was out of range, logMessages is also garbage — clear it so
    // getLastLogs() does not dump corrupt data into the crash report.
    if (sanitizeLogHead()) {
      clearLastLogs();
    }
  }
}

void checkPanic() {
  if (isRebootFromPanic()) {
    auto panicInfo = getPanicInfo(true);
    auto file = Storage.open("/crash_report.txt", O_WRITE | O_CREAT | O_TRUNC);
    if (file) {
      file.write(panicInfo.c_str(), panicInfo.size());
      file.close();
      LOG_INF("SYS", "Dumped panic info to SD card");
    } else {
      LOG_ERR("SYS", "Failed to open crash_report.txt for writing");
    }
  }
}

void clearPanic() {
  panicMessage[0] = '\0';
  for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
    panicStack[i].sp = 0;
  }
#ifdef TRACE_OOM_ALLOC
  oomSize = 0;  // clear stale OOM trace on a clean (non-panic) boot
  oomWasNothrow = 0;
  oomNothrowCount = 0;
  oomThrowCount = 0;
#endif
  clearLastLogs();
}

// Flush any pending OOM events captured by the operator-new trace to SD. MUST be
// called from a normal task context only (it does SD I/O) — never from inside an
// allocation path. Cheap no-op when nothing is pending. See the TRACE_OOM_ALLOC
// block above for why the failure path can't write SD itself.
void drainOomTrace() {
#ifdef TRACE_OOM_ALLOC
  if (g_oomRingPending == 0) return;

  // Snapshot the ring under lock, then release before touching the SD card.
  OomEvent events[OOM_RING_N];
  uint32_t count;
  portENTER_CRITICAL(&g_oomMux);
  count = g_oomRingPending;
  const uint32_t start = (g_oomRingHead + OOM_RING_N - count) % OOM_RING_N;
  for (uint32_t i = 0; i < count; i++) {
    events[i] = g_oomRing[(start + i) % OOM_RING_N];
  }
  g_oomRingPending = 0;
  portEXIT_CRITICAL(&g_oomMux);

  // .txt so the on-device file browser opens it directly (X3 has no serial). Cap
  // the size so a chronic-OOM session can't grow it without bound.
  constexpr const char* OOM_TRACE_PATH = "/oom_trace.txt";
  constexpr size_t MAX_TRACE_BYTES = 256 * 1024;
  HalFile probe;
  if (Storage.openFileForRead("OOMTRACE", OOM_TRACE_PATH, probe)) {
    const size_t sz = probe.size();
    probe.close();
    if (sz > MAX_TRACE_BYTES) Storage.remove(OOM_TRACE_PATH);
  }

  HalFile file;
  if (!Storage.openFileForAppend("OOMTRACE", OOM_TRACE_PATH, file)) return;
  for (uint32_t i = 0; i < count; i++) {
    const OomEvent& e = events[i];
    char line[160];
    const int len =
        snprintf(line, sizeof(line), "[%lu] %s OOM size=%lu callerPC=0x%08lX free=%lu largest=%lu\n",
                 (unsigned long)e.atMillis, e.nothrow ? "nothrow(recovered)" : "throwing(FATAL)", (unsigned long)e.size,
                 (unsigned long)e.callerPC, (unsigned long)e.freeBytes, (unsigned long)e.largest);
    if (len > 0) file.write(line, static_cast<size_t>(len));
  }
  file.flush();
#endif  // TRACE_OOM_ALLOC
}

std::string getPanicInfo(bool full) {
  if (!full) {
    return panicMessage;
  } else {
    std::string info;

    info += "CrossPoint version: " CROSSPOINT_VERSION;
    info += "\n\nPanic reason: " + std::string(panicMessage);

#ifdef TRACE_OOM_ALLOC
    // Only meaningful when the panic was an allocation failure; if the backtrace
    // below does not run through operator new, treat this as stale (prior OOM).
    if (oomSize != 0) {
      char buf[280];
      // A nothrow failure returns null and the device keeps running, so if the
      // last recorded OOM was nothrow it is NOT what panicked — flag it as such so
      // the trace isn't misread as the crash cause.
      snprintf(buf, sizeof(buf),
               "\n\nLast failed allocation (stale unless panic is OOM):\n  size=%lu bytes  callerPC=0x%08lX  "
               "freeAtFail=%lu  largestBlock=%lu  kind=%s\n  OOM counts this session: nothrow(recovered)=%lu  "
               "throwing(fatal)=%lu",
               (unsigned long)oomSize, (unsigned long)oomCallerPC, (unsigned long)oomFreeBytes,
               (unsigned long)oomLargest, oomWasNothrow ? "nothrow(recovered, NOT the panic cause)" : "throwing(fatal)",
               (unsigned long)oomNothrowCount, (unsigned long)oomThrowCount);
      info += buf;
    }
#endif

    info += "\n\nLast logs:\n" + getLastLogs();
    info += "\n\nStack memory:\n";

    auto toHex = [](uint32_t value) {
      char buffer[9];
      snprintf(buffer, sizeof(buffer), "%08X", value);
      return std::string(buffer);
    };
    for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
      if (panicStack[i].sp == 0) {
        break;
      }
      info += "0x" + toHex(panicStack[i].sp) + ": ";
      for (size_t j = 0; j < 8; j++) {
        info += "0x" + toHex(panicStack[i].spp[j]) + " ";
      }
      info += "\n";
    }

    return info;
  }
}

bool isRebootFromPanic() {
  const auto resetReason = esp_reset_reason();
  return resetReason == ESP_RST_PANIC || resetReason == ESP_RST_CPU_LOCKUP || resetReason == ESP_RST_INT_WDT ||
         resetReason == ESP_RST_TASK_WDT || resetReason == ESP_RST_WDT;
}

}  // namespace HalSystem
