add_executable(ReadingTimeHistoryTest
  ReadingTimeHistoryTest.cpp
  ${REPO_ROOT}/src/activities/reader/ReadingTimeHistory.cpp
)

target_include_directories(ReadingTimeHistoryTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/Serialization
)

target_link_libraries(ReadingTimeHistoryTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReadingTimeHistoryTest)
