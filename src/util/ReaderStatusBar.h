#pragma once

#include <string>

// Per-book "Toggle Status Bar" state (APP_STATE.statusBarHidden), kept in the book's cache
// directory as statusbar.bin: byte 0 = version, byte 1 = hidden. The reader that has a book
// open calls load() before its first render and unload() on exit; toggle() flips the flag and
// saves it for that book.
namespace ReaderStatusBar {
void load(const std::string& bookCachePath);
void unload();
void toggle();
}  // namespace ReaderStatusBar
