cmake_minimum_required(VERSION 3.25.1)
project(hostpulse
        VERSION 0.2.0.0
        LANGUAGES CXX
        HOMEPAGE_URL "https://github.com/khromenokroman/hostpulse")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_TESTS "Собирать тесты" ON)

find_package(nlohmann_json 3 REQUIRED)
find_package(Threads REQUIRED)

include(GNUInstallDirs)

# cpp-httplib header-only из third_party: пакет Debian собран в разделяемую библиотеку без .a
add_library(httplib INTERFACE)
target_include_directories(httplib SYSTEM INTERFACE third_party)

# сборщик данных из /proc и /sys
add_library(collector STATIC
        src/collector.cpp
)
target_include_directories(collector PUBLIC src)
target_link_libraries(collector PUBLIC nlohmann_json::nlohmann_json)

# HTTP-сервер и дашборд
add_library(server STATIC
        src/server.cpp
)
target_link_libraries(server PUBLIC collector httplib Threads::Threads)

add_executable(${PROJECT_NAME}
        src/main.cpp
)
target_compile_definitions(${PROJECT_NAME} PRIVATE HOSTPULSE_VERSION="${PROJECT_VERSION}")
target_link_libraries(${PROJECT_NAME} PRIVATE server)

if (BUILD_TESTS)
    enable_testing()
    find_package(GTest REQUIRED)

    add_executable(${PROJECT_NAME}_tests
            tests/test_collector.cpp
            tests/test_server.cpp
    )
    target_link_libraries(${PROJECT_NAME}_tests PRIVATE server GTest::gtest_main GTest::gmock)
    add_test(NAME ${PROJECT_NAME}_tests COMMAND ${PROJECT_NAME}_tests)

    # Синтаксис JS дашборда: при ошибке страница навсегда остаётся на «Загрузка...»
    find_program(NODE_EXECUTABLE NAMES node nodejs)
    if (NODE_EXECUTABLE)
        add_test(NAME web_ui_js_syntax
                COMMAND ${CMAKE_COMMAND}
                -DNODE=${NODE_EXECUTABLE}
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/src/web_ui.h
                -DOUT=${CMAKE_CURRENT_BINARY_DIR}/web_ui.js
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_web_ui_js.cmake)
    else ()
        message(STATUS "node не найден: проверка JS дашборда пропущена")
    endif ()
endif ()

install(TARGETS ${PROJECT_NAME}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES debian/copyright
        DESTINATION ${CMAKE_INSTALL_DOCDIR}
)

# служба systemd: DynamicUser, параметры в /etc/default/hostpulse
configure_file(debian/hostpulse.service.in hostpulse.service @ONLY)
configure_file(debian/conffiles.in conffiles @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/hostpulse.service
        DESTINATION lib/systemd/system
)
install(FILES debian/default
        DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/default
        RENAME hostpulse
)

set(CPACK_PACKAGE_NAME "${PROJECT_NAME}")
set(CPACK_PACKAGE_VENDOR "khromenokroman")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT "khromenokroman <roma55592@yandex.ru>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Computer health dashboard in the browser")
set(CPACK_PACKAGE_DESCRIPTION "Web dashboard with CPU load, temperatures, fans, memory, disks,\nhardware and network traffic, plus JSON HTTP API.")
set(CPACK_PACKAGE_HOMEPAGE_URL "${PROJECT_HOMEPAGE_URL}")

set(CPACK_GENERATOR "DEB")
set(CPACK_STRIP_FILES ON)
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION "admin")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "init-system-helpers")
# lspci: список видеокарт в «Оборудовании»
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "pciutils")
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
        ${CMAKE_CURRENT_BINARY_DIR}/conffiles
        ${CMAKE_CURRENT_SOURCE_DIR}/debian/postinst
        ${CMAKE_CURRENT_SOURCE_DIR}/debian/prerm
        ${CMAKE_CURRENT_SOURCE_DIR}/debian/postrm
)
set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION ON)
set(CPACK_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

include(CPack)
