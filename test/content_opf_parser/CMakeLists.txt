find_package(EXPAT REQUIRED)

add_executable(EncryptionManifestProbeTest
  EncryptionManifestProbeTest.cpp
)

target_include_directories(EncryptionManifestProbeTest PRIVATE
  ${REPO_ROOT}/test/chapter_html_slim_parser/stubs
  ${REPO_ROOT}/lib/Epub/Epub/parsers
  ${REPO_ROOT}/lib/XmlParserUtils
)

target_link_libraries(EncryptionManifestProbeTest PRIVATE
  crosspoint_test_common
  EXPAT::EXPAT
  GTest::gtest_main
)

gtest_discover_tests(EncryptionManifestProbeTest)
