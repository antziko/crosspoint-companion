add_executable(DictTabsTest
  DictTabsTest.cpp
)

target_include_directories(DictTabsTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(DictTabsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DictTabsTest)
