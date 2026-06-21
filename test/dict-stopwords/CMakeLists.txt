add_executable(DictStopwordsTest
  DictStopwordsTest.cpp
)

target_include_directories(DictStopwordsTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(DictStopwordsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DictStopwordsTest)
