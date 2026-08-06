add_executable(DictCleanWordTest
  DictCleanWordTest.cpp
  ${REPO_ROOT}/src/util/Dictionary.cpp
)

target_include_directories(DictCleanWordTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs       # HalStorage / Logging stubs (must win)
  ${REPO_ROOT}/src                        # util/Dictionary.h
)

target_link_libraries(DictCleanWordTest PRIVATE
  GTest::gtest_main
)

gtest_discover_tests(DictCleanWordTest)
