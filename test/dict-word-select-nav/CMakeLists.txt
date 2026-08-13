# Not a gtest suite — DictWordSelectNavigatorTest.cpp carries its own CHECK harness and
# main(), returning non-zero on failure. Registered with plain add_test rather than
# gtest_discover_tests. run.sh still works standalone for a quick local loop.
add_executable(DictWordSelectNavigatorTest
  DictWordSelectNavigatorTest.cpp
  ${REPO_ROOT}/src/util/WordSelectNavigator.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

# stubs/ must precede src/ and lib/ so the host GfxRenderer and MappedInputManager stubs
# win over the device-bound headers.
target_include_directories(DictWordSelectNavigatorTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(DictWordSelectNavigatorTest PRIVATE
  crosspoint_test_common
)

add_test(NAME DictWordSelectNavigator COMMAND DictWordSelectNavigatorTest)
