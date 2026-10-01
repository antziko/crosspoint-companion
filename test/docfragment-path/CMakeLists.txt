add_executable(DocFragmentPathTest
  DocFragmentPathTest.cpp
)

# Header-only xpath prefix parsing — no .cpp needed, no Arduino/HAL link.
target_include_directories(DocFragmentPathTest PRIVATE
  ${REPO_ROOT}/lib/KOReaderSync
)

target_link_libraries(DocFragmentPathTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DocFragmentPathTest)
