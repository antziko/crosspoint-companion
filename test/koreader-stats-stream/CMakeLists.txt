add_executable(KOReaderStatsStreamTest
  KOReaderStatsStreamTest.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

# stubs/ first so the host Logging.h shadows the device one (which pulls in Arduino).
target_include_directories(KOReaderStatsStreamTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/KOReaderSync
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(KOReaderStatsStreamTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(KOReaderStatsStreamTest)
