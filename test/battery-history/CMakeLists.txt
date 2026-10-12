add_executable(BatteryHistoryTest
  BatteryHistoryTest.cpp
  ${REPO_ROOT}/src/util/BatteryHistory.cpp
)

target_include_directories(BatteryHistoryTest PRIVATE
  ${REPO_ROOT}/src/util
)

target_link_libraries(BatteryHistoryTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BatteryHistoryTest)
