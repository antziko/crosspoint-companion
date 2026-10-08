add_executable(TextBookTest
  TextBookTest.cpp
  ${REPO_ROOT}/lib/TextBook/TextBookCommon.cpp
  ${REPO_ROOT}/lib/TextBook/TxtToXhtml.cpp
  ${REPO_ROOT}/lib/TextBook/MarkdownToXhtml.cpp
  ${REPO_ROOT}/lib/expat/xmlparse.c
  ${REPO_ROOT}/lib/expat/xmlrole.c
  ${REPO_ROOT}/lib/expat/xmltok.c
)

target_include_directories(TextBookTest PRIVATE
  ${REPO_ROOT}/lib/TextBook
  ${REPO_ROOT}/lib/expat
)

# Match the firmware's expat configuration, so "well-formed here" means "parses on device".
target_compile_definitions(TextBookTest PRIVATE
  XML_CONTEXT_BYTES=1024
  XML_GE=0
)

target_link_libraries(TextBookTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TextBookTest)
