add_executable(DictNotesTest
  DictNotesTest.cpp
  ${REPO_ROOT}/src/util/DictNotes.cpp
  ${REPO_ROOT}/src/util/Dictionary.cpp
)

# The HalStorage / Logging / SdDebugLog stubs are shared with test/dict-lookup-session rather
# than copied: DictNotes.cpp reaches cleanWord through the same Dictionary.cpp those stubs
# already serve, and a second copy would drift. Listed first so they win over the device
# headers. The stub passes paths through unchanged while no root is set, which is what lets
# these tests use an absolute temp directory as the cache path.
target_include_directories(DictNotesTest PRIVATE
  ${REPO_ROOT}/test/dict-lookup-session/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Utf8   # utf8IsCjkPunctuation (inline), via Dictionary.cpp
)

target_link_libraries(DictNotesTest PRIVATE
  GTest::gtest_main
  crosspoint_test_common
)

gtest_discover_tests(DictNotesTest)
