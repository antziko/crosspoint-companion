#pragma once

#include <atomic>

// Defers "this is the book that is open" until a page has actually reached the panel.
//
// The remembered path drives quick resume on wake, so recording it at open time means a
// book that cannot be indexed -- or that aborts partway through its first layout -- is
// reopened on every wake, and the only way out is pulling the SD card. Entering a reader
// therefore drops the remembered book first and puts it back once a page has rendered.
//
// The flag is set on the render task and read on the main task, hence the atomic; the
// commit itself (an SD write) stays on the main task.
class OpenBookRecord {
  std::atomic<bool> pageRendered{false};
  bool committed = false;

 public:
  void markPageRendered() { pageRendered.store(true, std::memory_order_release); }

  // True exactly once, on the first main-task pass after a page has landed.
  bool shouldCommit() {
    if (committed || !pageRendered.load(std::memory_order_acquire)) return false;
    committed = true;
    return true;
  }
};
