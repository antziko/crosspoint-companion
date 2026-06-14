add_executable(FileWindowSelectTest
  FileWindowSelectTest.cpp
  ${REPO_ROOT}/src/util/FileWindowSelect.cpp
)

target_include_directories(FileWindowSelectTest PRIVATE
  ${REPO_ROOT}/src/util
)

target_link_libraries(FileWindowSelectTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FileWindowSelectTest)
