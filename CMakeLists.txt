cmake_minimum_required(VERSION 3.20)
project(hostpulse VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_TESTS "Build tests" ON)

# nlohmann-json
find_package(nlohmann_json REQUIRED)

# fmt
find_package(fmt REQUIRED)

# OpenSSL
find_package(OpenSSL REQUIRED)

find_package(Threads REQUIRED)

# collector static library
add_library(collector STATIC
    src/collector.cpp
)
target_include_directories(collector PUBLIC src)
target_link_libraries(collector PUBLIC nlohmann_json::nlohmann_json)
target_link_libraries(collector PRIVATE fmt::fmt)

# server static library (HTTP + дашборд)
add_library(server STATIC
    src/server.cpp
)
target_include_directories(server PUBLIC src third_party)
target_link_libraries(server PUBLIC collector OpenSSL::SSL OpenSSL::Crypto Threads::Threads)

# hostpulse executable
add_executable(hostpulse
    src/main.cpp
)
target_link_libraries(hostpulse PRIVATE server)

# Tests
if(BUILD_TESTS)
    find_package(GTest REQUIRED)
    enable_testing()

    add_executable(hostpulse_tests
        tests/test_collector.cpp
        tests/test_server.cpp
    )
    target_link_libraries(hostpulse_tests PRIVATE GTest::gtest_main server)
    add_test(NAME hostpulse_tests COMMAND hostpulse_tests)

    # Синтаксис JS дашборда: при ошибке страница навсегда остаётся на «Загрузка...»
    find_program(NODE_EXECUTABLE NAMES node nodejs)
    if(NODE_EXECUTABLE)
        add_test(NAME web_ui_js_syntax
            COMMAND ${CMAKE_COMMAND}
                -DNODE=${NODE_EXECUTABLE}
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/src/web_ui.h
                -DOUT=${CMAKE_CURRENT_BINARY_DIR}/web_ui.js
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_web_ui_js.cmake)
    else()
        message(STATUS "node не найден: проверка JS дашборда пропущена")
    endif()
endif()
