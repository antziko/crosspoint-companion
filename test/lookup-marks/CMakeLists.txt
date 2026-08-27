add_executable(LookupMarksTest
  LookupMarksTest.cpp
  ${REPO_ROOT}/src/util/LookupMarks.cpp
)

target_include_directories(LookupMarksTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
)

target_link_libraries(LookupMarksTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(LookupMarksTest)
