add_executable(FlashcardDeckTest
  FlashcardDeckTest.cpp
  ${REPO_ROOT}/src/util/FlashcardDeck.cpp
)

target_include_directories(FlashcardDeckTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
)

target_link_libraries(FlashcardDeckTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FlashcardDeckTest)
