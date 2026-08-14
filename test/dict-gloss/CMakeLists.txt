add_executable(DictGlossTest
  DictGlossTest.cpp
  ${REPO_ROOT}/src/util/DictGloss.cpp
  ${REPO_ROOT}/src/util/DictLayout.cpp
  ${REPO_ROOT}/src/util/Dictionary.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

# The HalStorage / Logging / SdDebugLog stubs are shared with test/dict-lookup-session rather
# than copied: both suites drive the same Dictionary.cpp against the same device-path-splicing
# fake, and a second copy would drift. Listed first so they win over the device headers.
target_include_directories(DictGlossTest PRIVATE
  ${REPO_ROOT}/test/dict-lookup-session/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/DictHtmlRenderer  # StyledSpan, via DictLayout.h
  ${REPO_ROOT}/lib/EpdFont           # EpdFontFamily::Style
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/expat             # expat.h only: DictLayout calls no XML_* function
)

# Match the firmware's expat configuration so its header declares the same API surface.
target_compile_definitions(DictGlossTest PRIVATE
  XML_GE=0
  XML_CONTEXT_BYTES=1024
  REPO_ROOT="${REPO_ROOT}"
)

target_link_libraries(DictGlossTest PRIVATE
  GTest::gtest_main
  crosspoint_test_common
)

gtest_discover_tests(DictGlossTest)
