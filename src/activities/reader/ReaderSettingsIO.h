#pragma once

#include <string>

#include "CrossPointSettings.h"

// Per-book reader settings persistence (reader_settings.bin inside the epub
// cache dir). Single source of truth for the on-disk format, shared by the
// reader (seed/load on open) and the in-reader options editor (write on change).
// Keeping one implementation guarantees the writer and reader can never drift
// out of sync on field order or file version.
namespace ReaderSettingsIO {

// Reads reader_settings.bin from `cachePath`. On success fills `out` (with
// active = true) and returns true. Returns false if the file is missing or its
// version does not match the current layout.
bool load(const std::string& cachePath, CrossPointSettings::ReaderOverride& out);

// Writes `ov` to reader_settings.bin in `cachePath`. Returns true on success.
bool write(const std::string& cachePath, const CrossPointSettings::ReaderOverride& ov);

}  // namespace ReaderSettingsIO
