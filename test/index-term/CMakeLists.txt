add_executable(IndexTermTest
  IndexTermTest.cpp
  ${REPO_ROOT}/src/util/IndexTerm.cpp
  ${REPO_ROOT}/src/util/PageTokenScan.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

# The page-token-scan stubs supply the GfxRenderer surface PageTokenScan.cpp measures with.
target_include_directories(IndexTermTest PRIVATE
  ${REPO_ROOT}/test/page-token-scan/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/util
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(IndexTermTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(IndexTermTest)
