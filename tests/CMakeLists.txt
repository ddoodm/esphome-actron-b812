cmake_minimum_required(VERSION 3.14)
project(actron_b812_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.5.2
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(Catch2)

set(COMPONENT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../components/actron_b812)

add_executable(tests
  test_compressor_protection.cpp
  ${COMPONENT_DIR}/actron_b812.cpp
)

target_include_directories(tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/mocks
  ${COMPONENT_DIR}
)

target_link_libraries(tests PRIVATE Catch2::Catch2WithMain)

# Pass -DENABLE_TEST_LOGGING to see ESP_LOG* output during test runs
# target_compile_definitions(tests PRIVATE ENABLE_TEST_LOGGING)

include(CTest)
include(Catch)
catch_discover_tests(tests)
