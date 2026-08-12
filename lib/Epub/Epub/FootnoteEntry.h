#pragma once

#include <cstring>

#define FOOTNOTE_NUMBER_LEN 32
// Bumped from 96 to 256 (upstream #2722): calibre-generated EPUBs with long split
// filenames and URL-encoded characters routinely exceed 96 chars (e.g.
// "Author-Title_split_047.html#_ftn3" encoded is ~150), and Page::addFootnote()
// truncates with strncpy, so the stored href silently resolved to nothing.
// This is inline storage, so it also sets the record width in section.bin —
// see SECTION_FILE_VERSION v47 in Section.cpp.
#define FOOTNOTE_HREF_LEN 256

struct FootnoteEntry {
  char number[FOOTNOTE_NUMBER_LEN];
  char href[FOOTNOTE_HREF_LEN];

  FootnoteEntry() {
    number[0] = '\0';
    href[0] = '\0';
  }
};
