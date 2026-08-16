add_executable(OpdsStringArenaTest
  OpdsStringArenaTest.cpp
  ${REPO_ROOT}/lib/OpdsParser/OpdsStringArena.cpp
)

target_include_directories(OpdsStringArenaTest PRIVATE
  ${REPO_ROOT}/lib/OpdsParser
  ${REPO_ROOT}/lib/Memory
)

target_link_libraries(OpdsStringArenaTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(OpdsStringArenaTest)
