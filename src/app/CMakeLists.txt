add_executable(testbridge_lab
    main.cpp
)

target_include_directories(testbridge_lab PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core/logger
    ${CMAKE_SOURCE_DIR}/src/engine
    ${CMAKE_SOURCE_DIR}/src/engine/quick
)

target_compile_definitions(testbridge_lab PRIVATE
    TESTBRIDGE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    TESTBRIDGE_BUILD_DIR="${CMAKE_BINARY_DIR}"
    TESTBRIDGE_APP_ENABLED=$<BOOL:${ENABLE_TESTBRIDGE}>
    TESTBRIDGE_LIVE_SHADER_ENABLED=$<BOOL:${ENABLE_LIVE_SHADER}>
)

target_link_libraries(testbridge_lab PRIVATE
    Qt5::Core
    Qt5::Gui
    Qt5::Qml
    Qt5::Quick
    Qt5::QuickControls2
    Qt5::Network
    engine_core
)

if(ENABLE_TESTBRIDGE)
    target_link_libraries(testbridge_lab PRIVATE testbridge)
else()
    target_sources(testbridge_lab PRIVATE ../quick/guidatabus.cpp)
endif()

set_target_properties(testbridge_lab PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

add_custom_command(TARGET testbridge_lab POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/src/app/qml
            $<TARGET_FILE_DIR:testbridge_lab>/qml
    COMMENT "Copying QML files"
)

add_custom_command(TARGET testbridge_lab POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_BINARY_DIR}/bin/shaders
            $<TARGET_FILE_DIR:testbridge_lab>/shaders
    COMMENT "Copying compiled shaders"
)
