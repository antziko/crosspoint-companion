# Header-only ArduinoJson from .pio/libdeps, for the one-time recent.json conversion.
file(GLOB ARDUINOJSON_SRC_DIRS "${REPO_ROOT}/.pio/libdeps/*/ArduinoJson/src")
if(NOT ARDUINOJSON_SRC_DIRS)
  message(FATAL_ERROR "ArduinoJson not found under .pio/libdeps/*/ArduinoJson/src — run `pio run` once to fetch deps")
endif()
list(GET ARDUINOJSON_SRC_DIRS 0 ARDUINOJSON_SRC_DIR)

add_executable(RecentBooksStoreTest
  RecentBooksStoreTest.cpp
  ${REPO_ROOT}/src/RecentBooksStore.cpp
)

target_include_directories(RecentBooksStoreTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs  # HalStorage / Logging stubs (must win)
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Memory
  ${ARDUINOJSON_SRC_DIR}
)

target_link_libraries(RecentBooksStoreTest PRIVATE
  GTest::gtest_main
)

gtest_discover_tests(RecentBooksStoreTest)
