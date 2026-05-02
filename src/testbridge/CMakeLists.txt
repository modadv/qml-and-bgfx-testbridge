add_library(testbridge STATIC
    test_bridge.cpp
    ws_server.cpp
    rpc_dispatcher.cpp
    bus_endpoint.cpp
    qml_probe.cpp
    screen_grabber.cpp
    log_sink.cpp
    ${CMAKE_SOURCE_DIR}/src/quick/guidatabus.cpp
)

add_library(TestBridge::testbridge ALIAS testbridge)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

target_include_directories(testbridge PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/core/logger>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/core/logger>
)

target_link_libraries(testbridge PUBLIC
    Qt5::Core
    Qt5::Gui
    Qt5::Quick
    Qt5::Test
    Qt5::WebSockets
    spdlog::spdlog
    nlohmann_json::nlohmann_json
)

target_compile_definitions(testbridge PUBLIC TEST_BRIDGE_ENABLED=1)

set_target_properties(testbridge PROPERTIES
    EXPORT_NAME testbridge
    CXX_STANDARD 14
    CXX_STANDARD_REQUIRED ON
)

install(TARGETS testbridge
    EXPORT TestBridgeTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY ${CMAKE_SOURCE_DIR}/src/testbridge
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
)

install(DIRECTORY ${CMAKE_SOURCE_DIR}/src/core/logger
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/core
    FILES_MATCHING PATTERN "*.h"
)

set(TESTBRIDGE_CMAKE_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/TestBridge")

configure_package_config_file(
    ${CMAKE_SOURCE_DIR}/cmake/TestBridgeConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/TestBridgeConfig.cmake
    INSTALL_DESTINATION ${TESTBRIDGE_CMAKE_INSTALL_DIR}
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/TestBridgeConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/TestBridgeConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/TestBridgeConfigVersion.cmake
    DESTINATION ${TESTBRIDGE_CMAKE_INSTALL_DIR}
)

install(EXPORT TestBridgeTargets
    FILE TestBridgeTargets.cmake
    NAMESPACE TestBridge::
    DESTINATION ${TESTBRIDGE_CMAKE_INSTALL_DIR}
)
