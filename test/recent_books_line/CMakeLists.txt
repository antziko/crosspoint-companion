add_executable(RecentBooksLineTest
  RecentBooksLineTest.cpp
)

target_include_directories(RecentBooksLineTest PRIVATE
  ${REPO_ROOT}/src/util
)

target_link_libraries(RecentBooksLineTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(RecentBooksLineTest)
