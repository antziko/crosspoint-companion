// Host-side litmus for WordSelectNavigator — word navigation and
// hyphenated-pair smoothing.
//
// Tests the fix that makes a hyphenated word behave as one navigation stop
// in horizontal directions, while leaving row navigation free to land on
// either half.
//
// Build/run: test/word-select-nav/run.sh

#include <GfxRenderer.h>
#include <MappedInputManager.h>

#include <cstdio>
#include <cstring>

#include "util/TextPool.h"
#include "util/WordSelectNavigator.h"

// --------------------------------------------------------------------------
// Tiny test harness
// --------------------------------------------------------------------------
static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, msg)                                              \
  do {                                                                \
    ++g_checks;                                                       \
    if (!(cond)) {                                                    \
      ++g_failures;                                                   \
      std::printf("  FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
    }                                                                 \
  } while (0)

// --------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------

static uint16_t poolAppendString(std::string& pool, const char* text) {
  return TextPool::append(pool, text, std::strlen(text));
}

static WordSelectNavigator::WordInfo mkWord(const char* text, int16_t x, int16_t y, int16_t width, int row) {
  WordSelectNavigator::WordInfo w;
  w.screenX = x;
  w.screenY = y;
  w.width = width;
  w.row = row;
  w.continuationIndex = -1;
  w.continuationOf = -1;
  w.textLen = static_cast<uint16_t>(std::strlen(text));
  w.lookupLen = w.textLen;
  // textOffset / lookupOffset filled by caller
  return w;
}

// Row 0:  wordA(10)  wordB(60)  under-(200)
// Row 1:                        stand(200) wordD(260) wordE(310)
//
// Hyphenated pair: under- (first half, index 2) + stand (second half, index 3)
// load() starts the cursor on the middle row/word (row 1, word 1 -> wordD).
static WordSelectNavigator makeHyphenatedFixture() {
  std::string pool;

  WordSelectNavigator::WordInfo w0 = mkWord("wordA", 10, 0, 40, 0);
  w0.textOffset = poolAppendString(pool, "wordA");
  w0.lookupOffset = w0.textOffset;

  WordSelectNavigator::WordInfo w1 = mkWord("wordB", 60, 0, 35, 0);
  w1.textOffset = poolAppendString(pool, "wordB");
  w1.lookupOffset = w1.textOffset;

  WordSelectNavigator::WordInfo w2 = mkWord("under-", 200, 0, 50, 0);
  w2.textOffset = poolAppendString(pool, "under-");
  w2.lookupOffset = w2.textOffset;

  WordSelectNavigator::WordInfo w3 = mkWord("stand", 200, 20, 45, 1);
  w3.textOffset = poolAppendString(pool, "stand");
  w3.lookupOffset = w3.textOffset;

  WordSelectNavigator::WordInfo w4 = mkWord("wordD", 260, 20, 40, 1);
  w4.textOffset = poolAppendString(pool, "wordD");
  w4.lookupOffset = w4.textOffset;

  WordSelectNavigator::WordInfo w5 = mkWord("wordE", 310, 20, 40, 1);
  w5.textOffset = poolAppendString(pool, "wordE");
  w5.lookupOffset = w5.textOffset;

  std::vector<WordSelectNavigator::WordInfo> words = {w0, w1, w2, w3, w4, w5};
  std::vector<WordSelectNavigator::Row> rows;
  WordSelectNavigator::organizeIntoRows(words, rows);
  // Run the real merge logic (the activity's path) so continuation links AND the
  // merged, hyphen-stripped lookup text ("understand") match production exactly —
  // hand-setting continuationIndex/continuationOf here would leave getLookup()
  // identical to getDisplay() and mask hyphen-stripping bugs in phrase building.
  WordSelectNavigator::mergeHyphenatedPairs(words, rows, pool);

  WordSelectNavigator nav;
  nav.load(std::move(words), std::move(rows), std::move(pool));
  return nav;
}

// Row 0:  alpha(10)  bravo(100)  charlie(200)  delta(300)
// Row 1:  solo(10)                                            <- lone short word, far left
// Row 2:  echo(10)   foxtrot(100) golf(200)     hotel(300)
//
// The one-word row is the shape that used to destroy the cursor's column: row navigation
// re-derived its reference X from whatever it had just landed on, so every row move past
// "solo" inherited x=10 and stuck to the start of the line.
// load() starts the cursor on the middle row/word (row 1 -> solo, its only word).
static WordSelectNavigator makeShortRowFixture() {
  std::string pool;
  const char* texts[] = {"alpha", "bravo", "charlie", "delta", "solo", "echo", "foxtrot", "golf", "hotel"};
  const int16_t xs[] = {10, 100, 200, 300, 10, 10, 100, 200, 300};
  const int16_t ys[] = {0, 0, 0, 0, 20, 40, 40, 40, 40};
  const int rowOf[] = {0, 0, 0, 0, 1, 2, 2, 2, 2};

  std::vector<WordSelectNavigator::WordInfo> words;
  for (int i = 0; i < 9; i++) {
    WordSelectNavigator::WordInfo w = mkWord(texts[i], xs[i], ys[i], 40, rowOf[i]);
    w.textOffset = poolAppendString(pool, texts[i]);
    w.lookupOffset = w.textOffset;
    words.push_back(w);
  }

  std::vector<WordSelectNavigator::Row> rows;
  WordSelectNavigator::organizeIntoRows(words, rows);

  WordSelectNavigator nav;
  nav.load(std::move(words), std::move(rows), std::move(pool));
  return nav;
}

// Starting from the fixture's initial cursor (wordD), navigate to targetWord
// using Left/Right presses.
static void navigateTo(WordSelectNavigator& nav, MappedInputManager& input, GfxRenderer& renderer,
                       const char* targetWord) {
  const char* current = nav.getDisplay(*nav.getSelected());
  if (std::strcmp(current, targetWord) == 0) return;

  // Determine direction based on flat index
  int targetFlat = -1;
  for (int i = 0; i < 6; i++) {
    const auto* w = nav.getWordAt(i);
    if (w && std::strcmp(nav.getDisplay(*w), targetWord) == 0) {
      targetFlat = i;
      break;
    }
  }
  int currentFlat = nav.getCurrentFlatIndex();

  MappedInputManager::Button dir =
      (targetFlat > currentFlat) ? MappedInputManager::Button::Right : MappedInputManager::Button::Left;

  for (int guard = 0; guard < 10; guard++) {
    input.reset();
    input.setReleased(dir, true);
    nav.handleNavigation(input, renderer);
    const char* now = nav.getDisplay(*nav.getSelected());
    if (std::strcmp(now, targetWord) == 0) return;
  }
}

// Park the cursor on the hyphenated first half, reached with a Right step.
// The route matters. Right leaves pendingSnapIdx unset (unlike the Left snap path), and
// it clears the goal column, so a following Down is matched against the first half's own
// X and lands on its partner below. Arriving by Up from wordD instead would carry wordD's
// column into the Down and come straight back to wordD — the goal column working as
// intended, but useless for staging a test on the second half.
static void horizNavToFirstHalf(WordSelectNavigator& nav, MappedInputManager& input, GfxRenderer& renderer) {
  navigateTo(nav, input, renderer, "wordB");
  input.reset();
  input.setReleased(MappedInputManager::Button::Right, true);
  nav.handleNavigation(input, renderer);
}

// Continue from the first half onto the second half by row navigation.
static void rowNavToSecondHalf(WordSelectNavigator& nav, MappedInputManager& input, GfxRenderer& renderer) {
  horizNavToFirstHalf(nav, input, renderer);
  input.reset();
  input.setReleased(MappedInputManager::Button::Down, true);
  nav.handleNavigation(input, renderer);
}

// --------------------------------------------------------------------------
// Tests
// --------------------------------------------------------------------------

static void testOrganizeIntoRows() {
  std::printf("testOrganizeIntoRows\n");

  std::string pool;
  std::vector<WordSelectNavigator::WordInfo> words;
  // y=0
  words.push_back(mkWord("a", 0, 0, 10, 0));
  words.back().textOffset = poolAppendString(pool, "a");
  words.back().lookupOffset = words.back().textOffset;
  words.push_back(mkWord("b", 20, 0, 10, 0));
  words.back().textOffset = poolAppendString(pool, "b");
  words.back().lookupOffset = words.back().textOffset;
  // y=2 (within 2px tolerance -> same row)
  words.push_back(mkWord("c", 40, 2, 10, 0));
  words.back().textOffset = poolAppendString(pool, "c");
  words.back().lookupOffset = words.back().textOffset;
  // y=10 (new row)
  words.push_back(mkWord("d", 0, 10, 10, 0));
  words.back().textOffset = poolAppendString(pool, "d");
  words.back().lookupOffset = words.back().textOffset;

  std::vector<WordSelectNavigator::Row> rows;
  WordSelectNavigator::organizeIntoRows(words, rows);

  CHECK(rows.size() == 2, "two rows created");
  // Rows hold a contiguous flat range rather than an index vector, so the invariant to
  // check is that the ranges partition the word list in order with no gap or overlap.
  CHECK(rows[0].firstWord == 0 && rows[0].wordCount == 3, "row 0 covers words [0,3)");
  CHECK(rows[1].firstWord == 3 && rows[1].wordCount == 1, "row 1 covers words [3,4)");
  CHECK(rows[0].firstWord + rows[0].wordCount == rows[1].firstWord, "rows are contiguous");
  CHECK(rows[1].firstWord + rows[1].wordCount == static_cast<int>(words.size()), "rows cover every word");
  CHECK(words[0].row == 0, "word 0 in row 0");
  CHECK(words[1].row == 0, "word 1 in row 0");
  CHECK(words[2].row == 0, "word 2 in row 0 (within tolerance)");
  CHECK(words[3].row == 1, "word 3 in row 1");

  // rowY is the row's single y, not each word's own. The gloss box is positioned against it for
  // exactly this reason: word "c" sits 2px below its row-mates, and a box anchored to the word
  // would shift by those 2px on a plain left/right step — which costs a strip restore and a
  // clean panel refresh every time.
  WordSelectNavigator nav;
  nav.load(words, rows, pool);
  CHECK(nav.rowY(0) == 0, "row 0 y is the row's, not word c's 2");
  CHECK(nav.rowY(1) == 10, "row 1 y");
}

static void testHyphenatedNavBackward() {
  std::printf("testHyphenatedNavBackward\n");
  WordSelectNavigator nav = makeHyphenatedFixture();
  MappedInputManager input;
  GfxRenderer renderer;

  // Fixture starts on wordD (flat index 4).
  // Press Left -> lands on stand (index 0 in row 1), then snaps to under- (first half).
  input.reset();
  input.setReleased(MappedInputManager::Button::Left, true);
  bool changed = nav.handleNavigation(input, renderer);

  CHECK(changed, "selection changed");
  const WordSelectNavigator::WordInfo* sel = nav.getSelected();
  CHECK(sel != nullptr, "has selected word");
  if (sel) {
    CHECK(std::strcmp(nav.getDisplay(*sel), "under-") == 0, "cursor on first half 'under-'");
  }

  // Press Left again -> should move to wordB.
  input.reset();
  input.setReleased(MappedInputManager::Button::Left, true);
  changed = nav.handleNavigation(input, renderer);
  CHECK(changed, "selection changed again");
  sel = nav.getSelected();
  if (sel) {
    CHECK(std::strcmp(nav.getDisplay(*sel), "wordB") == 0, "cursor on 'wordB'");
  }
}

static void testHyphenatedNavForward() {
  std::printf("testHyphenatedNavForward\n");
  WordSelectNavigator nav = makeHyphenatedFixture();
  MappedInputManager input;
  GfxRenderer renderer;

  // Navigate to wordB first (start from wordD, go left past the hyphenated pair).
  navigateTo(nav, input, renderer, "wordB");
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "wordB") == 0, "arrived at 'wordB'");

  // Press Right -> land on under- (first half).
  input.reset();
  input.setReleased(MappedInputManager::Button::Right, true);
  nav.handleNavigation(input, renderer);

  const WordSelectNavigator::WordInfo* sel = nav.getSelected();
  CHECK(sel != nullptr, "has selected word");
  if (sel) {
    CHECK(std::strcmp(nav.getDisplay(*sel), "under-") == 0, "cursor on first half 'under-'");
  }

  // Press Right again -> row-wrap to stand, then skip past to wordD.
  input.reset();
  input.setReleased(MappedInputManager::Button::Right, true);
  bool changed = nav.handleNavigation(input, renderer);
  CHECK(changed, "selection changed");
  sel = nav.getSelected();
  if (sel) {
    CHECK(std::strcmp(nav.getDisplay(*sel), "wordD") == 0, "cursor on 'wordD' (skipped second half)");
  }
}

// swapAxes trades the pairs: the side buttons walk word by word and the front pair
// moves between rows. One distinct failure mode -- the swap not reaching the axes --
// so one test per direction of the trade.
static void testSwapAxesSideStepsWords() {
  std::printf("testSwapAxesSideStepsWords\n");
  WordSelectNavigator nav = makeHyphenatedFixture();
  MappedInputManager input;
  GfxRenderer renderer;

  // Fixture starts on wordD; wordE is the next word in the same row.
  input.reset();
  input.setReleased(MappedInputManager::Button::Down, true);
  const bool changed = nav.handleNavigation(input, renderer, /*swapAxes=*/true);
  CHECK(changed, "swapped: side Down changed the selection");
  const WordSelectNavigator::WordInfo* sel = nav.getSelected();
  CHECK(sel != nullptr, "swapped: has selected word");
  if (sel) {
    CHECK(std::strcmp(nav.getDisplay(*sel), "wordE") == 0, "swapped: side Down stepped to 'wordE'");
  }

  // Unswapped, the same press is row navigation and must not land on wordE.
  WordSelectNavigator plain = makeHyphenatedFixture();
  input.reset();
  input.setReleased(MappedInputManager::Button::Down, true);
  plain.handleNavigation(input, renderer);
  const WordSelectNavigator::WordInfo* plainSel = plain.getSelected();
  if (plainSel) {
    CHECK(std::strcmp(plain.getDisplay(*plainSel), "wordE") != 0, "unswapped: side Down is not a word step");
  }
}

static void testSwapAxesFrontMovesRows() {
  std::printf("testSwapAxesFrontMovesRows\n");
  WordSelectNavigator nav = makeHyphenatedFixture();
  MappedInputManager input;
  GfxRenderer renderer;

  // Fixture starts on wordD in row 1; swapped, front Left is row-previous.
  CHECK(nav.getSelected() != nullptr && nav.getSelected()->row == 1, "starts on row 1");
  input.reset();
  input.setReleased(MappedInputManager::Button::Left, true);
  const bool changed = nav.handleNavigation(input, renderer, /*swapAxes=*/true);
  CHECK(changed, "swapped: front Left changed the selection");
  const WordSelectNavigator::WordInfo* sel = nav.getSelected();
  CHECK(sel != nullptr, "swapped: has selected word");
  if (sel) {
    CHECK(sel->row == 0, "swapped: front Left moved to the previous row");
  }
}

static void testHyphenatedNavRowNavExempt() {
  std::printf("testHyphenatedNavRowNavExempt\n");
  WordSelectNavigator nav = makeHyphenatedFixture();
  MappedInputManager input;
  GfxRenderer renderer;

  // Navigate to under- (first half, row 0) via row-nav (Up from wordD).
  horizNavToFirstHalf(nav, input, renderer);
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "under-") == 0, "arrived at 'under-'");

  // Press Down -> the closest word in row 1 to under-'s X=200 is stand.
  // Row navigation should NOT snap back to under-; cursor stays on stand.
  input.reset();
  input.setReleased(MappedInputManager::Button::Down, true);
  bool changed = nav.handleNavigation(input, renderer);
  CHECK(changed, "selection changed");

  const WordSelectNavigator::WordInfo* sel = nav.getSelected();
  CHECK(sel != nullptr, "has selected word");
  if (sel) {
    CHECK(std::strcmp(nav.getDisplay(*sel), "stand") == 0,
          "row nav lands on second half 'stand', does NOT snap to first half");
  }
}

static void testHyphenatedGetPairedHalf() {
  std::printf("testHyphenatedGetPairedHalf\n");
  WordSelectNavigator nav = makeHyphenatedFixture();
  MappedInputManager input;
  GfxRenderer renderer;

  // Navigate to under- (first half).
  navigateTo(nav, input, renderer, "under-");
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "under-") == 0, "on first half");

  const WordSelectNavigator::WordInfo* sel = nav.getSelected();
  CHECK(sel != nullptr, "has selected word");
  if (sel) {
    const WordSelectNavigator::WordInfo* cont = nav.getPairedHalf();
    CHECK(cont != nullptr, "first half has continuation");
    if (cont) {
      CHECK(std::strcmp(nav.getDisplay(*cont), "stand") == 0, "continuation is 'stand'");
    }
  }
}

// Verify that pressing Right from the first half at the end of a row wraps
// to the second half and then skips it, even when the second half is the
// only word at its position in the row.
static void testForwardSkipAtRowBoundary() {
  std::printf("testForwardSkipAtRowBoundary\n");
  WordSelectNavigator nav = makeHyphenatedFixture();
  MappedInputManager input;
  GfxRenderer renderer;

  // Navigate to under- (first half, last word in row 0).
  navigateTo(nav, input, renderer, "under-");
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "under-") == 0, "cursor on 'under-'");

  // Press Right: wraps to row 1, would land on stand, but skip past to wordD.
  input.reset();
  input.setReleased(MappedInputManager::Button::Right, true);
  bool changed = nav.handleNavigation(input, renderer);
  CHECK(changed, "selection changed after row wrap");
  const WordSelectNavigator::WordInfo* sel = nav.getSelected();
  if (sel) {
    CHECK(std::strcmp(nav.getDisplay(*sel), "wordD") == 0, "after row-wrap forward skip, cursor on 'wordD'");
  }
}

// Single row: wordA(0) under-(1) stand(2)
// under- has continuationIndex=2, stand has continuationOf=1.
// load() centers on middle word = under- (wordInRow=1).
static WordSelectNavigator makeSingleRowHyphenatedFixture() {
  std::string pool;

  WordSelectNavigator::WordInfo w0 = mkWord("wordA", 10, 0, 40, 0);
  w0.textOffset = poolAppendString(pool, "wordA");
  w0.lookupOffset = w0.textOffset;

  WordSelectNavigator::WordInfo w1 = mkWord("under-", 60, 0, 50, 0);
  w1.textOffset = poolAppendString(pool, "under-");
  w1.lookupOffset = w1.textOffset;
  w1.continuationIndex = 2;

  WordSelectNavigator::WordInfo w2 = mkWord("stand", 120, 0, 45, 0);
  w2.textOffset = poolAppendString(pool, "stand");
  w2.lookupOffset = w2.textOffset;
  w2.continuationOf = 1;

  std::vector<WordSelectNavigator::WordInfo> words = {w0, w1, w2};
  std::vector<WordSelectNavigator::Row> rows;
  WordSelectNavigator::organizeIntoRows(words, rows);

  WordSelectNavigator nav;
  nav.load(std::move(words), std::move(rows), std::move(pool));
  return nav;
}

// Row 0:  wordA(10)  wordB(60)  under-(200)  ← first half, trailing hyphen
// Row 1:                        -stand(200)  wordD(260)  wordE(310)
//                                 ↑ second half, leading hyphen too
static WordSelectNavigator makeHyphenBothFixture() {
  std::string pool;

  WordSelectNavigator::WordInfo w0 = mkWord("wordA", 10, 0, 40, 0);
  w0.textOffset = poolAppendString(pool, "wordA");
  w0.lookupOffset = w0.textOffset;

  WordSelectNavigator::WordInfo w1 = mkWord("wordB", 60, 0, 35, 0);
  w1.textOffset = poolAppendString(pool, "wordB");
  w1.lookupOffset = w1.textOffset;

  WordSelectNavigator::WordInfo w2 = mkWord("under-", 200, 0, 50, 0);
  w2.textOffset = poolAppendString(pool, "under-");
  w2.lookupOffset = w2.textOffset;

  WordSelectNavigator::WordInfo w3 = mkWord("-stand", 200, 20, 45, 1);
  w3.textOffset = poolAppendString(pool, "-stand");
  w3.lookupOffset = w3.textOffset;

  WordSelectNavigator::WordInfo w4 = mkWord("wordD", 260, 20, 40, 1);
  w4.textOffset = poolAppendString(pool, "wordD");
  w4.lookupOffset = w4.textOffset;

  WordSelectNavigator::WordInfo w5 = mkWord("wordE", 310, 20, 40, 1);
  w5.textOffset = poolAppendString(pool, "wordE");
  w5.lookupOffset = w5.textOffset;

  std::vector<WordSelectNavigator::WordInfo> words = {w0, w1, w2, w3, w4, w5};
  std::vector<WordSelectNavigator::Row> rows;
  WordSelectNavigator::organizeIntoRows(words, rows);
  // See makeHyphenatedFixture: run the real merge so getLookup() reflects the
  // hyphen-stripped, merged text rather than mirroring getDisplay().
  WordSelectNavigator::mergeHyphenatedPairs(words, rows, pool);

  WordSelectNavigator nav;
  nav.load(std::move(words), std::move(rows), std::move(pool));
  return nav;
}

// When the only row ends with a hyphenated pair, pressing Right from the first
// half should wrap around to word 0 — not get stuck on the second half.
static void testSingleRowForwardSkipWraps() {
  std::printf("testSingleRowForwardSkipWraps\n");
  WordSelectNavigator nav = makeSingleRowHyphenatedFixture();
  MappedInputManager input;
  GfxRenderer renderer;

  // load() places cursor on under- (wordInRow=1, middle of 3).
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "under-") == 0, "cursor starts on 'under-'");

  // Press Right: moves to stand, smoothing code tries to skip past stand,
  // single-row else branch wraps to wordInRow=0 (wordA).
  input.reset();
  input.setReleased(MappedInputManager::Button::Right, true);
  bool changed = nav.handleNavigation(input, renderer);
  CHECK(changed, "selection changed");
  const WordSelectNavigator::WordInfo* sel = nav.getSelected();
  if (sel) {
    CHECK(std::strcmp(nav.getDisplay(*sel), "wordA") == 0,
          "single-row wrap: cursor on 'wordA', not stuck on second half");
  }
}

// After a wordPrev snap (second half → first half, crossing a row boundary),
// pressing rowPrev should reference the second half's row as the nav base, not
// the first half's row the cursor now sits on.
//
// Fixture:  row 0 — wordA  wordB  under-
//           row 1 — stand  wordD  wordE
//
// Sequence: start at wordD (row 1) → Left → land on stand → snap to under- (row 0).
// Then Up:
//   Without fix: rowNavBase = currentRow = 0 → targetRow = rowCount-1 = 1 → wraps to stand. WRONG.
//   With fix:    rowNavBase = stand.row = 1 → targetRow = 0 → stays on under-. CORRECT.
static void testHyphenatedBackwardThenRowPrev() {
  std::printf("testHyphenatedBackwardThenRowPrev\n");
  WordSelectNavigator nav = makeHyphenatedFixture();
  MappedInputManager input;
  GfxRenderer renderer;

  // Fixture starts on wordD (row 1). Left: land on stand → snap to under- (row 0).
  input.reset();
  input.setReleased(MappedInputManager::Button::Left, true);
  bool changed = nav.handleNavigation(input, renderer);
  CHECK(changed, "Left changed selection");
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "under-") == 0, "snapped to first half 'under-'");

  // Up: should navigate to row 0 (one above stand's row 1), not wrap to row 1.
  input.reset();
  input.setReleased(MappedInputManager::Button::Up, true);
  changed = nav.handleNavigation(input, renderer);
  CHECK(changed, "Up registered as a navigation event");
  const WordSelectNavigator::WordInfo* sel = nav.getSelected();
  CHECK(sel != nullptr, "has selected word after Up");
  if (sel) {
    CHECK(std::strcmp(nav.getDisplay(*sel), "under-") == 0,
          "Up after snap stays on row 0 ('under-'), does NOT wrap to 'stand' on row 1");
  }
}

// When the cursor arrives on the second half via row navigation and the user
// presses Left, the pair should be treated as one stop: one Left skips past
// the first half and lands on the word before it.
//
// Fixture:  row 0 — wordA  wordB  under-
//           row 1 —               stand  wordD  wordE
//
// Sequence: navigate to under- (row 0) → Down → land on stand (second half,
//           row 1) → Left once → expect wordB, not under-.
static void testHyphenatedNavFromSecondHalfLeft() {
  std::printf("testHyphenatedNavFromSecondHalfLeft\n");
  WordSelectNavigator nav = makeHyphenatedFixture();
  MappedInputManager input;
  GfxRenderer renderer;

  horizNavToFirstHalf(nav, input, renderer);
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "under-") == 0, "arrived at 'under-'");

  // Down: row nav from row 0 → row 1, closest X to under-(200) is stand(200).
  input.reset();
  input.setReleased(MappedInputManager::Button::Down, true);
  nav.handleNavigation(input, renderer);
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "stand") == 0, "row-nav landed on 'stand' (second half)");

  // Left once: should skip past the first half and land on wordB.
  input.reset();
  input.setReleased(MappedInputManager::Button::Left, true);
  bool changed = nav.handleNavigation(input, renderer);
  CHECK(changed, "selection changed on Left");
  const WordSelectNavigator::WordInfo* sel = nav.getSelected();
  if (sel) {
    CHECK(std::strcmp(nav.getDisplay(*sel), "wordB") == 0,
          "one Left from second half skips first half and lands on 'wordB'");
  }
}

// renderHighlight on a non-hyphenated word: exactly 1 fillRect + 1 drawText.
static void testRenderHighlightSingleWord() {
  std::printf("testRenderHighlightSingleWord\n");
  WordSelectNavigator nav = makeHyphenatedFixture();
  MappedInputManager input;
  GfxRenderer renderer;

  // Fixture starts on wordD (non-hyphenated).
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "wordD") == 0, "cursor on 'wordD'");

  renderer.resetCounters();
  nav.renderHighlight(renderer, 16);
  CHECK(renderer.fillRectCallCount == 1, "single word: 1 fillRect");
  CHECK(renderer.drawTextCallCount == 1, "single word: 1 drawText");
}

// renderHighlight on the first half of a hyphenated pair: 2 fillRect + 2 drawText.
static void testRenderHighlightHyphenatedBothHalves() {
  std::printf("testRenderHighlightHyphenatedBothHalves\n");
  WordSelectNavigator nav = makeHyphenatedFixture();
  MappedInputManager input;
  GfxRenderer renderer;

  navigateTo(nav, input, renderer, "under-");
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "under-") == 0, "cursor on 'under-'");

  renderer.resetCounters();
  nav.renderHighlight(renderer, 16);
  CHECK(renderer.fillRectCallCount == 2, "hyphenated first half: 2 fillRects (both halves)");
  CHECK(renderer.drawTextCallCount == 2, "hyphenated first half: 2 drawTexts (both halves)");
}

// renderHighlight when cursor is on the second half (via row-nav): 2 fillRect + 2 drawText.
static void testRenderHighlightHyphenatedFromSecondHalf() {
  std::printf("testRenderHighlightHyphenatedFromSecondHalf\n");
  WordSelectNavigator nav = makeHyphenatedFixture();
  MappedInputManager input;
  GfxRenderer renderer;

  rowNavToSecondHalf(nav, input, renderer);
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "stand") == 0, "cursor on 'stand' (second half)");

  renderer.resetCounters();
  nav.renderHighlight(renderer, 16);
  CHECK(renderer.fillRectCallCount == 2, "second half: 2 fillRects (both halves via continuationOf)");
  CHECK(renderer.drawTextCallCount == 2, "second half: 2 drawTexts (both halves via continuationOf)");
}

// renderHighlightDifferential returns nullopt: stub readFramebufferRegion returns 0
// (capture fails), and hyphenated words are always rejected by the fast path.
static void testRenderHighlightDifferentialFallback() {
  std::printf("testRenderHighlightDifferentialFallback\n");
  WordSelectNavigator nav = makeHyphenatedFixture();
  MappedInputManager input;
  GfxRenderer renderer;

  // Non-hyphenated word (wordD, flat index 4): capture fails because stub returns 0 bytes.
  navigateTo(nav, input, renderer, "wordD");
  const int wordDIdx = nav.getCurrentFlatIndex();
  auto result = nav.renderHighlightDifferential(renderer, 16, -1, wordDIdx);
  CHECK(!result.has_value(), "non-hyphenated: nullopt when readFramebufferRegion returns 0");

  // Hyphenated first half (under-, flat index 2): always nullopt (fast path rejected).
  navigateTo(nav, input, renderer, "under-");
  const int underIdx = nav.getCurrentFlatIndex();
  auto result2 = nav.renderHighlightDifferential(renderer, 16, -1, underIdx);
  CHECK(!result2.has_value(), "hyphenated word: nullopt (fast path not supported)");
}

// Multi-select highlight: continuation half outside [lo,hi] is still drawn.
// Anchor on wordB (index 1), cursor on under- (index 2, first half).
// The second half 'stand' (index 3) lies outside [1,2] and must also be drawn.
static void testRenderHighlightMultiSelectHyphenatedFirstHalf() {
  std::printf("testRenderHighlightMultiSelectHyphenatedFirstHalf\n");
  WordSelectNavigator nav = makeHyphenatedFixture();
  MappedInputManager input;
  GfxRenderer renderer;

  // Navigate to under- to position the cursor there.
  navigateTo(nav, input, renderer, "under-");
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "under-") == 0, "cursor on 'under-'");

  // Enter multi-select with anchor on under- (index 2), then manually simulate
  // an anchor one word earlier (wordB, index 1) by entering multi-select after
  // navigating back one step.
  navigateTo(nav, input, renderer, "wordB");
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "wordB") == 0, "cursor on 'wordB'");

  // Enter multi-select mode with anchor at wordB.
  input.reset();
  input.setPressed(MappedInputManager::Button::Confirm, true);
  input.setHeldTime(700);  // > 600ms default threshold
  std::string phrase;
  nav.handleMultiSelectInput(input, phrase);
  CHECK(nav.isMultiSelecting(), "entered multi-select mode");

  // Consume the release.
  input.reset();
  input.setReleased(MappedInputManager::Button::Confirm, true);
  nav.handleMultiSelectInput(input, phrase);

  // Move cursor to under- (first half, index 2).
  navigateTo(nav, input, renderer, "under-");
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "under-") == 0, "cursor on 'under-' in multi-select");
  CHECK(nav.isMultiSelecting(), "still in multi-select");

  // Range is wordB(1)..under-(2). 'stand'(3) is outside but is under-'s continuationIndex.
  // Expect 3 fillRects: wordB, under-, stand.
  renderer.resetCounters();
  nav.renderHighlight(renderer, 16);
  CHECK(renderer.fillRectCallCount == 3, "multi-select ending on first half: 3 fillRects (wordB, under-, stand)");
  CHECK(renderer.drawTextCallCount == 3, "multi-select ending on first half: 3 drawTexts");

  // Confirm the selection and verify the phrase includes the continuation half.
  input.reset();
  input.setReleased(MappedInputManager::Button::Confirm, true);
  std::string confirmedPhrase;
  auto action = nav.handleMultiSelectInput(input, confirmedPhrase);
  CHECK(action == WordSelectNavigator::MultiSelectAction::PhraseReady, "confirm yields PhraseReady");
  CHECK(confirmedPhrase == "wordB understand",
        "phrase uses merged hyphen-free lookup text even though the second half lay "
        "outside the flat-index range");
}

// Multi-select highlight: when the range starts on the second half (anchor on
// 'stand', index 3), the first half 'under-' (index 2) lies outside [3, hi]
// and must also be drawn.
static void testRenderHighlightMultiSelectHyphenatedSecondHalf() {
  std::printf("testRenderHighlightMultiSelectHyphenatedSecondHalf\n");
  WordSelectNavigator nav = makeHyphenatedFixture();
  MappedInputManager input;
  GfxRenderer renderer;

  CHECK(nav.getSelected() != nullptr && std::strcmp(nav.getDisplay(*nav.getSelected()), "wordD") == 0,
        "fixture starts on 'wordD'");

  rowNavToSecondHalf(nav, input, renderer);
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "stand") == 0, "cursor on 'stand' (second half)");

  // Enter multi-select with anchor on stand (index 3).
  input.reset();
  input.setPressed(MappedInputManager::Button::Confirm, true);
  input.setHeldTime(700);
  std::string phrase;
  nav.handleMultiSelectInput(input, phrase);
  CHECK(nav.isMultiSelecting(), "entered multi-select mode");

  // Consume the release.
  input.reset();
  input.setReleased(MappedInputManager::Button::Confirm, true);
  nav.handleMultiSelectInput(input, phrase);

  // Move cursor to wordD (index 4), so range is stand(3)..wordD(4).
  // 'under-'(2) is outside [3,4] but is stand's continuationOf; must be drawn.
  input.reset();
  input.setReleased(MappedInputManager::Button::Right, true);
  nav.handleNavigation(input, renderer);
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "wordD") == 0, "cursor on 'wordD'");
  CHECK(nav.isMultiSelecting(), "still in multi-select");

  // Expect 3 fillRects: under-, stand, wordD.
  renderer.resetCounters();
  nav.renderHighlight(renderer, 16);
  CHECK(renderer.fillRectCallCount == 3, "multi-select starting on second half: 3 fillRects (under-, stand, wordD)");
  CHECK(renderer.drawTextCallCount == 3, "multi-select starting on second half: 3 drawTexts");

  // Confirm the selection and verify the phrase includes the first half of the pair.
  input.reset();
  input.setReleased(MappedInputManager::Button::Confirm, true);
  std::string confirmedPhrase;
  auto action = nav.handleMultiSelectInput(input, confirmedPhrase);
  CHECK(action == WordSelectNavigator::MultiSelectAction::PhraseReady, "confirm yields PhraseReady");
  CHECK(confirmedPhrase == "understand wordD",
        "phrase uses merged hyphen-free lookup text even though the first half lay "
        "outside the flat-index range");
}

// Multi-select phrase spanning both halves of a pair (anchor on 'wordA', cursor on
// 'wordE'): the merged lookup text must appear exactly once, not duplicated by
// emitting both 'under-' and 'stand' as separate lookup tokens.
static void testBuildPhraseHyphenatedPairNotDuplicated() {
  std::printf("testBuildPhraseHyphenatedPairNotDuplicated\n");
  WordSelectNavigator nav = makeHyphenatedFixture();
  MappedInputManager input;
  GfxRenderer renderer;

  navigateTo(nav, input, renderer, "wordA");
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "wordA") == 0, "cursor on 'wordA'");

  // Enter multi-select with anchor on wordA (index 0).
  input.reset();
  input.setPressed(MappedInputManager::Button::Confirm, true);
  input.setHeldTime(700);
  std::string phrase;
  nav.handleMultiSelectInput(input, phrase);
  CHECK(nav.isMultiSelecting(), "entered multi-select mode");

  // Consume the release.
  input.reset();
  input.setReleased(MappedInputManager::Button::Confirm, true);
  nav.handleMultiSelectInput(input, phrase);

  // Move cursor to wordE (index 5), so range is wordA(0)..wordE(5) and contains
  // both halves of the pair (under-=2, stand=3).
  navigateTo(nav, input, renderer, "wordE");
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "wordE") == 0, "cursor on 'wordE'");
  CHECK(nav.isMultiSelecting(), "still in multi-select");

  input.reset();
  input.setReleased(MappedInputManager::Button::Confirm, true);
  std::string confirmedPhrase;
  auto action = nav.handleMultiSelectInput(input, confirmedPhrase);
  CHECK(action == WordSelectNavigator::MultiSelectAction::PhraseReady, "confirm yields PhraseReady");
  CHECK(confirmedPhrase == "wordA wordB understand wordD wordE",
        "merged lookup text for the pair appears exactly once when both halves are "
        "inside the selected range");
}

// Build a single-row navigator from a list of display strings, each 20px wide.
// Used by the join tests below, which only care about buildPhrase's separator logic.
static WordSelectNavigator makeSingleRowFixture(const std::vector<const char*>& texts) {
  std::string pool;
  std::vector<WordSelectNavigator::WordInfo> words;
  int16_t x = 0;
  for (const char* t : texts) {
    WordSelectNavigator::WordInfo w = mkWord(t, x, 0, 20, 0);
    w.textOffset = poolAppendString(pool, t);
    w.lookupOffset = w.textOffset;
    words.push_back(w);
    x = static_cast<int16_t>(x + 25);
  }
  std::vector<WordSelectNavigator::Row> rows;
  WordSelectNavigator::organizeIntoRows(words, rows);
  WordSelectNavigator nav;
  nav.load(std::move(words), std::move(rows), std::move(pool));
  return nav;
}

// CJK is written without spaces, so a space-joined phrase matches no headword.
// buildPhrase must suppress the separator at any boundary touching CJK, while leaving
// Latin joins exactly as they were.
static void testBuildPhraseCjkJoinsWithoutSpaces() {
  std::printf("testBuildPhraseCjkJoinsWithoutSpaces\n");

  WordSelectNavigator han = makeSingleRowFixture({"\xE4\xB8\xAD", "\xE5\x9B\xBD", "\xE4\xBA\xBA"});  // 中 国 人
  CHECK(han.buildPhrase(0, 2) == "\xE4\xB8\xAD\xE5\x9B\xBD\xE4\xBA\xBA", "Han run joins with no spaces (中国人)");
  CHECK(han.buildPhrase(0, 1) == "\xE4\xB8\xAD\xE5\x9B\xBD", "Han pair joins with no spaces (中国)");
  CHECK(han.buildPhrase(1, 1) == "\xE5\x9B\xBD", "single Han character is itself");

  // Kana and Hangul take the same path (utf8IsCjkBreakable covers both).
  WordSelectNavigator kana = makeSingleRowFixture({"\xE3\x81\x8B", "\xE3\x81\xAA"});  // か な
  CHECK(kana.buildPhrase(0, 1) == "\xE3\x81\x8B\xE3\x81\xAA", "Kana pair joins with no spaces (かな)");

  // Regression guard: Latin behaviour must be byte-identical to before the CJK change.
  WordSelectNavigator latin = makeSingleRowFixture({"the", "quick", "fox"});
  CHECK(latin.buildPhrase(0, 2) == "the quick fox", "Latin run still space-joined");

  // Mixed boundary: the space is dropped on whichever side touches CJK, and kept
  // between the two Latin tokens.
  WordSelectNavigator mixed = makeSingleRowFixture({"WiFi", "\xE5\xAF\x86\xE7\xA0\x81", "now"});  // WiFi 密码 now
  CHECK(mixed.buildPhrase(0, 2) ==
            "WiFi\xE5\xAF\x86\xE7\xA0\x81"
            "now",
        "CJK-adjacent boundaries drop the space on both sides");
  CHECK(mixed.buildPhrase(0, 0) == "WiFi", "leading Latin token unaffected");

  // Fullwidth digits are content, not punctuation, and are CJK-breakable — so they
  // join gap-lessly like the surrounding Han.
  WordSelectNavigator fullwidth = makeSingleRowFixture({"\xEF\xBC\x91", "\xE6\x9C\x88"});  // １ 月
  CHECK(fullwidth.buildPhrase(0, 1) == "\xEF\xBC\x91\xE6\x9C\x88", "fullwidth digit + Han join with no space (１月)");
}

// Run Tests A–E against any two-row fixture with the same layout as
// makeHyphenatedFixture. firstHalf / secondHalf are the display strings of
// the two pair members; the surrounding words are always wordA/wordB/wordD/wordE.
static void runHyphenNavSuite(const char* label, WordSelectNavigator (*make)(), const char* firstHalf,
                              const char* secondHalf) {
  std::printf("%s\n", label);

  // A: Left from wordD hits the second half, snaps to first half; second Left
  //    continues to wordB.
  {
    WordSelectNavigator nav = make();
    MappedInputManager input;
    GfxRenderer renderer;
    input.reset();
    input.setReleased(MappedInputManager::Button::Left, true);
    nav.handleNavigation(input, renderer);
    const WordSelectNavigator::WordInfo* sel = nav.getSelected();
    CHECK(sel && std::strcmp(nav.getDisplay(*sel), firstHalf) == 0, "A: snap to first half on Left");
    input.reset();
    input.setReleased(MappedInputManager::Button::Left, true);
    nav.handleNavigation(input, renderer);
    sel = nav.getSelected();
    CHECK(sel && std::strcmp(nav.getDisplay(*sel), "wordB") == 0, "A: second Left reaches wordB");
  }

  // B: Right from wordB lands on first half; next Right skips second half -> wordD.
  {
    WordSelectNavigator nav = make();
    MappedInputManager input;
    GfxRenderer renderer;
    navigateTo(nav, input, renderer, "wordB");
    input.reset();
    input.setReleased(MappedInputManager::Button::Right, true);
    nav.handleNavigation(input, renderer);
    const WordSelectNavigator::WordInfo* sel = nav.getSelected();
    CHECK(sel && std::strcmp(nav.getDisplay(*sel), firstHalf) == 0, "B: Right lands on first half");
    input.reset();
    input.setReleased(MappedInputManager::Button::Right, true);
    nav.handleNavigation(input, renderer);
    sel = nav.getSelected();
    CHECK(sel && std::strcmp(nav.getDisplay(*sel), "wordD") == 0, "B: second Right skips second half -> wordD");
  }

  // C: Up from wordD row-navigates to the first half.
  {
    WordSelectNavigator nav = make();
    MappedInputManager input;
    GfxRenderer renderer;
    input.reset();
    input.setReleased(MappedInputManager::Button::Up, true);
    nav.handleNavigation(input, renderer);
    CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), firstHalf) == 0, "C: Up reaches first half");
  }

  // C2: row navigation onto the second half stays there (no snap back to first half).
  {
    WordSelectNavigator nav = make();
    MappedInputManager input;
    GfxRenderer renderer;
    rowNavToSecondHalf(nav, input, renderer);
    const WordSelectNavigator::WordInfo* sel = nav.getSelected();
    CHECK(sel && std::strcmp(nav.getDisplay(*sel), secondHalf) == 0, "C2: Down lands on second half, not snapped away");
  }

  // D: after backward snap (Left from wordD -> first half), Up must stay on the
  //    first half's row, not jump above it.
  {
    WordSelectNavigator nav = make();
    MappedInputManager input;
    GfxRenderer renderer;
    input.reset();
    input.setReleased(MappedInputManager::Button::Left, true);
    nav.handleNavigation(input, renderer);
    CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), firstHalf) == 0, "D: snapped to first half");
    input.reset();
    input.setReleased(MappedInputManager::Button::Up, true);
    nav.handleNavigation(input, renderer);
    const WordSelectNavigator::WordInfo* sel = nav.getSelected();
    CHECK(sel && std::strcmp(nav.getDisplay(*sel), firstHalf) == 0, "D: Up after snap stays on first half's row");
  }

  // E: Left from second half (arrived via row nav) skips the first half entirely.
  {
    WordSelectNavigator nav = make();
    MappedInputManager input;
    GfxRenderer renderer;
    rowNavToSecondHalf(nav, input, renderer);
    CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), secondHalf) == 0, "E: on second half via row-nav");
    input.reset();
    input.setReleased(MappedInputManager::Button::Left, true);
    nav.handleNavigation(input, renderer);
    const WordSelectNavigator::WordInfo* sel = nav.getSelected();
    CHECK(sel && std::strcmp(nav.getDisplay(*sel), "wordB") == 0,
          "E: one Left from second half skips first half -> wordB");
  }
}

// A word that both starts and ends with '-' (e.g. -re-) must not be treated as
// the first half of a line-break compound, even when it is the last word on its
// row. mergeHyphenatedPairs guards this with a lastWord[0] == '-' check.
//
// Row 0:  wordA(10)  wordB(60)  -re-(200)   ← ends with '-' but starts with '-'
// Row 1:                        Test(200)  wordD(260)
//
// The test calls mergeHyphenatedPairs (the same function the activity uses) and
// asserts the fields directly before loading the navigator, so removing the guard
// from mergeHyphenatedPairs will make this test fail.
static void testHyphenBothEndsNotPaired() {
  std::printf("testHyphenBothEndsNotPaired\n");

  std::string pool;
  WordSelectNavigator::WordInfo w0 = mkWord("wordA", 10, 0, 40, 0);
  w0.textOffset = poolAppendString(pool, "wordA");
  w0.lookupOffset = w0.textOffset;

  WordSelectNavigator::WordInfo w1 = mkWord("wordB", 60, 0, 35, 0);
  w1.textOffset = poolAppendString(pool, "wordB");
  w1.lookupOffset = w1.textOffset;

  WordSelectNavigator::WordInfo w2 = mkWord("-re-", 200, 0, 30, 0);
  w2.textOffset = poolAppendString(pool, "-re-");
  w2.lookupOffset = w2.textOffset;

  WordSelectNavigator::WordInfo w3 = mkWord("Test", 200, 20, 35, 1);
  w3.textOffset = poolAppendString(pool, "Test");
  w3.lookupOffset = w3.textOffset;

  WordSelectNavigator::WordInfo w4 = mkWord("wordD", 260, 20, 40, 1);
  w4.textOffset = poolAppendString(pool, "wordD");
  w4.lookupOffset = w4.textOffset;

  std::vector<WordSelectNavigator::WordInfo> words = {w0, w1, w2, w3, w4};
  std::vector<WordSelectNavigator::Row> rows;
  WordSelectNavigator::organizeIntoRows(words, rows);

  // Run the actual merge logic — this is what the activity calls.
  // Without the guard, -re- would be paired with Test here.
  WordSelectNavigator::mergeHyphenatedPairs(words, rows, pool);

  CHECK(words[2].continuationIndex == -1, "-re- not paired: continuationIndex must stay -1 after merge");
  CHECK(words[3].continuationOf == -1, "Test not paired: continuationOf must stay -1 after merge");

  WordSelectNavigator nav;
  nav.load(std::move(words), std::move(rows), std::move(pool));
  MappedInputManager input;
  GfxRenderer renderer;

  input.reset();
  input.setReleased(MappedInputManager::Button::Left, true);
  nav.handleNavigation(input, renderer);
  const WordSelectNavigator::WordInfo* sel = nav.getSelected();
  CHECK(sel && std::strcmp(nav.getDisplay(*sel), "Test") == 0, "-re- not paired: Left from wordD lands on Test");

  input.reset();
  input.setReleased(MappedInputManager::Button::Left, true);
  nav.handleNavigation(input, renderer);
  sel = nav.getSelected();
  CHECK(sel && std::strcmp(nav.getDisplay(*sel), "-re-") == 0, "-re- not paired: second Left lands on -re-");

  renderer.resetCounters();
  nav.renderHighlight(renderer, 16);
  CHECK(renderer.fillRectCallCount == 1, "-re- not paired: renderHighlight draws only 1 highlight");
}

// mergeHyphenatedPairs must strip the trailing '-' from the first half AND the
// leading '-' from the second half so the lookup text is hyphen-free.
// e.g. "under-" + "-stand" → lookup "understand", not "under-stand".
static void testMergeLookupBothHyphens() {
  std::printf("testMergeLookupBothHyphens\n");

  std::string pool;
  WordSelectNavigator::WordInfo w0 = mkWord("wordA", 10, 0, 40, 0);
  w0.textOffset = poolAppendString(pool, "wordA");
  w0.lookupOffset = w0.textOffset;

  WordSelectNavigator::WordInfo w1 = mkWord("under-", 60, 0, 50, 0);
  w1.textOffset = poolAppendString(pool, "under-");
  w1.lookupOffset = w1.textOffset;

  WordSelectNavigator::WordInfo w2 = mkWord("-stand", 60, 20, 45, 1);
  w2.textOffset = poolAppendString(pool, "-stand");
  w2.lookupOffset = w2.textOffset;

  WordSelectNavigator::WordInfo w3 = mkWord("wordD", 120, 20, 40, 1);
  w3.textOffset = poolAppendString(pool, "wordD");
  w3.lookupOffset = w3.textOffset;

  std::vector<WordSelectNavigator::WordInfo> words = {w0, w1, w2, w3};
  std::vector<WordSelectNavigator::Row> rows;
  WordSelectNavigator::organizeIntoRows(words, rows);
  WordSelectNavigator::mergeHyphenatedPairs(words, rows, pool);

  CHECK(words[1].continuationIndex == 2, "under- paired with -stand");
  CHECK(words[2].continuationOf == 1, "-stand paired with under-");
  CHECK(std::strcmp(pool.data() + words[1].lookupOffset, "understand") == 0,
        "first-half lookup is 'understand', not 'under-stand'");
  CHECK(std::strcmp(pool.data() + words[2].lookupOffset, "understand") == 0,
        "second-half lookup is 'understand', not 'under-stand'");
}

// Press one button and run a single navigation step.
static void step(WordSelectNavigator& nav, MappedInputManager& input, GfxRenderer& renderer,
                 MappedInputManager::Button button) {
  input.reset();
  input.setReleased(button, true);
  nav.handleNavigation(input, renderer);
}

static void testGoalColumnSurvivesShortRow() {
  std::printf("testGoalColumnSurvivesShortRow\n");

  WordSelectNavigator nav = makeShortRowFixture();
  GfxRenderer renderer;
  MappedInputManager input;

  // Cursor starts on solo, and the seeded column puts the first Up on charlie. Step off it
  // and back so the column under test is the one a deliberate left/right left behind, not
  // the load-time seed.
  step(nav, input, renderer, MappedInputManager::Button::Up);
  step(nav, input, renderer, MappedInputManager::Button::Left);
  step(nav, input, renderer, MappedInputManager::Button::Right);
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "charlie") == 0, "stepped back onto charlie (x=200)");

  step(nav, input, renderer, MappedInputManager::Button::Down);
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "solo") == 0, "Down lands on the lone word, the only choice");

  // solo sits at x=10, but charlie's column is what row 2 must be matched against.
  step(nav, input, renderer, MappedInputManager::Button::Down);
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "golf") == 0,
        "column held across the lone-word row: row 2 picks golf (x=200), not echo (x=10)");
}

static void testHorizontalStepRedefinesGoalColumn() {
  std::printf("testHorizontalStepRedefinesGoalColumn\n");

  WordSelectNavigator nav = makeShortRowFixture();
  GfxRenderer renderer;
  MappedInputManager input;

  // Up lands on charlie (x=200) via the seeded column; one Right moves on to delta.
  step(nav, input, renderer, MappedInputManager::Button::Up);
  step(nav, input, renderer, MappedInputManager::Button::Right);
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "delta") == 0, "Right moves to delta (x=300)");

  step(nav, input, renderer, MappedInputManager::Button::Down);
  step(nav, input, renderer, MappedInputManager::Button::Down);
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "hotel") == 0,
        "left/right redefines the column: row 2 picks hotel (x=300), not golf (x=200)");
}

// The reported bug: the cursor starts on a row holding one short word, so the very first
// row move used to take that word's own far-left centre as its reference and land on the
// start of the next row -- and then keep doing it, because the row branch stores what it
// aimed at. The load-time seed gives it the page's column instead.
static void testFirstRowMoveFromLoneWordRowUsesPageColumn() {
  std::printf("testFirstRowMoveFromLoneWordRowUsesPageColumn\n");

  WordSelectNavigator nav = makeShortRowFixture();
  GfxRenderer renderer;
  MappedInputManager input;

  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "solo") == 0, "cursor starts on the lone word (x=10)");

  step(nav, input, renderer, MappedInputManager::Button::Down);
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "golf") == 0,
        "first Down lands mid-row on golf (x=200), not echo (x=10)");

  // And the column persists, rather than being rewritten by the lone word each time.
  step(nav, input, renderer, MappedInputManager::Button::Up);
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "solo") == 0, "back onto the lone row, its only word");
  step(nav, input, renderer, MappedInputManager::Button::Up);
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "charlie") == 0,
        "and on through it to charlie (x=200), still the page column");
}

// Row 0: the alpha bravo   Row 1: of gamma delta   Row 2: the a of  (all closed-class)
// Column-wise the nearest word to x=100 is a stopword on every row; only row 2 has no
// alternative.
static WordSelectNavigator makeStopwordFixture() {
  std::string pool;
  const char* texts[] = {"the", "alpha", "bravo", "of", "gamma", "delta", "the", "a", "of"};
  const int16_t xs[] = {10, 90, 180, 10, 90, 180, 10, 90, 180};
  const int16_t ys[] = {0, 0, 0, 20, 20, 20, 40, 40, 40};
  const int rowOf[] = {0, 0, 0, 1, 1, 1, 2, 2, 2};

  std::vector<WordSelectNavigator::WordInfo> words;
  for (int i = 0; i < 9; i++) {
    WordSelectNavigator::WordInfo w = mkWord(texts[i], xs[i], ys[i], 40, rowOf[i]);
    w.textOffset = poolAppendString(pool, texts[i]);
    w.lookupOffset = w.textOffset;
    words.push_back(w);
  }

  std::vector<WordSelectNavigator::Row> rows;
  WordSelectNavigator::organizeIntoRows(words, rows);

  WordSelectNavigator nav;
  nav.load(std::move(words), std::move(rows), std::move(pool));
  return nav;
}

static void testRowNavPrefersContentWords() {
  std::printf("testRowNavPrefersContentWords\n");

  WordSelectNavigator nav = makeStopwordFixture();
  GfxRenderer renderer;
  MappedInputManager input;

  // Middle row, middle word by index is "gamma" -- already content, so the nudge is a no-op.
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "gamma") == 0, "cursor starts on the middle content word");

  // Page column is (10 + 220) / 2 = 115, nearest to which on row 0 is "alpha" (c=110). Row 0
  // also holds "the" at c=30, which the search must not prefer just for being closer to a
  // shorter reference later on.
  step(nav, input, renderer, MappedInputManager::Button::Up);
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "alpha") == 0, "Up lands on alpha, a content word");

  // Back down the same column.
  step(nav, input, renderer, MappedInputManager::Button::Down);
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "gamma") == 0, "Down returns to gamma on the same column");

  // Row 2 is nothing but closed-class words, so the fallback must still land somewhere.
  step(nav, input, renderer, MappedInputManager::Button::Down);
  const auto* sel = nav.getSelected();
  CHECK(sel != nullptr && sel->row == 2, "an all-stopword row still accepts the cursor");
}

// A row whose middle word by index is closed-class starts on its neighbour instead.
static void testInitialCursorNudgedOffStopword() {
  std::printf("testInitialCursorNudgedOffStopword\n");

  std::string pool;
  const char* texts[] = {"alpha", "bravo", "charlie", "delta", "the", "gamma"};
  const int16_t xs[] = {10, 90, 180, 10, 90, 180};
  const int16_t ys[] = {0, 0, 0, 20, 20, 20};
  const int rowOf[] = {0, 0, 0, 1, 1, 1};

  std::vector<WordSelectNavigator::WordInfo> words;
  for (int i = 0; i < 6; i++) {
    WordSelectNavigator::WordInfo w = mkWord(texts[i], xs[i], ys[i], 40, rowOf[i]);
    w.textOffset = poolAppendString(pool, texts[i]);
    w.lookupOffset = w.textOffset;
    words.push_back(w);
  }
  std::vector<WordSelectNavigator::Row> rows;
  WordSelectNavigator::organizeIntoRows(words, rows);

  WordSelectNavigator nav;
  nav.load(std::move(words), std::move(rows), std::move(pool));

  // Row 1, index 1 is "the"; the nudge walks outward and takes the nearer neighbour.
  const auto* sel = nav.getSelected();
  CHECK(sel != nullptr && std::strcmp(nav.getDisplay(*sel), "delta") == 0, "initial cursor nudged off 'the'");
}

// Multi-select builds a contiguous range, so a row move inside it must not skip anything --
// "man of the world" has to stay selectable.
static void testMultiSelectRowNavDoesNotSkipStopwords() {
  std::printf("testMultiSelectRowNavDoesNotSkipStopwords\n");

  WordSelectNavigator nav = makeStopwordFixture();
  GfxRenderer renderer;
  MappedInputManager input;

  // Starts on "gamma" (row 1, x=90).
  input.reset();
  input.setPressed(MappedInputManager::Button::Confirm, true);
  input.setHeldTime(700);
  std::string phrase;
  nav.handleMultiSelectInput(input, phrase);
  CHECK(nav.isMultiSelecting(), "entered multi-select mode");
  input.reset();
  input.setReleased(MappedInputManager::Button::Confirm, true);
  nav.handleMultiSelectInput(input, phrase);

  // Left inside multi-select is a plain one-word step: "of" is part of the range being
  // built, not something to walk past.
  step(nav, input, renderer, MappedInputManager::Button::Left);
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "of") == 0, "multi-select Left stops on 'of'");

  // And from "of" (c=30) the row move takes row 0's nearest word rather than its nearest
  // content word.
  step(nav, input, renderer, MappedInputManager::Button::Up);
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "the") == 0,
        "multi-select row nav takes the nearest word, stopword or not");
}

// Row 0: alpha(10)  the(90)   bravo(180)
// Row 1: gamma(10)  of(90)    a(180)
// Row 2: delta(10)  kappa(90) omega(180)
// Every row mixes content words with closed-class ones, and the word load() picks by index
// (row 1, index 1) is one of the closed-class ones.
static WordSelectNavigator makeMixedStopwordFixture(bool skipStopwords) {
  std::string pool;
  const char* texts[] = {"alpha", "the", "bravo", "gamma", "of", "a", "delta", "kappa", "omega"};
  const int16_t xs[] = {10, 90, 180, 10, 90, 180, 10, 90, 180};
  const int16_t ys[] = {0, 0, 0, 20, 20, 20, 40, 40, 40};
  const int rowOf[] = {0, 0, 0, 1, 1, 1, 2, 2, 2};

  std::vector<WordSelectNavigator::WordInfo> words;
  for (int i = 0; i < 9; i++) {
    WordSelectNavigator::WordInfo w = mkWord(texts[i], xs[i], ys[i], 40, rowOf[i]);
    w.textOffset = poolAppendString(pool, texts[i]);
    w.lookupOffset = w.textOffset;
    words.push_back(w);
  }

  std::vector<WordSelectNavigator::Row> rows;
  WordSelectNavigator::organizeIntoRows(words, rows);

  WordSelectNavigator nav;
  nav.setSkipStopwords(skipStopwords);
  nav.load(std::move(words), std::move(rows), std::move(pool));
  return nav;
}

// A page holding nothing but closed-class words, so the skip can never succeed.
static WordSelectNavigator makeAllStopwordFixture() {
  std::string pool;
  const char* texts[] = {"the", "a", "of", "and", "but", "for"};
  const int16_t xs[] = {10, 90, 180, 10, 90, 180};
  const int16_t ys[] = {0, 0, 0, 20, 20, 20};
  const int rowOf[] = {0, 0, 0, 1, 1, 1};

  std::vector<WordSelectNavigator::WordInfo> words;
  for (int i = 0; i < 6; i++) {
    WordSelectNavigator::WordInfo w = mkWord(texts[i], xs[i], ys[i], 40, rowOf[i]);
    w.textOffset = poolAppendString(pool, texts[i]);
    w.lookupOffset = w.textOffset;
    words.push_back(w);
  }

  std::vector<WordSelectNavigator::Row> rows;
  WordSelectNavigator::organizeIntoRows(words, rows);

  WordSelectNavigator nav;
  nav.load(std::move(words), std::move(rows), std::move(pool));
  return nav;
}

// Left/right walks past closed-class words instead of stopping on them, in the row and
// across the row boundary.
static void testHorizontalSkipsStopwords() {
  std::printf("testHorizontalSkipsStopwords\n");

  WordSelectNavigator nav = makeMixedStopwordFixture(true);
  GfxRenderer renderer;
  MappedInputManager input;

  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "gamma") == 0, "cursor starts on a content word");

  // Right past "of" and "a", over the row boundary, onto row 2's first content word.
  step(nav, input, renderer, MappedInputManager::Button::Right);
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "delta") == 0, "Right skips 'of' and 'a' onto delta");

  // And back the same way.
  step(nav, input, renderer, MappedInputManager::Button::Left);
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "gamma") == 0, "Left skips them again on the way back");

  // Wrapping backward past the start of the row lands on row 0's last word, which is content.
  step(nav, input, renderer, MappedInputManager::Button::Left);
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "bravo") == 0, "Left wraps to the previous row's last word");

  // Within one row, in both directions.
  step(nav, input, renderer, MappedInputManager::Button::Left);
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "alpha") == 0, "Left skips 'the' inside the row");
  step(nav, input, renderer, MappedInputManager::Button::Right);
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "bravo") == 0, "Right skips 'the' inside the row");
}

// Row navigation takes the nearest CONTENT word, not the nearest word.
static void testRowNavPrefersContentWordOverNearerStopword() {
  std::printf("testRowNavPrefersContentWordOverNearerStopword\n");

  WordSelectNavigator nav = makeMixedStopwordFixture(true);
  GfxRenderer renderer;
  MappedInputManager input;

  // Park on "bravo" (row 0, c=200) so the column comes from the cursor.
  step(nav, input, renderer, MappedInputManager::Button::Left);
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "bravo") == 0, "parked on bravo");

  // Row 1's nearest word to c=200 is "a" (c=200); the only content word on it is gamma.
  step(nav, input, renderer, MappedInputManager::Button::Down);
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "gamma") == 0, "Down passes over 'a' for gamma");
}

// The skip is bounded: a page with no content word at all still moves one stop per press.
static void testHorizontalSkipOnAllStopwordPage() {
  std::printf("testHorizontalSkipOnAllStopwordPage\n");

  WordSelectNavigator nav = makeAllStopwordFixture();
  GfxRenderer renderer;
  MappedInputManager input;

  // Row 1, index 1 -> "but"; contentWordNear has nothing to nudge onto.
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "but") == 0, "cursor starts on the middle word");

  step(nav, input, renderer, MappedInputManager::Button::Right);
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "for") == 0, "Right still advances exactly one word");

  step(nav, input, renderer, MappedInputManager::Button::Left);
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "but") == 0, "Left still steps back exactly one word");
}

// Quote selection turns the whole preference off: a quote is saved verbatim, so every word
// has to be reachable and the cursor must be able to start on one of them.
static void testSkipDisabledReachesEveryWord() {
  std::printf("testSkipDisabledReachesEveryWord\n");

  WordSelectNavigator nav = makeMixedStopwordFixture(false);
  GfxRenderer renderer;
  MappedInputManager input;

  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "of") == 0, "initial cursor is the plain index-middle word");

  // Page column is (10 + 220) / 2 = 115, nearest to which on row 0 is "the" (c=110).
  step(nav, input, renderer, MappedInputManager::Button::Up);
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "the") == 0, "row nav takes the nearest word, stopword or not");

  step(nav, input, renderer, MappedInputManager::Button::Left);
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "alpha") == 0, "Left steps one word");
  step(nav, input, renderer, MappedInputManager::Button::Right);
  CHECK(std::strcmp(nav.getDisplay(*nav.getSelected()), "the") == 0, "Right stops on 'the' again");
}

static void testHyphenEndOnly() {
  runHyphenNavSuite("testHyphenEndOnly (\"under-\" + \"stand\")", makeHyphenatedFixture, "under-", "stand");
}

static void testHyphenBoth() {
  runHyphenNavSuite("testHyphenBoth (\"under-\" + \"-stand\")", makeHyphenBothFixture, "under-", "-stand");
}

int main() {
  std::printf("=== WordSelectNavigator host litmus ===\n");
  testOrganizeIntoRows();
  testHyphenatedNavBackward();
  testHyphenatedNavForward();
  testHyphenatedNavRowNavExempt();
  testSwapAxesSideStepsWords();
  testSwapAxesFrontMovesRows();
  testHyphenatedGetPairedHalf();
  testForwardSkipAtRowBoundary();
  testSingleRowForwardSkipWraps();
  testHyphenatedBackwardThenRowPrev();
  testHyphenatedNavFromSecondHalfLeft();
  testRenderHighlightSingleWord();
  testRenderHighlightHyphenatedBothHalves();
  testRenderHighlightHyphenatedFromSecondHalf();
  testRenderHighlightDifferentialFallback();
  testRenderHighlightMultiSelectHyphenatedFirstHalf();
  testRenderHighlightMultiSelectHyphenatedSecondHalf();
  testBuildPhraseHyphenatedPairNotDuplicated();
  testBuildPhraseCjkJoinsWithoutSpaces();
  testHyphenBothEndsNotPaired();
  testMergeLookupBothHyphens();
  testGoalColumnSurvivesShortRow();
  testHorizontalStepRedefinesGoalColumn();
  testFirstRowMoveFromLoneWordRowUsesPageColumn();
  testRowNavPrefersContentWords();
  testInitialCursorNudgedOffStopword();
  testMultiSelectRowNavDoesNotSkipStopwords();
  testHorizontalSkipsStopwords();
  testRowNavPrefersContentWordOverNearerStopword();
  testHorizontalSkipOnAllStopwordPage();
  testSkipDisabledReachesEveryWord();
  testHyphenEndOnly();
  testHyphenBoth();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
