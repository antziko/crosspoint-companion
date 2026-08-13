#include "DictHtmlRenderer.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// Tag classification
// ---------------------------------------------------------------------------

DictHtmlRenderer::TagAction DictHtmlRenderer::classify(const XML_Char* name) {
  // Block-strip: entire subtree discarded
  if (strcmp(name, "svg") == 0) return TagAction::BLOCK_STRIP;
  if (strcmp(name, "hiero") == 0) return TagAction::BLOCK_STRIP;
  if (strcmp(name, "math") == 0) return TagAction::BLOCK_STRIP;
  if (strcmp(name, "gallery") == 0) return TagAction::BLOCK_STRIP;
  if (strcmp(name, "nowiki") == 0) return TagAction::BLOCK_STRIP;
  if (strcmp(name, "poem") == 0) return TagAction::BLOCK_STRIP;
  if (strcmp(name, "ref") == 0) return TagAction::BLOCK_STRIP;
  if (strcmp(name, "REF") == 0) return TagAction::BLOCK_STRIP;
  if (strcmp(name, "img") == 0) return TagAction::BLOCK_STRIP;

  // Children of block-strip tags (registered to avoid unknown-tag errors)
  if (strcmp(name, "defs") == 0) return TagAction::REGISTERED;
  if (strcmp(name, "g") == 0) return TagAction::REGISTERED;
  if (strcmp(name, "path") == 0) return TagAction::REGISTERED;
  if (strcmp(name, "rect") == 0) return TagAction::REGISTERED;
  if (strcmp(name, "use") == 0) return TagAction::REGISTERED;
  if (strcmp(name, "table") == 0) return TagAction::REGISTERED;
  if (strcmp(name, "tr") == 0) return TagAction::REGISTERED;
  if (strcmp(name, "td") == 0) return TagAction::REGISTERED;

  // Inline formatting
  if (strcmp(name, "b") == 0) return TagAction::FORMAT_BOLD;
  if (strcmp(name, "strong") == 0) return TagAction::FORMAT_BOLD;
  if (strcmp(name, "i") == 0) return TagAction::FORMAT_ITALIC;
  if (strcmp(name, "em") == 0) return TagAction::FORMAT_ITALIC;
  if (strcmp(name, "u") == 0) return TagAction::FORMAT_UNDERLINE;
  if (strcmp(name, "s") == 0) return TagAction::FORMAT_STRIKE;
  if (strcmp(name, "sup") == 0) return TagAction::FORMAT_SMALL;
  if (strcmp(name, "sub") == 0) return TagAction::FORMAT_SMALL;
  if (strcmp(name, "code") == 0) return TagAction::FORMAT_CODE;
  if (strcmp(name, "tt") == 0) return TagAction::FORMAT_CODE;
  if (strcmp(name, "small") == 0) return TagAction::FORMAT_SMALL;
  if (strcmp(name, "big") == 0) return TagAction::FORMAT_SMALL;

  // Block structure
  if (strcmp(name, "p") == 0) return TagAction::BLOCK_BREAK;
  if (strcmp(name, "div") == 0) return TagAction::BLOCK_BREAK;
  if (strcmp(name, "br") == 0) return TagAction::BLOCK_BREAK;
  if (strcmp(name, "blockquote") == 0) return TagAction::BLOCK_QUOTE;

  // Lists
  if (strcmp(name, "li") == 0) return TagAction::LIST_ITEM;
  if (strcmp(name, "ol") == 0) return TagAction::LIST_CONTAINER;
  if (strcmp(name, "ul") == 0) return TagAction::LIST_CONTAINER;

  // Headings
  if (strcmp(name, "h1") == 0) return TagAction::HEADING;
  if (strcmp(name, "h2") == 0) return TagAction::HEADING;
  if (strcmp(name, "h3") == 0) return TagAction::HEADING;
  if (strcmp(name, "h4") == 0) return TagAction::HEADING;

  // Special
  if (strcmp(name, "abbr") == 0) return TagAction::ABBR;
  if (strcmp(name, "var") == 0) return TagAction::VAR;
  if (strcmp(name, "span") == 0) return TagAction::SPAN;
  if (strcmp(name, "a") == 0) return TagAction::SPAN;            // Registered; no actual anchors in dict
  if (strcmp(name, "_root") == 0) return TagAction::REGISTERED;  // Synthetic wrapper used by render()

  // XDXF (sametypesequence=x). Registered unconditionally: none of these names collide with
  // HTML ("abr" is XDXF, "abbr" is HTML), so one table serves both formats. They would already
  // fall through to STRIP_KEEP below and render acceptably, but they are listed explicitly
  // because the host test builds with DICT_HTML_RENDERER_TRACK_UNKNOWN and fails on unexpected
  // unknown tags — and because <k> and <abr> want more than the default.
  //
  // XDXF shares <b>/<i>/<u>/<sub>/<sup>/<blockquote> with HTML, so those are already handled
  // above and come out styled. <tr> is XDXF's transcription but HTML's table row; it is left as
  // REGISTERED above, which keeps the text either way.
  if (strcmp(name, "k") == 0) return TagAction::BLOCK_STRIP;      // headword — already in the title bar
  if (strcmp(name, "abr") == 0) return TagAction::FORMAT_ITALIC;  // "UK", "US", "noun"
  if (strcmp(name, "ex") == 0) return TagAction::FORMAT_ITALIC;   // usage example
  if (strcmp(name, "nu") == 0) return TagAction::BLOCK_BREAK;
  if (strcmp(name, "c") == 0) return TagAction::REGISTERED;  // colour: meaningless on mono e-ink
  if (strcmp(name, "ar") == 0) return TagAction::REGISTERED;
  if (strcmp(name, "xdxf") == 0) return TagAction::REGISTERED;
  if (strcmp(name, "dtrn") == 0) return TagAction::REGISTERED;
  if (strcmp(name, "co") == 0) return TagAction::REGISTERED;
  if (strcmp(name, "def") == 0) return TagAction::REGISTERED;
  if (strcmp(name, "gr") == 0) return TagAction::REGISTERED;
  if (strcmp(name, "opt") == 0) return TagAction::REGISTERED;
  if (strcmp(name, "kref") == 0) return TagAction::REGISTERED;  // cross-refs: keep the visible text
  if (strcmp(name, "iref") == 0) return TagAction::REGISTERED;
  if (strcmp(name, "rref") == 0) return TagAction::REGISTERED;

  // Wikitext annotation tags: t:XX, tr:XX, lang:XX, gloss:XX, pos:XX, sc:XX, alt:XX, id:XX
  if (strncmp(name, "t:", 2) == 0) return TagAction::WIKI_ANNOT;
  if (strncmp(name, "tr:", 3) == 0) return TagAction::WIKI_ANNOT;
  if (strncmp(name, "lang:", 5) == 0) return TagAction::WIKI_ANNOT;
  if (strncmp(name, "gloss:", 6) == 0) return TagAction::WIKI_ANNOT;
  if (strncmp(name, "pos:", 4) == 0) return TagAction::WIKI_ANNOT;
  if (strncmp(name, "sc:", 3) == 0) return TagAction::WIKI_ANNOT;
  if (strncmp(name, "alt:", 4) == 0) return TagAction::WIKI_ANNOT;
  if (strncmp(name, "id:", 3) == 0) return TagAction::WIKI_ANNOT;

  return TagAction::STRIP_KEEP;  // Unknown — strip tag, keep text
}

const char* DictHtmlRenderer::findAttr(const XML_Char** atts, const char* name) {
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], name) == 0) return atts[i + 1];
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Constructor / destructor / reset
// ---------------------------------------------------------------------------

DictHtmlRenderer::DictHtmlRenderer() {
  // spans/textBuf are deliberately NOT reserved here — see ensureBatchBuffers(). tagStack is
  // used by both modes and is small, so it stays.
  tagStack.reserve(8);
}

// Reserve the batch-mode accumulators, on first batch use rather than at construction.
//
// Only batch rendering (render / renderFromFile) fills these; the streaming path hands each
// span straight to its sink and never touches them — pushSpan's sink branch, and the note at
// the end of renderFromFileStreaming. Reserving them in the constructor therefore charged
// every DictHtmlRenderer ~1536 bytes it might never read, and charged it through the THROWING
// operator new, which abort()s instead of failing. The definition viewer owns one of these as
// a member and streams exclusively, on a heap device logs show at single-digit KB — so this
// was 1.5 KB taken from, and a reboot risk imposed on, the one screen that wanted neither.
//
// clear() keeps capacity, so this is a no-op after the first batch render.
void DictHtmlRenderer::ensureBatchBuffers() {
  if (spans.capacity() == 0) spans.reserve(64);
  if (textBuf.capacity() == 0) textBuf.reserve(512);
}

DictHtmlRenderer::~DictHtmlRenderer() { releaseParser(); }

void DictHtmlRenderer::releaseParser() {
  if (parser) {
    XML_ParserFree(parser);
    parser = nullptr;
  }
}

void DictHtmlRenderer::reset() {
  spans.clear();
  textBuf.clear();
  pendingText.clear();
  tagStack.clear();
  parseError = false;
  fmt = FormatState{};
  newlinePending = false;
  listItemPending = false;
  krefArrowPending_ = false;
  spanSink_ = SpanSink{};  // batch mode by default; streaming sets it after reset()

  // Reuse the existing parser (cheap reset) rather than free+create on every
  // render. Only the first call (null parser) allocates. XML_ParserReset clears
  // handlers + user data; callers re-apply them after reset (parseOpenFile,
  // render). Pays off because the renderer is now a reused activity member.
  if (parser) {
    XML_ParserReset(parser, nullptr);
  } else {
    parser = XML_ParserCreate(nullptr);
  }

#ifdef DICT_HTML_RENDERER_TRACK_UNKNOWN
  unknownTagCount = 0;
#endif
}

// ---------------------------------------------------------------------------
// Text management
// ---------------------------------------------------------------------------

void DictHtmlRenderer::pushSpan() {
  if (pendingText.empty()) return;

  StyledSpan span;
  span.bold = fmt.bold;
  span.italic = fmt.italic;
  span.underline = fmt.underline;
  span.strikethrough = fmt.strikethrough;
  span.indentLevel = fmt.indentLevel;
  span.newlineBefore = newlinePending;
  span.isListItem = listItemPending;

  if (spanSink_.fn) {
    // Streaming: deliver the span immediately. text points into pendingText and
    // is valid only for the duration of the call — the sink copies it. textBuf
    // and the spans vector are never grown.
    span.text = pendingText.c_str();
    span.textOffset = 0;
    spanSink_(span);
  } else {
    // Batch: accumulate text into textBuf; span.text pointers fixed up after the
    // full parse (textBuf reallocates as it grows).
    span.textOffset = static_cast<uint32_t>(textBuf.size());
    if (textBuf.size() + pendingText.size() + 1 > textBuf.capacity()) {
      textBuf.reserve(textBuf.capacity() + 512);
    }
    textBuf.insert(textBuf.end(), pendingText.begin(), pendingText.end());
    textBuf.push_back('\0');
    spans.push_back(span);
  }

  pendingText.clear();
  newlinePending = false;
  listItemPending = false;
}

// Codepoints that no SD-card font subset carries, mapped to a near-equivalent every font has.
// Unlike the kref arrow these are CONTENT and must not be dropped — the ✗ in particular is the
// only thing marking an example sentence as incorrect, and the dictionary has no ✓ counterpart,
// so losing it inverts the meaning. Left alone they reach the screen as EpdFont's U+FFFD
// replacement glyph, i.e. a bare "?" (EpdFont.cpp:224).
//
// Mapped here rather than at the point of the glyph miss so that measuring, prewarm and drawing
// all see the same character: a font-layer substitution would have to keep EpdFont::getGlyph,
// SdCardFont's advance-table replacementIdx (SdCardFont.cpp:1293) and the prewarm scan in
// agreement, and disagreeing lays out widths for a glyph that is never drawn. The cost is that a
// font which genuinely has ✗ still gets ×; dingbats only arrive with the `symbols`/`reading`
// interval presets, which no text-font subset uses.
//
// Caller has already matched the 0xE2 lead byte, so this is only reached for U+2000-U+2FFF.
static const char* substituteUndrawable(const unsigned char b1, const unsigned char b2) {
  if (b1 == 0x96 && b2 == 0xAA) return "\xC2\xB7";  // U+25AA ▪ -> U+00B7 ·  idiom separator
  if (b1 == 0xB1 && b2 == 0xB5) return "|-";        // U+2C75 Ⱶ -> "|-"      tree tee, see below
  if (b1 == 0x9C && b2 == 0x97) return "\xC3\x97";  // U+2717 ✗ -> U+00D7 ×  "incorrect example"
  return nullptr;
}
// Ⱶ is half of a pair: the Thesaurus outline uses it for a middle child and a plain ASCII 'L'
// for the last one — the converter's lookalikes for ├ and └. All 222 runs of Ⱶ lines in the
// Cambridge dictionary are closed by an L line. 'L' is an ordinary letter and cannot be
// distinguished from real text, so it stays; "|-" is chosen to still read as a tree beside it.

void DictHtmlRenderer::emitText(const char* s, int len) {
  if (len <= 0) return;

  // First text inside a <kref>: drop a leading U+2191 (E2 86 91) — see krefArrowPending_.
  // Cleared unconditionally, so only the leading position is ever considered; expat never
  // splits a codepoint across callbacks, so the three bytes always arrive together.
  if (krefArrowPending_) {
    krefArrowPending_ = false;
    if (len >= 3 && static_cast<unsigned char>(s[0]) == 0xE2 && static_cast<unsigned char>(s[1]) == 0x86 &&
        static_cast<unsigned char>(s[2]) == 0x91) {
      s += 3;
      len -= 3;
      if (len <= 0) return;
    }
  }

  for (int i = 0; i < len; i++) {
    // One comparison guards the whole substitution table: every codepoint in it is U+2000-U+2FFF,
    // so every UTF-8 encoding starts 0xE2.
    if (static_cast<unsigned char>(s[i]) == 0xE2 && i + 2 < len) {
      if (const char* sub =
              substituteUndrawable(static_cast<unsigned char>(s[i + 1]), static_cast<unsigned char>(s[i + 2]))) {
        pendingText += sub;
        i += 2;
        continue;
      }
    }

    char c = s[i];
    if (c == '\r' || c == '\n') {
      pushSpan();
      newlinePending = true;
      // Treat \r\n as a single line ending
      if (c == '\r' && i + 1 < len && s[i + 1] == '\n') i++;
      continue;
    }
    if (c == '\t') c = ' ';
    pendingText += c;
  }
}

void DictHtmlRenderer::flushPending() { pushSpan(); }

// ---------------------------------------------------------------------------
// Expat callbacks
// ---------------------------------------------------------------------------

void XMLCALL DictHtmlRenderer::onStart(void* ud, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<DictHtmlRenderer*>(ud);
  if (self->parseError) return;

  TagAction action = classify(name);

  // Propagate suppression from parent
  bool parentSuppresses = !self->tagStack.empty() && self->tagStack.back().suppressChildren;

  StackEntry entry{};
  entry.action = action;
  entry.savedFmt = self->fmt;
  entry.suppressChildren = parentSuppresses;
#ifdef DICT_HTML_RENDERER_TRACK_UNKNOWN
  entry.pendingLenAtOpen = static_cast<int>(self->pendingText.size());
  entry.unknownTagName[0] = '\0';
#endif

  if (!parentSuppresses) {
    switch (action) {
      case TagAction::BLOCK_STRIP:
        self->flushPending();
        entry.suppressChildren = true;
        break;

      case TagAction::FORMAT_BOLD:
        self->flushPending();
        self->fmt.bold = true;
        break;

      case TagAction::FORMAT_ITALIC:
      case TagAction::VAR:
        self->flushPending();
        self->fmt.italic = true;
        break;

      case TagAction::FORMAT_UNDERLINE:
        self->flushPending();
        self->fmt.underline = true;
        break;

      case TagAction::FORMAT_STRIKE:
        self->flushPending();
        self->fmt.strikethrough = true;
        break;

      case TagAction::FORMAT_SMALL:
        self->flushPending();
        break;

      case TagAction::FORMAT_CODE:
        self->flushPending();
        self->fmt.bold = true;
        break;

      case TagAction::BLOCK_BREAK:
        self->flushPending();
        self->newlinePending = true;
        break;

      case TagAction::BLOCK_QUOTE:
        self->flushPending();
        self->newlinePending = true;
        self->fmt.indentLevel++;
        break;

      case TagAction::LIST_CONTAINER:
        self->flushPending();
        self->newlinePending = true;
        break;

      case TagAction::LIST_ITEM:
        self->flushPending();
        self->newlinePending = true;
        self->listItemPending = true;
        self->fmt.indentLevel++;
        break;

      case TagAction::HEADING:
        self->flushPending();
        self->newlinePending = true;
        self->fmt.bold = true;
        break;

      case TagAction::ABBR: {
        self->flushPending();  // Flush preceding text before body accumulates
        const char* title = findAttr(atts, "title");
        if (title) {
          entry.abbrTitle = title;
        }
        break;
      }

      case TagAction::WIKI_ANNOT: {
        const char* colon = strchr(name, ':');
        if (colon && colon[1] != '\0') {
          entry.wikiText = colon + 1;
        }
        break;
      }

      case TagAction::STRIP_KEEP:
#ifdef DICT_HTML_RENDERER_TRACK_UNKNOWN
        strncpy(entry.unknownTagName, name, sizeof(entry.unknownTagName) - 1);
        entry.unknownTagName[sizeof(entry.unknownTagName) - 1] = '\0';
        entry.pendingLenAtOpen = static_cast<int>(self->pendingText.size());
#endif
        break;

      default:
        break;
    }

    // Arm the leading-arrow strip. Not a switch case: classify() folds kref in with c, dtrn,
    // iref, rref and the rest of TagAction::REGISTERED, so only the name distinguishes it.
    // Deliberately not cleared by intervening tags — <kref><c>↑Foo</c></kref> must strip too.
    if (strcmp(name, "kref") == 0) {
      self->krefArrowPending_ = true;
    }
  }

  self->tagStack.push_back(entry);
}

void XMLCALL DictHtmlRenderer::onEnd(void* ud, const XML_Char* name) {
  auto* self = static_cast<DictHtmlRenderer*>(ud);
  if (self->parseError) return;

  // Disarm on close so an empty <kref></kref> cannot eat an arrow that belongs to the text
  // after it. Done before the tagStack guard: the flag is parser state, not stack state.
  if (strcmp(name, "kref") == 0) {
    self->krefArrowPending_ = false;
  }

  if (self->tagStack.empty()) return;

  StackEntry& entry = self->tagStack.back();
  bool suppressed = entry.suppressChildren || entry.action == TagAction::BLOCK_STRIP;

  if (!suppressed) {
    switch (entry.action) {
      case TagAction::FORMAT_BOLD:
      case TagAction::FORMAT_ITALIC:
      case TagAction::FORMAT_UNDERLINE:
      case TagAction::FORMAT_STRIKE:
      case TagAction::FORMAT_SMALL:
      case TagAction::FORMAT_CODE:
      case TagAction::VAR:
      case TagAction::SPAN:
      case TagAction::REGISTERED:
        self->flushPending();
        break;

      case TagAction::BLOCK_BREAK:
        self->flushPending();
        self->newlinePending = true;
        break;

      case TagAction::BLOCK_QUOTE:
        self->flushPending();
        self->newlinePending = true;
        break;

      case TagAction::LIST_CONTAINER:
        self->flushPending();
        self->newlinePending = true;
        break;

      case TagAction::LIST_ITEM:
        self->flushPending();
        break;

      case TagAction::HEADING:
        self->flushPending();
        self->newlinePending = true;
        break;

      case TagAction::ABBR:
        if (!entry.abbrTitle.empty()) {
          self->flushPending();
          std::string expanded = " (" + entry.abbrTitle + ")";
          self->emitText(expanded.c_str(), static_cast<int>(expanded.size()));
          self->flushPending();
        }
        break;

      case TagAction::WIKI_ANNOT:
        if (!entry.wikiText.empty()) {
          self->flushPending();  // Flush preceding text before emitting wiki suffix
          self->emitText(entry.wikiText.c_str(), static_cast<int>(entry.wikiText.size()));
          self->flushPending();
        }
        break;

      case TagAction::STRIP_KEEP:
#ifdef DICT_HTML_RENDERER_TRACK_UNKNOWN
        if (entry.unknownTagName[0] != '\0') {
          // Extract word before (from text accumulated before tag opened)
          char wordBefore[32] = {};
          // We can't easily recover the text before — use empty for now
          // Extract tag contents: text accumulated between open and close
          char tagContents[64] = {};
          int contStart = entry.pendingLenAtOpen;
          int contLen = static_cast<int>(self->pendingText.size()) - contStart;
          if (contLen > 0 && contLen < static_cast<int>(sizeof(tagContents))) {
            memcpy(tagContents, self->pendingText.c_str() + contStart, contLen);
            tagContents[contLen] = '\0';
          }
          self->recordUnknownTag(entry.unknownTagName, wordBefore, tagContents, "");
        }
#endif
        self->flushPending();
        break;

      default:
        break;
    }

    // Restore saved format (bold/italic/underline/strikethrough/indentLevel)
    self->fmt = entry.savedFmt;
  }

  self->tagStack.pop_back();
}

void XMLCALL DictHtmlRenderer::onText(void* ud, const XML_Char* s, int len) {
  auto* self = static_cast<DictHtmlRenderer*>(ud);
  if (self->parseError) return;

  if (!self->tagStack.empty()) {
    const StackEntry& top = self->tagStack.back();
    // Suppress text inside block-strip and for wiki-annot (emit from tag name, not body)
    if (top.suppressChildren || top.action == TagAction::WIKI_ANNOT || top.action == TagAction::BLOCK_STRIP) {
      return;
    }
  }

  self->emitText(s, len);
}

// ---------------------------------------------------------------------------
// HTML entity resolver — feeds definition to expat incrementally
// ---------------------------------------------------------------------------

// Returns the UTF-8 replacement for a known HTML named entity (matched by
// in-place strncmp — no copy, no stack buffer). Returns nullptr for unknowns.
// Empty string ("") means the entity should be silently dropped (zero-width chars).
static const char* lookupHtmlEntity(const char* name, int nameLen) {
  struct E {
    const char* n;
    int nLen;
    const char* utf8;
  };
  static constexpr E kTable[] = {
      {"lsqb", 4, "["},
      {"rsqb", 4, "]"},
      {"nbsp", 4, " "},
      {"ndash", 5, "\xE2\x80\x93"},   // –
      {"mdash", 5, "\xE2\x80\x94"},   // —
      {"hellip", 6, "\xE2\x80\xA6"},  // …
      {"bull", 4, "\xE2\x80\xA2"},    // •
      {"dagger", 6, "\xE2\x80\xA0"},  // †
      {"sect", 4, "\xC2\xA7"},        // §
      {"para", 4, "\xC2\xB6"},        // ¶
      {"prime", 5, "\xE2\x80\xB2"},   // ′
      {"Prime", 5, "\xE2\x80\xB3"},   // ″
      {"times", 5, "\xC3\x97"},       // ×
      {"minus", 5, "\xE2\x88\x92"},   // −
      {"middot", 6, "\xC2\xB7"},      // ·
      {"lrm", 3, ""},                 // LEFT-TO-RIGHT MARK — zero-width, drop
      {"rlm", 3, ""},                 // RIGHT-TO-LEFT MARK — zero-width, drop
  };
  for (const auto& e : kTable) {
    if (e.nLen == nameLen && strncmp(name, e.n, nameLen) == 0) return e.utf8;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Public render()
// ---------------------------------------------------------------------------

const std::vector<StyledSpan>& DictHtmlRenderer::render(const char* html, int len) {
  reset();
  ensureBatchBuffers();

  if (!parser) {
    parseError = true;
    return spans;
  }

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, onStart, onEnd);
  XML_SetCharacterDataHandler(parser, onText);

  // Wrap fragment in a synthetic root element so expat sees well-formed XML
  static constexpr const char kOpen[] = "<_root>";
  static constexpr const char kClose[] = "</_root>";

  if (XML_Parse(parser, kOpen, static_cast<int>(sizeof(kOpen) - 1), 0) != XML_STATUS_OK) {
    parseError = true;
  }

  // Walk the definition, feeding normal text runs directly to expat and
  // resolving HTML named entities inline. No heap allocation; entity names
  // compared in-place with strncmp — no copy, no extra stack buffer.
  if (!parseError) {
    static const char* const kXmlBuiltins[] = {"amp", "lt", "gt", "quot", "apos"};
    int i = 0;
    while (i < len && !parseError) {
      // Feed run of non-entity characters directly (zero copy)
      int runStart = i;
      while (i < len && html[i] != '&') i++;
      if (i > runStart) {
        if (XML_Parse(parser, html + runStart, i - runStart, 0) != XML_STATUS_OK) {
          parseError = true;
          break;
        }
      }
      if (i >= len) break;

      // html[i] == '&' — scan for matching ';'
      int j = i + 1;
      while (j < len && html[j] != ';' && html[j] != '&' && html[j] != '<' && (j - i) < 33) j++;

      if (j < len && html[j] == ';') {
        int nameLen = j - i - 1;
        const char* nameStart = html + i + 1;

        // Numeric references (&#NN; / &#xNN;) and XML built-ins pass through unchanged
        bool passThrough = (nameLen > 0 && nameStart[0] == '#');
        if (!passThrough) {
          for (const auto* b : kXmlBuiltins) {
            if (static_cast<int>(strlen(b)) == nameLen && strncmp(nameStart, b, nameLen) == 0) {
              passThrough = true;
              break;
            }
          }
        }

        if (passThrough) {
          if (XML_Parse(parser, html + i, j - i + 1, 0) != XML_STATUS_OK) {
            parseError = true;
            break;
          }
        } else {
          const char* repl = lookupHtmlEntity(nameStart, nameLen);
          if (repl && repl[0] != '\0') {
            if (XML_Parse(parser, repl, static_cast<int>(strlen(repl)), 0) != XML_STATUS_OK) {
              parseError = true;
              break;
            }
          }
        }
        i = j + 1;
      } else {
        // Malformed entity — pass '&' through and continue
        if (XML_Parse(parser, html + i, 1, 0) != XML_STATUS_OK) {
          parseError = true;
          break;
        }
        i++;
      }
    }
  }

  if (!parseError) {
    if (XML_Parse(parser, kClose, static_cast<int>(sizeof(kClose) - 1), 1) != XML_STATUS_OK) {
      parseError = true;
    }
  }

  flushPending();

  // textBuf is now stable — convert offsets to pointers.
  // Done even on parse error so that partially accumulated spans are usable.
  for (auto& s : spans) {
    s.text = textBuf.data() + s.textOffset;
  }

  return spans;
}

// ---------------------------------------------------------------------------
// Chunked entity resolution (shared by render() and renderFromFile())
// ---------------------------------------------------------------------------

void DictHtmlRenderer::feedEntityResolved(const char* buf, int len, bool isLast, char* carry, int* carryLen) {
  static const char* const kXmlBuiltins[] = {"amp", "lt", "gt", "quot", "apos"};
  int i = 0;
  while (i < len && !parseError) {
    int runStart = i;
    while (i < len && buf[i] != '&') i++;
    if (i > runStart) {
      if (XML_Parse(parser, buf + runStart, i - runStart, 0) != XML_STATUS_OK) {
        parseError = true;
        return;
      }
    }
    if (i >= len) break;

    // buf[i] == '&' — scan for ';'
    int j = i + 1;
    while (j < len && buf[j] != ';' && buf[j] != '&' && buf[j] != '<' && (j - i) < 33) j++;

    if (j >= len && !isLast) {
      // Entity spans chunk boundary — carry remainder for next chunk
      if (carry && carryLen) {
        int remaining = len - i;
        if (remaining > 63) remaining = 63;  // safety cap
        memcpy(carry, buf + i, remaining);
        *carryLen = remaining;
      }
      return;
    }

    if (j < len && buf[j] == ';') {
      int nameLen = j - i - 1;
      const char* nameStart = buf + i + 1;

      bool passThrough = (nameLen > 0 && nameStart[0] == '#');
      if (!passThrough) {
        for (const auto* b : kXmlBuiltins) {
          if (static_cast<int>(strlen(b)) == nameLen && strncmp(nameStart, b, nameLen) == 0) {
            passThrough = true;
            break;
          }
        }
      }

      if (passThrough) {
        if (XML_Parse(parser, buf + i, j - i + 1, 0) != XML_STATUS_OK) {
          parseError = true;
          return;
        }
      } else {
        const char* repl = lookupHtmlEntity(nameStart, nameLen);
        if (repl && repl[0] != '\0') {
          if (XML_Parse(parser, repl, static_cast<int>(strlen(repl)), 0) != XML_STATUS_OK) {
            parseError = true;
            return;
          }
        }
      }
      i = j + 1;
    } else {
      // Malformed entity — pass '&' through
      if (XML_Parse(parser, buf + i, 1, 0) != XML_STATUS_OK) {
        parseError = true;
        return;
      }
      i++;
    }
  }
}

// ---------------------------------------------------------------------------
// Stream-render from .dict file (definition never held in RAM)
// ---------------------------------------------------------------------------

void DictHtmlRenderer::parseOpenFile(HalFile& file, uint32_t size) {
  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, onStart, onEnd);
  XML_SetCharacterDataHandler(parser, onText);

  static constexpr const char kOpen[] = "<_root>";
  static constexpr const char kClose[] = "</_root>";

  if (XML_Parse(parser, kOpen, static_cast<int>(sizeof(kOpen) - 1), 0) != XML_STATUS_OK) {
    parseError = true;
  }

  if (!parseError) {
    char chunk[512];
    char carry[64];
    int carryLen = 0;
    uint32_t remaining = size;

    while (remaining > 0 && !parseError) {
      // Prepend carryover from previous chunk
      uint32_t space = static_cast<uint32_t>(sizeof(chunk) - carryLen);
      uint32_t toRead = remaining < space ? remaining : space;
      if (carryLen > 0) {
        memmove(chunk + carryLen, chunk, 0);  // no-op, just reserving layout
        memcpy(chunk, carry, carryLen);
      }
      int n = file.read(reinterpret_cast<uint8_t*>(chunk + carryLen), static_cast<int>(toRead));
      if (n <= 0) break;
      remaining -= static_cast<uint32_t>(n);
      int bufLen = carryLen + n;
      carryLen = 0;

      feedEntityResolved(chunk, bufLen, remaining == 0, carry, &carryLen);
    }

    // Flush any trailing carryover (e.g. malformed entity at end of file)
    if (carryLen > 0 && !parseError) {
      feedEntityResolved(carry, carryLen, true, nullptr, nullptr);
    }
  }

  if (!parseError) {
    if (XML_Parse(parser, kClose, static_cast<int>(sizeof(kClose) - 1), 1) != XML_STATUS_OK) {
      parseError = true;
    }
  }

  flushPending();
}

const std::vector<StyledSpan>& DictHtmlRenderer::renderFromFile(const char* dictPath, uint32_t offset, uint32_t size) {
  reset();
  ensureBatchBuffers();

  if (!parser) {
    parseError = true;
    return spans;
  }

  HalFile file;
  if (!Storage.openFileForRead("DICT", dictPath, file)) {
    LOG_ERR("DHTML", "Failed to open: %s", dictPath);
    parseError = true;
    return spans;
  }
  file.seekSet(offset);

  parseOpenFile(file, size);
  file.close();

  for (auto& s : spans) {
    s.text = textBuf.data() + s.textOffset;
  }

  return spans;
}

bool DictHtmlRenderer::renderFromFileStreaming(const char* dictPath, uint32_t offset, uint32_t size,
                                               const SpanSink& sink) {
  reset();
  spanSink_ = sink;  // install for this parse; pushSpan() now streams to the sink

  if (!parser) {
    parseError = true;
    spanSink_ = SpanSink{};
    return false;
  }

  HalFile file;
  if (!Storage.openFileForRead("DICT", dictPath, file)) {
    LOG_ERR("DHTML", "Failed to open: %s", dictPath);
    parseError = true;
    spanSink_ = SpanSink{};
    return false;
  }
  file.seekSet(offset);

  parseOpenFile(file, size);
  file.close();

  spanSink_ = SpanSink{};  // streaming never materializes spans/textBuf — nothing to fix up
  return !parseError;
}

// ---------------------------------------------------------------------------
// Unknown tag tracking (smoke test only)
// ---------------------------------------------------------------------------

#ifdef DICT_HTML_RENDERER_TRACK_UNKNOWN

void DictHtmlRenderer::recordUnknownTag(const char* tagName, const char* wordBefore, const char* tagContents,
                                        const char* wordAfter) {
  if (unknownTagCount >= MAX_UNKNOWN_TAGS) return;
  UnknownTagInfo& info = unknownTags[unknownTagCount++];
  strncpy(info.tag, tagName, sizeof(info.tag) - 1);
  info.tag[sizeof(info.tag) - 1] = '\0';
  strncpy(info.entry, currentEntryName ? currentEntryName : "", sizeof(info.entry) - 1);
  info.entry[sizeof(info.entry) - 1] = '\0';
  strncpy(info.wordBefore, wordBefore ? wordBefore : "", sizeof(info.wordBefore) - 1);
  info.wordBefore[sizeof(info.wordBefore) - 1] = '\0';
  strncpy(info.tagContents, tagContents ? tagContents : "", sizeof(info.tagContents) - 1);
  info.tagContents[sizeof(info.tagContents) - 1] = '\0';
  strncpy(info.wordAfter, wordAfter ? wordAfter : "", sizeof(info.wordAfter) - 1);
  info.wordAfter[sizeof(info.wordAfter) - 1] = '\0';
}

void DictHtmlRenderer::extractLastWord(const char* text, int len, char* out, int outSize) {
  if (!text || len <= 0 || !out || outSize <= 0) {
    if (out && outSize > 0) out[0] = '\0';
    return;
  }
  int end = len - 1;
  while (end >= 0 && (text[end] == ' ' || text[end] == '\t' || text[end] == '\n')) end--;
  if (end < 0) {
    out[0] = '\0';
    return;
  }
  int start = end;
  while (start > 0 && text[start - 1] != ' ' && text[start - 1] != '\t' && text[start - 1] != '\n') start--;
  int wlen = end - start + 1;
  if (wlen >= outSize) wlen = outSize - 1;
  memcpy(out, text + start, wlen);
  out[wlen] = '\0';
}

#endif  // DICT_HTML_RENDERER_TRACK_UNKNOWN
