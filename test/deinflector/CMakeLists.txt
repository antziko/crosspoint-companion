add_executable(DeinflectorTest
  DeinflectorTest.cpp
  ${REPO_ROOT}/lib/Dict/Deinflector.cpp
)

target_include_directories(DeinflectorTest PRIVATE
  ${REPO_ROOT}/lib/Dict
)

target_link_libraries(DeinflectorTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DeinflectorTest)
