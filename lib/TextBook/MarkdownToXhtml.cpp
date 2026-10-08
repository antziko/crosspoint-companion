// CommonMark-lite Markdown -> XHTML parts.
//
// Two passes over the source with the same block logic. Pass 1 writes nothing; it records
// which part every heading / footnote id lands in, the reference-link definitions, and the
// title, so pass 2 can point "#slug", "[^1]" and "[text][ref]" links at the right part file.
// Both passes split parts at the same top-level boundaries because they run the same code.
#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "TextBookInternal.h"

namespace textbook {

using namespace detail;

namespace {

constexpr size_t MAX_DEPTH = 12;
constexpr size_t MAX_REF_DEFS = 256;
constexpr size_t PARA_FLUSH_BYTES = 8 * 1024;
constexpr size_t OUT_FLUSH_BYTES = 2048;
constexpr char HARD_BREAK = '\x02';
constexpr std::string_view CODE_BLOCK_OPEN = "<div style=\"margin-left:1em;text-indent:0\">";

uint32_t fnv1a(const std::string_view s) {
  uint32_t h = 2166136261u;
  for (const char c : s) {
    h ^= static_cast<uint8_t>(c);
    h *= 16777619u;
  }
  return h;
}

bool isSpace(const char c) { return c == ' ' || c == '\t' || c == '\n' || c == HARD_BREAK; }
bool isAlnum(const char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }
bool isAsciiPunct(const char c) { return std::ispunct(static_cast<unsigned char>(c)) != 0; }

size_t countRun(const std::string_view s, size_t i, const char c) {
  const size_t start = i;
  while (i < s.size() && s[i] == c) i++;
  return i - start;
}

size_t countSpaces(const std::string_view s, size_t i = 0) {
  const size_t start = i;
  while (i < s.size() && s[i] == ' ') i++;
  return i - start;
}

bool isBlank(const std::string_view s) { return countSpaces(s) == s.size(); }

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

// GitHub heading slug: lowercase ASCII, spaces to '-', punctuation other than '-'/'_' dropped,
// non-ASCII bytes kept.
std::string slugify(const std::string_view text) {
  std::string slug;
  slug.reserve(text.size());
  for (const char c : text) {
    const auto u = static_cast<unsigned char>(c);
    if (u >= 0x80 || isAlnum(c) || c == '-' || c == '_') {
      slug += static_cast<char>(std::tolower(u));
    } else if (c == ' ') {
      slug += '-';
    }
  }
  return slug;
}

std::string footnoteId(const std::string_view label) {
  std::string id = "fn-";
  for (const char c : label) id += (isAlnum(c) || c == '-' || c == '_') ? c : '-';
  return id;
}

// Reference labels match case-insensitively with inner whitespace collapsed.
std::string normalizeLabel(const std::string_view label) {
  std::string out;
  bool space = false;
  for (const char c : trim(label)) {
    if (isSpace(c)) {
      space = true;
      continue;
    }
    if (space && !out.empty()) out += ' ';
    space = false;
    out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

std::string unescapeBackslashes(const std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '\\' && i + 1 < s.size() && isAsciiPunct(s[i + 1])) i++;
    out += s[i];
  }
  return out;
}

std::string percentDecode(const std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
        std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
      const char hex[3] = {s[i + 1], s[i + 2], '\0'};
      out += static_cast<char>(strtol(hex, nullptr, 16));
      i += 2;
    } else {
      out += s[i];
    }
  }
  return out;
}

bool isExternalUrl(const std::string_view url) {
  const size_t colon = url.find(':');
  if (colon == std::string_view::npos || colon == 0) return false;
  for (size_t i = 0; i < colon; i++) {
    if (!std::isalpha(static_cast<unsigned char>(url[i])) && url[i] != '+' && url[i] != '-' && url[i] != '.') {
      return false;
    }
  }
  return true;
}

// Joins `dir` and a relative `path`, folding "." and "..", and returns it root-relative
// (no leading slash), which is how the EPUB parser hands image paths back to the book.
std::string resolveSdPath(const std::string_view dir, const std::string_view path) {
  std::string joined;
  if (!path.empty() && path[0] == '/') {
    joined = std::string(path);
  } else {
    joined = std::string(dir);
    joined += '/';
    joined += path;
  }
  std::vector<std::string_view> parts;
  parts.reserve(8);
  std::string_view rest(joined);
  while (!rest.empty()) {
    const size_t slash = rest.find('/');
    const std::string_view seg = rest.substr(0, slash);
    rest = slash == std::string_view::npos ? std::string_view() : rest.substr(slash + 1);
    if (seg.empty() || seg == ".") continue;
    if (seg == "..") {
      if (!parts.empty()) parts.pop_back();
      continue;
    }
    parts.push_back(seg);
  }
  std::string out;
  for (const auto& seg : parts) {
    if (!out.empty()) out += '/';
    out += seg;
  }
  return out;
}

struct NamedEntity {
  const char* name;
  const char* utf8;
};
// The XML five pass through as-is; these common HTML ones become their characters.
constexpr NamedEntity NAMED_ENTITIES[] = {
    {"nbsp", "&#160;"}, {"mdash", "\xE2\x80\x94"}, {"ndash", "\xE2\x80\x93"},  {"hellip", "\xE2\x80\xA6"},
    {"lsquo", "\xE2\x80\x98"}, {"rsquo", "\xE2\x80\x99"}, {"ldquo", "\xE2\x80\x9C"}, {"rdquo", "\xE2\x80\x9D"},
    {"laquo", "\xC2\xAB"},     {"raquo", "\xC2\xBB"},     {"copy", "\xC2\xA9"},      {"reg", "\xC2\xAE"},
    {"trade", "\xE2\x84\xA2"}, {"middot", "\xC2\xB7"},    {"bull", "\xE2\x80\xA2"},  {"times", "\xC3\x97"},
    {"deg", "\xC2\xB0"},       {"euro", "\xE2\x82\xAC"},  {"larr", "\xE2\x86\x90"},  {"rarr", "\xE2\x86\x92"},
};

struct Shared {
  std::vector<std::pair<uint32_t, uint16_t>> ids;  // fnv1a(id) -> part, sorted after pass 1
  struct RefDef {
    std::string label;
    std::string url;
  };
  std::vector<RefDef> refs;
  std::string title;
  std::string author;
  uint8_t minHeadingLevel = 7;

  int partOf(const std::string_view id) const {
    const uint32_t h = fnv1a(id);
    const auto it = std::lower_bound(ids.begin(), ids.end(), std::make_pair(h, uint16_t{0}));
    return (it != ids.end() && it->first == h) ? it->second : -1;
  }
  const RefDef* findRef(const std::string_view label) const {
    const std::string key = normalizeLabel(label);
    for (const auto& r : refs) {
      if (r.label == key) return &r;
    }
    return nullptr;
  }
};

class Converter {
 public:
  Converter(Sink* sink, Shared& sh, const std::string_view docDir, const std::string_view title)
      : sink(sink), sh(sh), pass1(sink == nullptr), docDir(docDir), title(title) {
    stack.reserve(MAX_DEPTH);
    para.reserve(512);
    out.reserve(OUT_FLUSH_BYTES + 512);
  }

  void line(const std::string_view raw, const size_t bytes) {
    if (failed) return;
    if (frontMatter(raw)) return;
    expand(raw);
    // Split before a top-level H1/H2 or once the part is big enough, but only where nothing
    // is open, so every part is well-formed on its own.
    if (partOpen && partHasContent && stack.empty() && leaf == Leaf::None) {
      const std::string_view r = std::string_view(expanded).substr(std::min<size_t>(countSpaces(expanded), 3));
      if (partBytes >= PART_SPLIT_BYTES || (atxLevel(r) >= 1 && atxLevel(r) <= 2)) closePart();
    }
    partBytes += static_cast<uint32_t>(bytes);
    process(expanded);
  }

  bool finish() {
    closeContainersFrom(0);
    resolveDangling(0);
    if (!partOpen && part == 0) ensurePart();
    if (partOpen) closePart();
    return !failed;
  }

  uint16_t parts() const { return part; }

 private:
  enum class Leaf : uint8_t { None, Para, Fence, Indented, Table };

  struct Container {
    bool quote;
    bool ordered;
    char marker;
    uint16_t contentIndent;
    bool hasText;
  };

  struct Dangling {
    bool active = false;
    bool ordered = false;
    char marker = 0;
  };

  struct ListMarker {
    bool ordered = false;
    char marker = 0;
    unsigned start = 1;
    size_t width = 0;   // marker plus the spaces consumed after it
    bool empty = true;  // nothing after the marker
  };

  Sink* sink;
  Shared& sh;
  bool pass1;
  std::string docDir;
  std::string title;

  std::string out;
  std::string expanded;
  std::string tmp;
  bool failed = false;

  uint16_t part = 0;
  bool partOpen = false;
  bool partHasContent = false;
  uint32_t partBytes = 0;

  std::vector<Container> stack;
  Dangling dangling[MAX_DEPTH + 1];

  Leaf leaf = Leaf::None;
  std::string para;
  size_t paraLines = 0;
  bool paraOpened = false;
  std::string paraFootnote;
  char fenceChar = 0;
  size_t fenceLen = 0;
  size_t fenceIndent = 0;
  bool codeFirstLine = true;
  size_t pendingCodeBlanks = 0;
  size_t tableCols = 0;
  std::unordered_map<uint32_t, uint16_t> slugCounts;
  uint8_t fmState = 0;  // 0 = first line not seen, 1 = inside front matter, 2 = done

  // ---- output ----

  void flushOut() {
    if (!out.empty() && sink && !sink->write(out.data(), out.size())) failed = true;
    out.clear();
  }

  void ensurePart() {
    if (partOpen) return;
    partOpen = true;
    partHasContent = false;
    partBytes = 0;
    if (pass1) return;
    if (!sink->beginPart(part)) failed = true;
    appendPartHeader(out, title);
  }

  void closePart() {
    resolveDangling(0);
    if (!pass1) {
      appendPartFooter(out);
      flushOut();
      if (!sink->endPart()) failed = true;
    }
    partOpen = false;
    part++;
  }

  void emit(const std::string_view t) {
    ensurePart();
    partHasContent = true;
    if (pass1) return;
    out.append(t);
    if (out.size() >= OUT_FLUSH_BYTES) flushOut();
  }

  void emitInline(const std::string_view s) {
    if (pass1) {
      emit("");
      return;
    }
    tmp.clear();
    inl(s, tmp, false);
    emit(tmp);
  }

  // ---- front matter ----

  bool frontMatter(const std::string_view raw) {
    if (fmState == 2) return false;
    const std::string_view t = trim(raw);
    if (fmState == 0) {
      fmState = (t == "---") ? 1 : 2;
      return fmState == 1;
    }
    if (t == "---" || t == "...") {
      fmState = 2;
      return true;
    }
    const size_t colon = t.find(':');
    bool keyOk = colon != std::string_view::npos && colon > 0;
    for (size_t i = 0; keyOk && i < colon; i++) keyOk = isAlnum(t[i]) || t[i] == '_' || t[i] == '-';
    if (!keyOk && !t.empty() && t[0] != '-' && t[0] != '#') {
      fmState = 2;  // not YAML after all: render from here on
      return false;
    }
    if (keyOk && pass1) {
      std::string key(t.substr(0, colon));
      for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      std::string_view value = trim(t.substr(colon + 1));
      if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front()) {
        value = value.substr(1, value.size() - 2);
      }
      if (key == "title" && !value.empty()) sh.title = std::string(value);
      if ((key == "author" || key == "authors") && !value.empty()) sh.author = std::string(value);
    }
    return true;
  }

  // ---- block structure ----

  void expand(const std::string_view raw) {
    expanded.clear();
    for (const char c : raw) {
      if (c == '\t') {
        do {
          expanded += ' ';
        } while (expanded.size() % 4 != 0);
      } else {
        expanded += c;
      }
    }
  }

  static int atxLevel(const std::string_view r) {
    const size_t hashes = countRun(r, 0, '#');
    if (hashes < 1 || hashes > 6) return 0;
    if (hashes < r.size() && r[hashes] != ' ') return 0;
    return static_cast<int>(hashes);
  }

  static bool isThematicBreak(const std::string_view r) {
    if (r.empty() || (r[0] != '-' && r[0] != '*' && r[0] != '_')) return false;
    size_t count = 0;
    for (const char c : r) {
      if (c == r[0]) {
        count++;
      } else if (c != ' ') {
        return false;
      }
    }
    return count >= 3;
  }

  static bool fenceOpen(const std::string_view r, char& ch, size_t& len) {
    if (r.empty() || (r[0] != '`' && r[0] != '~')) return false;
    const size_t run = countRun(r, 0, r[0]);
    if (run < 3) return false;
    if (r[0] == '`' && r.substr(run).find('`') != std::string_view::npos) return false;
    ch = r[0];
    len = run;
    return true;
  }

  static bool parseListMarker(const std::string_view r, ListMarker& m) {
    size_t w = 0;
    if (!r.empty() && (r[0] == '-' || r[0] == '*' || r[0] == '+')) {
      m.ordered = false;
      m.marker = r[0];
      w = 1;
    } else {
      size_t digits = 0;
      while (digits < r.size() && digits < 9 && std::isdigit(static_cast<unsigned char>(r[digits]))) digits++;
      if (digits == 0 || digits >= r.size() || (r[digits] != '.' && r[digits] != ')')) return false;
      m.ordered = true;
      m.marker = r[digits];
      m.start = static_cast<unsigned>(strtoul(std::string(r.substr(0, digits)).c_str(), nullptr, 10));
      w = digits + 1;
    }
    if (w < r.size() && r[w] != ' ') return false;
    const size_t spaces = countSpaces(r, w);
    m.empty = (w + spaces >= r.size());
    m.width = w + ((m.empty || spaces > 4) ? std::min<size_t>(spaces, 1) : spaces);
    return true;
  }

  // inList: the line falls just outside an open list item, where a sibling item of any
  // number may start (only a list's first item must be "1" to interrupt a paragraph).
  bool startsNewBlock(const std::string_view rest, const bool inList = false) const {
    const size_t ind = countSpaces(rest);
    if (ind >= 4) return false;
    const std::string_view r = rest.substr(ind);
    char ch;
    size_t len;
    ListMarker m;
    return (!r.empty() && r[0] == '>') || atxLevel(r) > 0 || isThematicBreak(r) || fenceOpen(r, ch, len) ||
           (parseListMarker(r, m) && !m.empty && (!m.ordered || m.start == 1 || inList));
  }

  void resolveDangling(const size_t depth) {
    if (depth > MAX_DEPTH || !dangling[depth].active) return;
    emit(dangling[depth].ordered ? "</ol>\n" : "</ul>\n");
    dangling[depth].active = false;
  }

  void closeContainersFrom(const size_t k) {
    closeLeaf();
    while (stack.size() > k) {
      const size_t i = stack.size() - 1;
      resolveDangling(i + 1);
      const Container c = stack.back();
      stack.pop_back();
      if (c.quote) {
        emit("</blockquote>\n");
      } else {
        emit("</li>\n");
        dangling[i] = {true, c.ordered, c.marker};
      }
    }
  }

  void openQuote() {
    const size_t d = stack.size();
    resolveDangling(d);
    emit("<blockquote>\n");
    stack.push_back({true, false, 0, 0, false});
  }

  void openItem(const ListMarker& m, const size_t indent) {
    const size_t d = stack.size();
    Dangling& dl = dangling[d];
    if (dl.active && dl.ordered == m.ordered && dl.marker == m.marker) {
      dl.active = false;
    } else {
      resolveDangling(d);
      if (m.ordered) {
        if (m.start != 1) {
          char tag[32];
          snprintf(tag, sizeof(tag), "<ol start=\"%u\">\n", m.start);
          emit(tag);
        } else {
          emit("<ol>\n");
        }
      } else {
        emit("<ul>\n");
      }
    }
    emit("<li>");
    stack.push_back({false, m.ordered, m.marker, static_cast<uint16_t>(indent + m.width), false});
  }

  void process(const std::string_view line) {
    size_t pos = 0;
    size_t matched = 0;
    for (; matched < stack.size(); matched++) {
      const Container& c = stack[matched];
      if (c.quote) {
        const size_t ind = countSpaces(line, pos);
        if (ind <= 3 && pos + ind < line.size() && line[pos + ind] == '>') {
          pos += ind + 1;
          if (pos < line.size() && line[pos] == ' ') pos++;
          continue;
        }
        break;
      }
      if (isBlank(line.substr(pos))) continue;
      if (countSpaces(line, pos) >= c.contentIndent) {
        pos += c.contentIndent;
        continue;
      }
      break;
    }
    std::string_view rest = line.substr(pos);
    const bool restBlank = isBlank(rest);

    if (leaf == Leaf::Fence) {
      if (matched == stack.size()) {
        const size_t ind = countSpaces(rest);
        const std::string_view r = rest.substr(ind);
        if (ind <= 3 && countRun(r, 0, fenceChar) >= fenceLen && isBlank(r.substr(countRun(r, 0, fenceChar)))) {
          closeLeaf();
        } else {
          codeLine(rest.substr(std::min(ind, fenceIndent)));
        }
        return;
      }
      closeContainersFrom(matched);
    }

    if (matched < stack.size()) {
      if (leaf == Leaf::Para && !restBlank && !startsNewBlock(rest, !stack[matched].quote)) {
        appendParaLine(rest);
        return;
      }
      closeContainersFrom(matched);
    }

    if (leaf == Leaf::Indented) {
      if (restBlank) {
        pendingCodeBlanks++;
        return;
      }
      if (countSpaces(rest) >= 4) {
        for (; pendingCodeBlanks > 0; pendingCodeBlanks--) codeLine("");
        codeLine(rest.substr(4));
        return;
      }
      closeLeaf();
    }

    if (leaf == Leaf::Table) {
      if (!restBlank && !startsNewBlock(rest)) {
        tableRow(rest, "td");
        return;
      }
      closeLeaf();
    }

    while (stack.size() < MAX_DEPTH) {
      const size_t ind = countSpaces(rest);
      if (ind >= 4) break;
      const std::string_view r = rest.substr(ind);
      if (!r.empty() && r[0] == '>') {
        closeLeaf();
        openQuote();
        rest = r.substr(1);
        if (!rest.empty() && rest[0] == ' ') rest.remove_prefix(1);
        continue;
      }
      ListMarker m;
      if (!isThematicBreak(r) && parseListMarker(r, m) &&
          (leaf != Leaf::Para || (!m.empty && (!m.ordered || m.start == 1)))) {
        closeLeaf();
        openItem(m, ind);
        rest = r.substr(std::min(m.width, r.size()));
        continue;
      }
      break;
    }

    leafLine(rest);
  }

  void leafLine(const std::string_view rest) {
    if (isBlank(rest)) {
      if (leaf == Leaf::Para) closeLeaf();
      return;
    }
    const size_t ind = countSpaces(rest);
    if (ind >= 4 && leaf != Leaf::Para) {
      openLeaf(Leaf::Indented);
      codeLine(rest.substr(4));
      return;
    }
    const std::string_view r = rest.substr(std::min<size_t>(ind, 3));

    if (leaf == Leaf::Para && !paraOpened) {
      const std::string_view t = trim(r);
      if (!t.empty() && (t[0] == '=' || t[0] == '-') && countRun(t, 0, t[0]) == t.size()) {
        heading(t[0] == '=' ? 1 : 2, para);
        resetLeaf();
        return;
      }
      if (paraLines == 1 && para.find('|') != std::string::npos && isDelimiterRow(t) &&
          splitCells(para).size() == splitCells(t).size()) {
        const std::string header = para;
        resetLeaf();
        openLeaf(Leaf::Table);
        emit("<table>\n");
        tableCols = splitCells(header).size();
        tableRow(header, "th");
        return;
      }
    }

    if (const int level = atxLevel(r)) {
      closeLeaf();
      std::string_view text = trim(r.substr(static_cast<size_t>(level)));
      // Optional closing sequence: a run of '#' preceded by a space (or the whole text).
      size_t end = text.size();
      while (end > 0 && text[end - 1] == '#') end--;
      if (end == 0) {
        text = std::string_view();
      } else if (end < text.size() && text[end - 1] == ' ') {
        text = trim(text.substr(0, end));
      }
      openLeaf(Leaf::None);
      heading(level, text);
      return;
    }

    char ch;
    size_t len;
    if (fenceOpen(r, ch, len)) {
      closeLeaf();
      openLeaf(Leaf::Fence);
      fenceChar = ch;
      fenceLen = len;
      fenceIndent = std::min<size_t>(ind, 3);
      return;
    }

    if (isThematicBreak(r)) {
      closeLeaf();
      openLeaf(Leaf::None);
      emit("<hr/>\n");
      return;
    }

    if (leaf != Leaf::Para && r.size() > 3 && r[0] == '[') {
      const size_t close = r.find("]:");
      if (close != std::string_view::npos && close > 1) {
        if (r[1] == '^') {
          openLeaf(Leaf::Para);
          paraFootnote = std::string(r.substr(2, close - 2));
          appendParaLine(r.substr(close + 2));
          return;
        }
        std::string_view def = trim(r.substr(close + 2));
        if (!def.empty()) {
          if (pass1 && sh.refs.size() < MAX_REF_DEFS && !sh.findRef(r.substr(1, close - 1))) {
            if (def.front() == '<') def = def.substr(1, def.find('>') == std::string_view::npos ? 0 : def.find('>') - 1);
            sh.refs.push_back({normalizeLabel(r.substr(1, close - 1)),
                               unescapeBackslashes(def.substr(0, def.find(' ')))});
          }
          return;
        }
      }
    }

    if (leaf != Leaf::Para) openLeaf(Leaf::Para);
    appendParaLine(r);
  }

  void openLeaf(const Leaf kind) {
    resolveDangling(stack.size());
    leaf = kind;
    if (kind == Leaf::Fence || kind == Leaf::Indented) {
      emit(CODE_BLOCK_OPEN);
      codeFirstLine = true;
      pendingCodeBlanks = 0;
    }
  }

  void resetLeaf() {
    leaf = Leaf::None;
    para.clear();
    paraLines = 0;
    paraOpened = false;
    paraFootnote.clear();
  }

  void closeLeaf() {
    switch (leaf) {
      case Leaf::Para:
        closePara();
        break;
      case Leaf::Fence:
      case Leaf::Indented:
        emit("</div>\n");
        break;
      case Leaf::Table:
        emit("</table>\n");
        break;
      case Leaf::None:
        break;
    }
    resetLeaf();
  }

  // ---- leaves ----

  void appendParaLine(const std::string_view line) {
    std::string_view t = line;
    while (!t.empty() && t.front() == ' ') t.remove_prefix(1);
    if (!para.empty()) {
      // Two trailing spaces or a trailing backslash make a hard break.
      size_t trailing = 0;
      while (trailing < para.size() && para[para.size() - 1 - trailing] == ' ') trailing++;
      para.resize(para.size() - trailing);
      if (trailing >= 2) {
        para += HARD_BREAK;
      } else if (!para.empty() && para.back() == '\\') {
        para.back() = HARD_BREAK;
      } else {
        para += '\n';
      }
    }
    para.append(t);
    paraLines++;
    if (para.size() >= PARA_FLUSH_BYTES) {
      openPara();
      emitInline(para);
      para.clear();
    }
  }

  bool inItem() const { return !stack.empty() && !stack.back().quote; }

  void openPara() {
    if (paraOpened) return;
    paraOpened = true;
    if (!paraFootnote.empty()) {
      const std::string id = footnoteId(paraFootnote);
      std::string tag = "<p id=\"";
      appendEscapedAttr(tag, id);
      tag += "\"><b>";
      appendEscaped(tag, paraFootnote);
      tag += ".</b> ";
      emit(tag);
      if (pass1) sh.ids.emplace_back(fnv1a(id), part);
    } else if (inItem()) {
      if (stack.back().hasText) emit("<br/>");
    } else {
      emit("<p>");
    }
  }

  void closePara() {
    while (!para.empty() && (para.back() == ' ' || para.back() == '\\')) para.pop_back();
    openPara();
    emitInline(para);
    if (!paraFootnote.empty() || !inItem()) emit("</p>\n");
    if (inItem()) stack.back().hasText = true;
  }

  void codeLine(const std::string_view text) {
    std::string line;
    if (!codeFirstLine) line += "<br/>";
    codeFirstLine = false;
    if (text.empty()) {
      line += "&#160;";
    } else {
      SpaceState st;
      appendPlainText(line, text, st);
    }
    emit(line);
  }

  void heading(const int level, const std::string_view text) {
    std::string plain;
    inl(text, plain, true);
    std::string slug = slugify(trim(plain));
    if (slug.empty()) slug = "section";
    const uint16_t seen = slugCounts[fnv1a(slug)]++;
    if (seen > 0) slug += "-" + std::to_string(seen);

    std::string tag = "<h0 id=\"";
    tag[2] = static_cast<char>('0' + level);
    appendEscapedAttr(tag, slug);
    tag += "\">";
    emit(tag);
    emitInline(text);
    tag = "</h0>\n";
    tag[3] = static_cast<char>('0' + level);
    emit(tag);

    const std::string_view name = trim(plain);
    if (pass1) {
      sh.ids.emplace_back(fnv1a(slug), part);
      sh.minHeadingLevel = std::min<uint8_t>(sh.minHeadingLevel, static_cast<uint8_t>(level));
      if (level == 1 && sh.title.empty() && !name.empty()) sh.title = std::string(name);
      return;
    }
    const int tocLevel = level - sh.minHeadingLevel + 1;
    if (tocLevel <= 3 && !name.empty()) sink->addToc(static_cast<uint8_t>(tocLevel), std::string(name), part, slug);
  }

  static bool isDelimiterRow(const std::string_view t) {
    if (t.find('|') == std::string_view::npos || t.find('-') == std::string_view::npos) return false;
    for (const char c : t) {
      if (c != '|' && c != '-' && c != ':' && c != ' ') return false;
    }
    return true;
  }

  static std::vector<std::string_view> splitCells(std::string_view row) {
    row = trim(row);
    if (!row.empty() && row.front() == '|') row.remove_prefix(1);
    if (!row.empty() && row.back() == '|' && (row.size() < 2 || row[row.size() - 2] != '\\')) row.remove_suffix(1);
    std::vector<std::string_view> cells;
    cells.reserve(8);
    size_t start = 0;
    bool inCode = false;
    for (size_t i = 0; i < row.size(); i++) {
      if (row[i] == '\\') {
        i++;
      } else if (row[i] == '`') {
        inCode = !inCode;
      } else if (row[i] == '|' && !inCode) {
        cells.push_back(trim(row.substr(start, i - start)));
        start = i + 1;
      }
    }
    cells.push_back(trim(row.substr(start)));
    return cells;
  }

  void tableRow(const std::string_view row, const char* cellTag) {
    const auto cells = splitCells(row);
    std::string cell;
    emit("<tr>");
    for (size_t i = 0; i < tableCols; i++) {
      cell = "<";
      cell += cellTag;
      cell += ">";
      emit(cell);
      if (i < cells.size()) {
        // "\|" inside a cell is a literal pipe.
        std::string text;
        for (size_t k = 0; k < cells[i].size(); k++) {
          if (cells[i][k] == '\\' && k + 1 < cells[i].size() && cells[i][k + 1] == '|') continue;
          text += cells[i][k];
        }
        emitInline(text);
      }
      cell = "</";
      cell += cellTag;
      cell += ">";
      emit(cell);
    }
    emit("</tr>\n");
  }

  // ---- inline ----

  static bool isSpecial(const char c) {
    return c == '\\' || c == '\n' || c == HARD_BREAK || c == '`' || c == '!' || c == '[' || c == '<' || c == '*' ||
           c == '_' || c == '~' || c == '&';
  }

  static void text(std::string& o, const std::string_view t, const bool plain) {
    if (plain) {
      o.append(t);
    } else {
      appendEscaped(o, t);
    }
  }

  static size_t codeSpanEnd(const std::string_view s, const size_t from, const size_t run) {
    size_t k = from;
    while (k < s.size()) {
      if (s[k] != '`') {
        k++;
        continue;
      }
      const size_t r = countRun(s, k, '`');
      if (r == run) return k;
      k += r;
    }
    return std::string_view::npos;
  }

  // Skips a code span starting at k if it closes; returns the index after it, or k + 1.
  static size_t skipCode(const std::string_view s, const size_t k) {
    const size_t run = countRun(s, k, '`');
    const size_t end = codeSpanEnd(s, k + run, run);
    return end == std::string_view::npos ? k + run : end + run;
  }

  static size_t bracketClose(const std::string_view s, const size_t open) {
    int depth = 0;
    for (size_t k = open; k < s.size();) {
      const char c = s[k];
      if (c == '\\') {
        k += 2;
        continue;
      }
      if (c == '`') {
        k = skipCode(s, k);
        continue;
      }
      if (c == '[') depth++;
      if (c == ']' && --depth == 0) return k;
      k++;
    }
    return std::string_view::npos;
  }

  static size_t findCloser(const std::string_view s, const size_t from, const char c, const size_t want) {
    for (size_t k = from; k < s.size();) {
      if (s[k] == '\\') {
        k += 2;
        continue;
      }
      if (s[k] == '`') {
        k = skipCode(s, k);
        continue;
      }
      if (s[k] != c) {
        k++;
        continue;
      }
      const size_t r = countRun(s, k, c);
      const bool rightFlanking = k > from && !isSpace(s[k - 1]);
      const bool okEnd = c != '_' || k + r >= s.size() || !isAlnum(s[k + r]);
      if (r == want && rightFlanking && okEnd) return k;
      k += r;
    }
    return std::string_view::npos;
  }

  bool tryEmphasis(const std::string_view s, const size_t i, std::string& o, const bool plain, size_t& next) {
    const char c = s[i];
    const size_t run = countRun(s, i, c);
    if (i + run >= s.size() || isSpace(s[i + run])) return false;
    if (c == '~') {
      if (run != 2) return false;
    } else {
      if (run > 3) return false;
      if (c == '_' && i > 0 && isAlnum(s[i - 1])) return false;
    }
    const size_t close = findCloser(s, i + run, c, run);
    if (close == std::string_view::npos) return false;
    const std::string_view inner = s.substr(i + run, close - i - run);
    const char* openTag = c == '~' ? "<del>" : run == 1 ? "<i>" : run == 2 ? "<b>" : "<b><i>";
    const char* closeTag = c == '~' ? "</del>" : run == 1 ? "</i>" : run == 2 ? "</b>" : "</i></b>";
    if (!plain) o += openTag;
    inl(inner, o, plain);
    if (!plain) o += closeTag;
    next = close + run;
    return true;
  }

  static bool parseDest(const std::string_view s, size_t p, std::string& dest, size_t& end) {
    while (p < s.size() && isSpace(s[p])) p++;
    size_t start;
    size_t stop;
    if (p < s.size() && s[p] == '<') {
      start = p + 1;
      const size_t gt = s.find('>', start);
      if (gt == std::string_view::npos) return false;
      stop = gt;
      p = gt + 1;
    } else {
      start = p;
      int depth = 0;
      while (p < s.size()) {
        const char c = s[p];
        if (c == '\\' && p + 1 < s.size()) {
          p += 2;
          continue;
        }
        if (isSpace(c)) break;
        if (c == '(') depth++;
        if (c == ')') {
          if (depth == 0) break;
          depth--;
        }
        p++;
      }
      stop = p;
    }
    while (p < s.size() && isSpace(s[p])) p++;
    if (p < s.size() && (s[p] == '"' || s[p] == '\'' || s[p] == '(')) {
      const char closeCh = s[p] == '(' ? ')' : s[p];
      const size_t q = s.find(closeCh, p + 1);
      if (q == std::string_view::npos) return false;
      p = q + 1;
      while (p < s.size() && isSpace(s[p])) p++;
    }
    if (p >= s.size() || s[p] != ')') return false;
    dest = unescapeBackslashes(s.substr(start, stop - start));
    end = p + 1;
    return true;
  }

  // "#slug" -> "pNNNN.xhtml#slug" for an id the book defines; anything else is not followable.
  bool internalHref(const std::string_view dest, std::string& href) const {
    if (dest.size() < 2 || dest[0] != '#') return false;
    std::string id = percentDecode(dest.substr(1));
    int p = sh.partOf(id);
    if (p < 0) {
      id = slugify(id);
      p = sh.partOf(id);
    }
    if (p < 0) return false;
    href = partHref(static_cast<uint16_t>(p));
    href += '#';
    href += id;
    return true;
  }

  void renderLink(const std::string_view label, const std::string_view dest, const bool image, std::string& o,
                  const bool plain) {
    if (image) {
      std::string alt;
      inl(label, alt, true);
      if (plain || dest.empty() || isExternalUrl(dest)) {
        text(o, alt, plain);
        return;
      }
      std::string src;
      for (const char c : resolveSdPath(docDir, percentDecode(dest))) {
        if (c == '%') {
          src += "%25";
        } else {
          src += c;
        }
      }
      o += "<img src=\"";
      appendEscapedAttr(o, src);
      o += "\" alt=\"";
      appendEscapedAttr(o, alt);
      o += "\"/>";
      return;
    }
    std::string href;
    if (plain || !internalHref(dest, href)) {
      inl(label, o, plain);
      return;
    }
    o += "<a href=\"";
    appendEscapedAttr(o, href);
    o += "\">";
    inl(label, o, false);
    o += "</a>";
  }

  bool tryLink(const std::string_view s, const size_t open, std::string& o, const bool plain, const bool image,
               size_t& next) {
    if (!image && open + 1 < s.size() && s[open + 1] == '^') {
      const size_t close = s.find(']', open + 2);
      if (close == std::string_view::npos || close == open + 2) return false;
      const std::string_view label = s.substr(open + 2, close - open - 2);
      if (label.find(' ') != std::string_view::npos) return false;
      const std::string id = footnoteId(label);
      const int p = sh.partOf(id);
      if (plain || p < 0) {
        text(o, s.substr(open, close - open + 1), plain);
      } else {
        o += "<sup><a href=\"";
        appendEscapedAttr(o, partHref(static_cast<uint16_t>(p)) + "#" + id);
        o += "\">";
        appendEscaped(o, label);
        o += "</a></sup>";
      }
      next = close + 1;
      return true;
    }

    const size_t close = bracketClose(s, open);
    if (close == std::string_view::npos) return false;
    const std::string_view label = s.substr(open + 1, close - open - 1);

    if (close + 1 < s.size() && s[close + 1] == '(') {
      std::string dest;
      size_t end;
      if (parseDest(s, close + 2, dest, end)) {
        renderLink(label, dest, image, o, plain);
        next = end;
        return true;
      }
    }
    if (close + 1 < s.size() && s[close + 1] == '[') {
      const size_t refClose = s.find(']', close + 2);
      if (refClose != std::string_view::npos) {
        const std::string_view ref = s.substr(close + 2, refClose - close - 2);
        if (const auto* def = sh.findRef(ref.empty() ? label : ref)) {
          renderLink(label, def->url, image, o, plain);
          next = refClose + 1;
          return true;
        }
        return false;
      }
    }
    if (const auto* def = sh.findRef(label)) {
      renderLink(label, def->url, image, o, plain);
      next = close + 1;
      return true;
    }
    return false;
  }

  static bool tryAutolink(const std::string_view s, const size_t open, std::string& o, const bool plain,
                          size_t& next) {
    const size_t close = s.find('>', open + 1);
    if (close == std::string_view::npos || close == open + 1) return false;
    const std::string_view inner = s.substr(open + 1, close - open - 1);
    for (const char c : inner) {
      if (isSpace(c) || c == '<') return false;
    }
    if (!isExternalUrl(inner) && inner.find('@') == std::string_view::npos) return false;
    text(o, inner, plain);
    next = close + 1;
    return true;
  }

  static bool tryEntity(const std::string_view s, const size_t amp, std::string& o, size_t& next) {
    const size_t semi = s.find(';', amp + 1);
    if (semi == std::string_view::npos || semi - amp > 12 || semi == amp + 1) return false;
    const std::string_view body = s.substr(amp + 1, semi - amp - 1);
    bool valid;
    if (body[0] == '#') {
      const bool hex = body.size() > 1 && (body[1] == 'x' || body[1] == 'X');
      const size_t digitsFrom = hex ? 2 : 1;
      valid = body.size() > digitsFrom;
      for (size_t k = digitsFrom; valid && k < body.size(); k++) {
        valid = hex ? std::isxdigit(static_cast<unsigned char>(body[k])) != 0
                    : std::isdigit(static_cast<unsigned char>(body[k])) != 0;
      }
      if (!valid) return false;
      o.append(s.substr(amp, semi - amp + 1));
    } else if (body == "amp" || body == "lt" || body == "gt" || body == "quot" || body == "apos") {
      o.append(s.substr(amp, semi - amp + 1));
    } else {
      const NamedEntity* hit = nullptr;
      for (const auto& e : NAMED_ENTITIES) {
        if (body == e.name) hit = &e;
      }
      if (!hit) return false;
      o += hit->utf8;
    }
    next = semi + 1;
    return true;
  }

  void inl(const std::string_view s, std::string& o, const bool plain) {
    size_t i = 0;
    while (i < s.size()) {
      const char c = s[i];
      size_t next = i;
      if (c == '\\' && i + 1 < s.size() && isAsciiPunct(s[i + 1])) {
        text(o, s.substr(i + 1, 1), plain);
        i += 2;
      } else if (c == '\n') {
        o += ' ';
        i++;
      } else if (c == HARD_BREAK) {
        o += plain ? " " : "<br/>";
        i++;
      } else if (c == '`') {
        const size_t run = countRun(s, i, '`');
        const size_t end = codeSpanEnd(s, i + run, run);
        if (end == std::string_view::npos) {
          text(o, s.substr(i, run), plain);
          i += run;
          continue;
        }
        std::string code(s.substr(i + run, end - i - run));
        for (auto& ch : code) {
          if (ch == '\n' || ch == HARD_BREAK) ch = ' ';
        }
        if (code.size() >= 2 && code.front() == ' ' && code.back() == ' ') code = code.substr(1, code.size() - 2);
        if (!plain) o += "<code>";
        text(o, code, plain);
        if (!plain) o += "</code>";
        i = end + run;
      } else if (c == '!' && i + 1 < s.size() && s[i + 1] == '[' && tryLink(s, i + 1, o, plain, true, next)) {
        i = next;
      } else if (c == '[' && tryLink(s, i, o, plain, false, next)) {
        i = next;
      } else if (c == '<' && tryAutolink(s, i, o, plain, next)) {
        i = next;
      } else if ((c == '*' || c == '_' || c == '~') && tryEmphasis(s, i, o, plain, next)) {
        i = next;
      } else if (c == '&' && !plain && tryEntity(s, i, o, next)) {
        i = next;
      } else if (c == '*' || c == '_' || c == '~') {
        const size_t run = countRun(s, i, c);
        text(o, s.substr(i, run), plain);
        i += run;
      } else {
        size_t j = i + 1;
        while (j < s.size() && !isSpecial(s[j])) j++;
        text(o, s.substr(i, j - i), plain);
        i = j;
      }
    }
  }
};

class NullSink final : public Sink {
 public:
  bool beginPart(uint16_t) override { return true; }
  bool write(const char*, size_t) override { return true; }
  bool endPart() override { return true; }
  void addToc(uint8_t, const std::string&, uint16_t, const std::string&) override {}
};

bool runPass(Source& src, Converter& conv) {
  LineReader reader(src);
  if (!reader.ok()) return false;
  std::string line;
  line.reserve(256);
  bool partial = false;
  size_t bytes = 0;
  while (reader.next(line, partial, bytes)) conv.line(line, bytes);
  return !reader.failed();
}

}  // namespace

Result convertMarkdown(Source& src, Sink& sink, const std::string_view fallbackTitle, const std::string_view docDir) {
  Result result;
  Shared sh;
  sh.ids.reserve(64);

  {
    Converter scan(nullptr, sh, docDir, "");
    if (!runPass(src, scan) || !scan.finish()) return result;
  }
  std::sort(sh.ids.begin(), sh.ids.end());
  if (sh.title.empty()) sh.title = std::string(fallbackTitle);
  if (!src.rewind()) return result;

  Converter conv(&sink, sh, docDir, sh.title);
  if (!runPass(src, conv) || !conv.finish()) return result;

  result.ok = true;
  result.parts = conv.parts();
  result.title = sh.title;
  result.author = sh.author;
  return result;
}

}  // namespace textbook
