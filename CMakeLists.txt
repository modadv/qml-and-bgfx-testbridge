cmake_minimum_required(VERSION 3.26)

project(testbridge-lab VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

option(ENABLE_TESTBRIDGE "Build and start the in-process Agent TestBridge" ON)
option(ENABLE_LIVE_SHADER "Enable Agent-controlled live shader compile/apply/revert hooks" ON)
option(ENABLE_AGENT_MCP "Enable Python MCP unit tests for Agent tools" ON)
option(ENABLE_RENDER_GOLDEN_TESTS "Enable render golden-image test entry points" ON)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(WIN32)
    add_compile_definitions(NOMINMAX _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0A00)
    if(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
        add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
    endif()
endif()

set(APP_VERSION_FULL "0.1.0-lab" CACHE STRING "Application version exposed by TestBridge")
add_compile_definitions(APP_VERSION_FULL="${APP_VERSION_FULL}")

find_package(Qt5 5.15 REQUIRED COMPONENTS
    Core Gui Qml Quick QuickControls2 Network OpenGL Test WebSockets
)
if(UNIX AND NOT APPLE)
    find_package(Qt5 5.15 REQUIRED COMPONENTS X11Extras)
endif()

find_package(spdlog REQUIRED CONFIG)
find_package(nlohmann_json REQUIRED CONFIG)

set(BGFX_BUILD_TOOLS ON CACHE BOOL "" FORCE)
set(BGFX_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(external/bgfx.cmake)

add_subdirectory(src/engine)
if(ENABLE_TESTBRIDGE)
    add_subdirectory(src/testbridge)
endif()
add_subdirectory(src/app)

if(BUILD_TESTING AND ENABLE_TESTBRIDGE)
    find_package(Python3 COMPONENTS Interpreter)
    set(TESTBRIDGE_PYTHON "" CACHE FILEPATH "Python interpreter used to run TestBridge tests")
    if(NOT TESTBRIDGE_PYTHON)
        set(_TESTBRIDGE_VENV_PYTHON "${CMAKE_SOURCE_DIR}/tools/testbridge-mcp/.venv/Scripts/python.exe")
        if(EXISTS "${_TESTBRIDGE_VENV_PYTHON}")
            set(TESTBRIDGE_PYTHON "${_TESTBRIDGE_VENV_PYTHON}")
        elseif(Python3_Interpreter_FOUND)
            set(TESTBRIDGE_PYTHON "${Python3_EXECUTABLE}")
        endif()
    endif()

    if(TESTBRIDGE_PYTHON)
        if(ENABLE_AGENT_MCP)
            add_test(
                NAME testbridge_mcp_unit
                COMMAND "${TESTBRIDGE_PYTHON}" -m pytest tools/testbridge-mcp/testbridge_mcp/tests -q
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            )
        endif()

        add_test(
            NAME template_manifest
            COMMAND "${TESTBRIDGE_PYTHON}" -m pytest tests/test_template_manifest.py -q
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        )
        set_tests_properties(template_manifest PROPERTIES
            LABELS "template"
        )

        add_test(
            NAME testbridge_lab_smoke
            COMMAND "${TESTBRIDGE_PYTHON}" tests/smoke_testbridge_lab.py
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        )
        set_tests_properties(testbridge_lab_smoke PROPERTIES
            ENVIRONMENT "TESTBRIDGE_LAB_APP=$<TARGET_FILE:testbridge_lab>"
        )

        add_test(
            NAME testbridge_lab_smoke_full
            COMMAND "${TESTBRIDGE_PYTHON}" tests/smoke_testbridge_lab.py
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        )
        set_tests_properties(testbridge_lab_smoke_full PROPERTIES
            LABELS "render;full"
            ENVIRONMENT "TESTBRIDGE_LAB_APP=$<TARGET_FILE:testbridge_lab>;TESTBRIDGE_RENDER_TIER=full;TESTBRIDGE_WINDOW_WIDTH=1180;TESTBRIDGE_WINDOW_HEIGHT=760"
        )

        add_test(
            NAME testbridge_lab_smoke_nocompute_small
            COMMAND "${TESTBRIDGE_PYTHON}" tests/smoke_testbridge_lab.py
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        )
        set_tests_properties(testbridge_lab_smoke_nocompute_small PROPERTIES
            LABELS "render;nocompute;small-window"
            ENVIRONMENT "TESTBRIDGE_LAB_APP=$<TARGET_FILE:testbridge_lab>;TESTBRIDGE_RENDER_TIER=nocompute;TESTBRIDGE_WINDOW_WIDTH=960;TESTBRIDGE_WINDOW_HEIGHT=640"
        )

        if(ENABLE_LIVE_SHADER)
            add_test(
                NAME testbridge_lab_live_shader_full
                COMMAND "${TESTBRIDGE_PYTHON}" tests/smoke_testbridge_lab.py
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            )
            set_tests_properties(testbridge_lab_live_shader_full PROPERTIES
                LABELS "render;full;shader"
                ENVIRONMENT "TESTBRIDGE_LAB_APP=$<TARGET_FILE:testbridge_lab>;TESTBRIDGE_RENDER_TIER=full;TESTBRIDGE_LIVE_SHADER_SELFTEST=1;TESTBRIDGE_WINDOW_WIDTH=1180;TESTBRIDGE_WINDOW_HEIGHT=760"
            )

            add_test(
                NAME testbridge_lab_live_shader_nocompute
                COMMAND "${TESTBRIDGE_PYTHON}" tests/smoke_testbridge_lab.py
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            )
            set_tests_properties(testbridge_lab_live_shader_nocompute PROPERTIES
                LABELS "render;nocompute;shader"
                ENVIRONMENT "TESTBRIDGE_LAB_APP=$<TARGET_FILE:testbridge_lab>;TESTBRIDGE_RENDER_TIER=nocompute;TESTBRIDGE_LIVE_SHADER_SELFTEST=1;TESTBRIDGE_WINDOW_WIDTH=960;TESTBRIDGE_WINDOW_HEIGHT=640"
            )
        endif()

        if(ENABLE_RENDER_GOLDEN_TESTS)
            add_test(
                NAME testbridge_lab_golden_probe
                COMMAND "${TESTBRIDGE_PYTHON}" tests/smoke_testbridge_lab.py
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            )
            set_tests_properties(testbridge_lab_golden_probe PROPERTIES
                LABELS "render;golden"
                DISABLED TRUE
                ENVIRONMENT "TESTBRIDGE_LAB_APP=$<TARGET_FILE:testbridge_lab>;TESTBRIDGE_SAVE_ORACLE_ARTIFACTS=1"
            )
        endif()

        add_test(
            NAME testbridge_package_consumer
            COMMAND "${TESTBRIDGE_PYTHON}" tests/package_consumer_check.py --build-dir "${CMAKE_BINARY_DIR}" --config $<CONFIG>
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        )
        set_tests_properties(testbridge_package_consumer PROPERTIES
            LABELS "package;template"
        )
    else()
        message(WARNING "Python interpreter not found; TestBridge CTest entries were not added")
    endif()
endif()
