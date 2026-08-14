add_executable(DictLookupSessionTest
  DictLookupSessionTest.cpp
  ${REPO_ROOT}/src/util/Dictionary.cpp
)

target_include_directories(DictLookupSessionTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs       # HalStorage / Logging stubs (must win)
  ${REPO_ROOT}/src                        # util/Dictionary.h
  ${REPO_ROOT}/lib/Utf8                   # utf8IsCjkPunctuation (inline, no link needed)
)

# Fixture dictionaries are copied out of the repo at run time, not baked in.
target_compile_definitions(DictLookupSessionTest PRIVATE
  REPO_ROOT="${REPO_ROOT}"
)

target_link_libraries(DictLookupSessionTest PRIVATE
  GTest::gtest_main
)

gtest_discover_tests(DictLookupSessionTest)
