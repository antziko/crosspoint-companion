add_executable(BookCacheUtilsTest
  BookCacheUtilsTest.cpp
)

# siblingOriginPath is header-only + pure (delegates to KOReaderDocumentId::
# stripDeviceTag) — no .cpp, no Arduino/HAL link.
target_include_directories(BookCacheUtilsTest PRIVATE
  ${REPO_ROOT}/src/util
  ${REPO_ROOT}/lib/KOReaderSync
)

target_link_libraries(BookCacheUtilsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookCacheUtilsTest)
