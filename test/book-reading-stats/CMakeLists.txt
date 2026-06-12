add_executable(BookReadingStatsTest
  BookReadingStatsTest.cpp
  ${REPO_ROOT}/src/activities/reader/BookReadingStats.cpp
)

target_include_directories(BookReadingStatsTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../reading-time-history/stubs
  ${REPO_ROOT}/src/activities/reader
)

target_link_libraries(BookReadingStatsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookReadingStatsTest)
