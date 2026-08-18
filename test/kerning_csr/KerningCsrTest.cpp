// Kerning is stored in two encodings that must agree: built-in fonts use CSR
// (kernRowOffsets != nullptr), SD-card fonts build a dense matrix in RAM. A wrong
// CSR lookup shifts glyph advances, which shifts line breaks, which silently
// invalidates every cached page. These tests pin the two paths to each other.

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "EpdFont.h"
#include "EpdFontData.h"

namespace {

// Two kern classes are enough to exercise every branch: the fixture maps each
// codepoint to its own class so a pair (left, right) addresses one matrix cell.
constexpr uint8_t kLeftClasses = 4;
constexpr uint8_t kRightClasses = 5;

// Deliberately sparse and signed, with empty rows (row 2) and boundary columns
// (first and last of a row) because those are where a binary search goes wrong.
constexpr int8_t kDense[kLeftClasses * kRightClasses] = {
    // rc: 0    1    2    3    4
    -8, 0, 0, 0, 5,   // lc 0: first and last column only
    0,  0, 3, 0, 0,   // lc 1: single interior column
    0,  0, 0, 0, 0,   // lc 2: empty row
    1,  2, 3, 4, -5,  // lc 3: fully populated
};

struct KernFixture {
  std::vector<EpdKernClassEntry> left, right;
  std::vector<int8_t> values;
  std::vector<uint16_t> rowOffsets;
  std::vector<uint8_t> cols;
  EpdFontData dense{};
  EpdFontData csr{};
};

// Codepoints are arbitrary but must be ascending: both class tables are searched
// with lower_bound, so an unsorted fixture would mask a real ordering bug.
uint32_t leftCp(uint8_t cls) { return 0x41 + cls; }
uint32_t rightCp(uint8_t cls) { return 0x61 + cls; }

void buildFixture(KernFixture& f) {
  for (uint8_t i = 0; i < kLeftClasses; i++) {
    f.left.push_back({static_cast<uint16_t>(leftCp(i)), static_cast<uint8_t>(i + 1)});
  }
  for (uint8_t i = 0; i < kRightClasses; i++) {
    f.right.push_back({static_cast<uint16_t>(rightCp(i)), static_cast<uint8_t>(i + 1)});
  }

  f.rowOffsets.push_back(0);
  for (uint8_t r = 0; r < kLeftClasses; r++) {
    for (uint8_t c = 0; c < kRightClasses; c++) {
      const int8_t v = kDense[r * kRightClasses + c];
      if (v != 0) {
        f.cols.push_back(c);
        f.values.push_back(v);
      }
    }
    f.rowOffsets.push_back(static_cast<uint16_t>(f.cols.size()));
  }

  f.dense.kernLeftClasses = f.left.data();
  f.dense.kernRightClasses = f.right.data();
  f.dense.kernLeftEntryCount = static_cast<uint16_t>(f.left.size());
  f.dense.kernRightEntryCount = static_cast<uint16_t>(f.right.size());
  f.dense.kernLeftClassCount = kLeftClasses;
  f.dense.kernRightClassCount = kRightClasses;
  f.dense.kernMatrix = kDense;
  f.dense.kernRowOffsets = nullptr;  // selects the dense path (SD-card fonts)

  f.csr = f.dense;
  f.csr.kernMatrix = f.values.data();
  f.csr.kernRowOffsets = f.rowOffsets.data();
  f.csr.kernCols = f.cols.data();
}

TEST(KerningCsr, CsrMatchesDenseForEveryClassPair) {
  KernFixture f;
  buildFixture(f);
  const EpdFont denseFont(&f.dense);
  const EpdFont csrFont(&f.csr);

  for (uint8_t l = 0; l < kLeftClasses; l++) {
    for (uint8_t r = 0; r < kRightClasses; r++) {
      const int8_t expected = denseFont.getKerning(leftCp(l), rightCp(r));
      EXPECT_EQ(csrFont.getKerning(leftCp(l), rightCp(r)), expected)
          << "left class " << int(l) << ", right class " << int(r);
      EXPECT_EQ(expected, kDense[l * kRightClasses + r]);
    }
  }
}

TEST(KerningCsr, UnclassedCodepointsKernAsZeroInBothEncodings) {
  KernFixture f;
  buildFixture(f);
  const EpdFont denseFont(&f.dense);
  const EpdFont csrFont(&f.csr);

  // 0x7F is in neither class table, so lookupKernClass returns 0 on both sides.
  EXPECT_EQ(csrFont.getKerning(0x7F, rightCp(0)), 0);
  EXPECT_EQ(csrFont.getKerning(leftCp(0), 0x7F), 0);
  EXPECT_EQ(denseFont.getKerning(0x7F, rightCp(0)), 0);
  EXPECT_EQ(denseFont.getKerning(leftCp(0), 0x7F), 0);
}

TEST(KerningCsr, EmptyCsrRowReturnsZeroWithoutReadingNeighbouringRows) {
  KernFixture f;
  buildFixture(f);
  const EpdFont csrFont(&f.csr);

  // Row 2 is empty: rowOffsets[2] == rowOffsets[3]. A lookup must return 0
  // rather than fall through into row 3's entries.
  for (uint8_t r = 0; r < kRightClasses; r++) {
    EXPECT_EQ(csrFont.getKerning(leftCp(2), rightCp(r)), 0) << "right class " << int(r);
  }
}

TEST(KerningCsr, AbsentFontKerningIsZero) {
  EpdFontData none{};
  const EpdFont font(&none);
  EXPECT_EQ(font.getKerning(leftCp(0), rightCp(0)), 0);
}

// EpdGlyph is 12 bytes so the built-in glyph tables cost 4 bytes less per glyph.
// The .cpfont on-disk record is still 16 bytes and is decoded field-by-field —
// if this size changes, SdCardFont::decodeGlyphRecord must be revisited.
TEST(KerningCsr, EpdGlyphStaysTwelveBytesAndNaturallyAligned) {
  EXPECT_EQ(sizeof(EpdGlyph), 12u);
  EXPECT_EQ(alignof(EpdGlyph), 4u);
  EXPECT_EQ(offsetof(EpdGlyph, dataOffset), 8u);
}

}  // namespace
