# Header-only target: DictionaryRegistry::nextIndex is a static inline in the header,
# so DictionaryRegistry.cpp (which pulls in HalStorage) is deliberately NOT linked.
add_executable(DictRegistryCycleTest
  DictRegistryCycleTest.cpp
)

target_include_directories(DictRegistryCycleTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(DictRegistryCycleTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DictRegistryCycleTest)
