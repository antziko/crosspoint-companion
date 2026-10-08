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

add_executable(CodepointFreqTest
  CodepointFreqTest.cpp
)

# CodepointFreq is header-only + pure (constexpr bit packing) — no .cpp, no Arduino/HAL link.
target_include_directories(CodepointFreqTest PRIVATE
  ${REPO_ROOT}/lib/EpdFont
)

target_link_libraries(CodepointFreqTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CodepointFreqTest)

add_executable(SdCardFontKernTest
  SdCardFontKernTest.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

# Real SdCardFont over an in-memory font file; stubs/ stands in for HalStorage, the
# inflate scratch window, heap probes and logging.
target_include_directories(SdCardFontKernTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Memory
)

target_link_libraries(SdCardFontKernTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)
target_compile_options(SdCardFontKernTest PRIVATE -fno-exceptions)

gtest_discover_tests(SdCardFontKernTest)
