#include <cstdio>
#include <cstring>

#include "TextBookInternal.h"

namespace textbook {

std::string partHref(const uint16_t index) {
  char name[16];
  snprintf(name, sizeof(name), "p%04u.xhtml", static_cast<unsigned>(index));
  return name;
}

bool isPartHref(const std::string_view href) {
  if (href.size() != 11 || href[0] != 'p' || href.substr(5) != ".xhtml") return false;
  for (size_t i = 1; i < 5; i++) {
    if (href[i] < '0' || href[i] > '9') return false;
  }
  return true;
}

namespace detail {

bool LineReader::fill() {
  if (eof) return false;
  const int n = src.read(buf.get(), BUF_SIZE);
  if (n < 0) readError = true;
  if (n <= 0) {
    eof = true;
    return false;
  }
  len = static_cast<size_t>(n);
  pos = 0;
  if (!started) {
    started = true;
    if (len >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF) pos = 3;
  }
  return true;
}

bool LineReader::next(std::string& line, bool& partial, size_t& bytes) {
  line.clear();
  partial = false;
  bytes = 0;
  bool any = false;
  while (true) {
    if (pos >= len && !fill()) return any;
    any = true;
    const auto* start = buf.get() + pos;
    const auto* nl = static_cast<const uint8_t*>(memchr(start, '\n', len - pos));
    const size_t take = nl ? static_cast<size_t>(nl - start) : len - pos;
    size_t room = MAX_LINE_BYTES - line.size();
    if (take > room) {
      line.append(reinterpret_cast<const char*>(start), room);
      pos += room;
      bytes += room;
      partial = true;
      return true;
    }
    line.append(reinterpret_cast<const char*>(start), take);
    pos += take;
    bytes += take;
    if (nl) {
      pos++;
      bytes++;
      if (!line.empty() && line.back() == '\r') line.pop_back();
      return true;
    }
  }
}

void appendEscaped(std::string& out, const std::string_view text) {
  for (const char c : text) {
    switch (c) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      default:
        if (static_cast<uint8_t>(c) < 0x20 && c != '\t') {
          out += ' ';
        } else {
          out += c;
        }
    }
  }
}

void appendEscapedAttr(std::string& out, const std::string_view text) {
  for (const char c : text) {
    if (c == '"') {
      out += "&quot;";
    } else {
      appendEscaped(out, std::string_view(&c, 1));
    }
  }
}

void appendPlainText(std::string& out, const std::string_view text, SpaceState& st) {
  for (const char c : text) {
    if (c == ' ') {
      if (st.atLineStart) {
        out += "&#160;";
      } else {
        st.pendingSpaces++;
      }
      continue;
    }
    if (st.pendingSpaces > 0) {
      for (size_t s = 1; s < st.pendingSpaces; s++) out += "&#160;";
      out += ' ';
      st.pendingSpaces = 0;
    }
    st.atLineStart = false;
    appendEscaped(out, std::string_view(&c, 1));
  }
}

void finishPlainLine(std::string&, SpaceState& st) {
  st.atLineStart = true;
  st.pendingSpaces = 0;
}

void appendPartHeader(std::string& out, const std::string_view title) {
  out += "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>";
  appendEscaped(out, title);
  out += "</title></head><body>\n";
}

void appendPartFooter(std::string& out) { out += "</body></html>\n"; }

}  // namespace detail
}  // namespace textbook
