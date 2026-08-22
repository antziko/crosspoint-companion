# Not a gtest suite — PageTokenScanTest.cpp carries its own CHECK harness and main(),
# returning non-zero on failure. Registered with plain add_test rather than
# gtest_discover_tests. run.sh still works standalone for a quick local loop.
add_executable(PageTokenScanTest
  PageTokenScanTest.cpp
  ${REPO_ROOT}/src/util/PageTokenScan.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

# stubs/ must precede src/ and lib/ so the host GfxRenderer stub wins over the device-bound
# header.
target_include_directories(PageTokenScanTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(PageTokenScanTest PRIVATE
  crosspoint_test_common
)

add_test(NAME PageTokenScan COMMAND PageTokenScanTest)
