add_executable(AdvanceTableMergeTest
  AdvanceTableMergeTest.cpp
)

# AdvanceTableMerge is header-only + pure (no allocation, logging or I/O) — no .cpp,
# no Arduino/HAL link.
target_include_directories(AdvanceTableMergeTest PRIVATE
  ${REPO_ROOT}/lib/EpdFont
)

target_link_libraries(AdvanceTableMergeTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(AdvanceTableMergeTest)

add_executable(InkExtentTest
  InkExtentTest.cpp
)

# InkExtent is header-only + pure; it pulls in EpdFontData.h only for fp4's constexpr helpers.
target_include_directories(InkExtentTest PRIVATE
  ${REPO_ROOT}/lib/EpdFont
)

target_link_libraries(InkExtentTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(InkExtentTest)
