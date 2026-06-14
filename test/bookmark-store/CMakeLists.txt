# Locate the header-only ArduinoJson that PlatformIO fetched into .pio/libdeps.
# Any environment's copy works (header-only); pick the first that exists.
file(GLOB ARDUINOJSON_SRC_DIRS "${REPO_ROOT}/.pio/libdeps/*/ArduinoJson/src")
if(NOT ARDUINOJSON_SRC_DIRS)
  message(FATAL_ERROR "ArduinoJson not found under .pio/libdeps/*/ArduinoJson/src — run `pio run` once to fetch deps")
endif()
list(GET ARDUINOJSON_SRC_DIRS 0 ARDUINOJSON_SRC_DIR)

add_executable(BookmarkStoreTest
  BookmarkStoreTest.cpp
  ${REPO_ROOT}/src/BookmarkStore.cpp
)

target_include_directories(BookmarkStoreTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs       # HalStorage / Logging / esp_rom_crc stubs (must win)
  ${REPO_ROOT}/src                        # BookmarkStore.h
  ${REPO_ROOT}/lib/Serialization          # Serialization.h (real, host-safe)
  ${REPO_ROOT}/lib/Memory                 # Memory.h (real, host-safe)
  ${ARDUINOJSON_SRC_DIR}                   # ArduinoJson.h + its internal <ArduinoJson/...> headers
)

target_link_libraries(BookmarkStoreTest PRIVATE
  GTest::gtest_main
)

gtest_discover_tests(BookmarkStoreTest)
