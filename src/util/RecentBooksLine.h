#pragma once

#include <string>
#include <string_view>

// The recent-books history file holds one book per line: path, title, author
// and cover path, tab-separated, newest first. Pure helpers, host-testable.
namespace recentline {

constexpr char SEPARATOR = '\t';

struct Fields {
  std::string_view path;
  std::string_view title;
  std::string_view author;
  std::string_view cover;
};

// Appends `field` with the line's own separators (tab, CR, LF) turned into
// spaces, so a stray one in a title cannot split or end the line.
inline void appendField(std::string& line, const std::string_view field) {
  for (const char c : field) line += (c == SEPARATOR || c == '\r' || c == '\n') ? ' ' : c;
}

// One complete line, newline included.
inline std::string encode(const std::string_view path, const std::string_view title, const std::string_view author,
                          const std::string_view cover) {
  std::string line;
  line.reserve(path.size() + title.size() + author.size() + cover.size() + 4);
  appendField(line, path);
  line += SEPARATOR;
  appendField(line, title);
  line += SEPARATOR;
  appendField(line, author);
  line += SEPARATOR;
  appendField(line, cover);
  line += '\n';
  return line;
}

// Splits a line given without its newline. Missing trailing fields are empty;
// false when the line carries no path (blank or malformed).
inline bool decode(std::string_view line, Fields& out) {
  if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
  std::string_view* fields[] = {&out.path, &out.title, &out.author, &out.cover};
  for (std::string_view* field : fields) *field = {};
  for (std::string_view* field : fields) {
    const size_t tab = line.find(SEPARATOR);
    *field = line.substr(0, tab);
    if (tab == std::string_view::npos) break;
    line.remove_prefix(tab + 1);
  }
  return !out.path.empty();
}

// The first folder of an absolute path: "calibre" for "/calibre/x/y.epub",
// empty for a book at the card root.
inline std::string_view topFolder(std::string_view path) {
  while (!path.empty() && path.front() == '/') path.remove_prefix(1);
  const size_t slash = path.find('/');
  return slash == std::string_view::npos ? std::string_view{} : path.substr(0, slash);
}

}  // namespace recentline
