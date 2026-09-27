add_executable(WordStoreTest
  WordStoreTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/WordStore.cpp
)

target_include_directories(WordStoreTest PRIVATE
  # For the Logging.h stub; WordStore itself needs only that and Memory.h.
  ${REPO_ROOT}/test/chapter_html_slim_parser/stubs
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Epub/Epub
  ${REPO_ROOT}/lib/Memory
)

target_link_libraries(WordStoreTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(WordStoreTest)
