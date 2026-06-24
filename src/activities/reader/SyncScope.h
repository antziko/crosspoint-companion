#pragma once

// What a KOReader sync run covers. ALL is the default full sync (progress
// comparison + bookmarks + stats + dictionary history + flashcards). The
// single-feature scopes skip the progress-comparison UI and run just that one
// feature, then show a completion summary. Each feature opens its own short
// keep-alive connection (see KOReaderSyncActivity::performSync) so one feature's
// failure can't poison others. Stats / Dict / Flashcards all ride the stats
// endpoint but gate independently (counters / "dh" blob / "fc" blob).
// Mixed-case enumerators on purpose: an all-caps `BOOKMARKS` would collide with the
// `#define BOOKMARKS BookmarkStore::getInstance()` macro in BookmarkStore.h.
enum class SyncScope { All, Progress, Bookmarks, Stats, Dict, Flashcards };
