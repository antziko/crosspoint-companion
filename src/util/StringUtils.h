#pragma once

#include <string>

namespace StringUtils {

/**
 * Sanitize a string for use as a filename.
 * Replaces invalid characters with underscores, trims spaces/dots,
 * and limits length to maxBytes bytes.
 */
std::string sanitizeFilename(const std::string& name, size_t maxBytes = 100);

/**
 * Sanitize a complete filename while keeping a conventional extension inside
 * the byte limit. The stem is shortened on a UTF-8 codepoint boundary.
 */
std::string sanitizeFilenamePreservingExtension(const std::string& name, size_t maxBytes = 100);

}  // namespace StringUtils
