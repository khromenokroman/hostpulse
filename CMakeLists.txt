cmake_minimum_required(VERSION 3.20)
project(sys_info VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_TESTS "Build tests" ON)

# nlohmann-json
find_package(nlohmann_json REQUIRED)

# fmt
find_package(fmt REQUIRED)

# OpenSSL
find_package(OpenSSL REQUIRED)

# sys_info executable
add_executable(sys_info
    src/main.cpp
    src/collector.cpp
    src/server.cpp
)
target_include_directories(sys_info PRIVATE src third_party)
target_link_libraries(sys_info PRIVATE OpenSSL::SSL OpenSSL::Crypto pthread)
target_link_libraries(sys_info PRIVATE    nlohmann_json::nlohmann_json fmt::fmt)

# Tests
if(BUILD_TESTS)
    find_package(GTest REQUIRED)
    enable_testing()
    add_executable(sys_info_tests
        tests/test_collector.cpp
    )
    target_include_directories(sys_info_tests PRIVATE src third_party)
    target_link_libraries(sys_info_tests PRIVATE GTest::gtest_main GTest::gtest
        OpenSSL::SSL OpenSSL::Crypto pthread)
    target_link_libraries(sys_info_tests PRIVATE    nlohmann_json::nlohmann_json fmt::fmt)
    add_test(NAME sys_info_tests COMMAND sys_info_tests)
endif()
