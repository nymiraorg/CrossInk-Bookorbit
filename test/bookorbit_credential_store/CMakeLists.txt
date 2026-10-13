add_executable(BookOrbitCredentialStoreTest
  BookOrbitCredentialStoreTest.cpp
  ${REPO_ROOT}/lib/BookOrbitSync/BookOrbitCredentialStore.cpp
  ${REPO_ROOT}/lib/Serialization/PersistableStore.cpp
  ${REPO_ROOT}/lib/Serialization/ObfuscationUtils.cpp
)

target_include_directories(BookOrbitCredentialStoreTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/BookOrbitSync
  ${REPO_ROOT}/lib/Serialization
)

target_link_libraries(BookOrbitCredentialStoreTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
  ArduinoJson
)

gtest_discover_tests(BookOrbitCredentialStoreTest)
