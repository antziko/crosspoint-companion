add_executable(TextPoolTest
  TextPoolTest.cpp
)

# TextPool is header-only and pure (std::string only — no Arduino/HAL, no I/O).
target_include_directories(TextPoolTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(TextPoolTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TextPoolTest)
