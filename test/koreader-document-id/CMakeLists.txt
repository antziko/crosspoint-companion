add_executable(KOReaderDocumentIdTest
  KOReaderDocumentIdTest.cpp
)

# Header-only helper (stripDeviceSuffix) — no .cpp needed, no Arduino/HAL link.
target_include_directories(KOReaderDocumentIdTest PRIVATE
  ${REPO_ROOT}/lib/KOReaderSync
)

target_link_libraries(KOReaderDocumentIdTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(KOReaderDocumentIdTest)
