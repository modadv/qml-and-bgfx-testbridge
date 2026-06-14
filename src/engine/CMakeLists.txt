project(engine_core LANGUAGES CXX)

set(SHADER_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SHADER_OUTPUT_DIR ${CMAKE_BINARY_DIR}/bin/shaders)
set(VARYING_DEF ${SHADER_SOURCE_DIR}/varying.def.sc)

set(SHADER_INCLUDE_PATHS
    ${CMAKE_SOURCE_DIR}/external/bgfx.cmake/bgfx/src
    ${SHADER_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/common
)

file(GLOB SHADER_DEPENDS
    ${SHADER_SOURCE_DIR}/*.sh
    ${CMAKE_CURRENT_SOURCE_DIR}/common/*.sh
)

set(INCLUDE_ARGS "")
foreach(INCLUDE_PATH ${SHADER_INCLUDE_PATHS})
    list(APPEND INCLUDE_ARGS -i "${INCLUDE_PATH}")
endforeach()

set(VERTEX_SHADERS
    vs_terrain_render
    vs_terrain_simple
    vs_color
    vs_rect_wire
)

set(FRAGMENT_SHADERS
    fs_terrain_render
    fs_terrain_render_normal
    fs_terrain_simple
    fs_terrain_simple_normal
    fs_color
    fs_rect_wire
)

set(COMPUTE_SHADERS
    cs_generate_smap
    cs_terrain_init
    cs_terrain_lod
    cs_terrain_update_draw
    cs_terrain_update_indirect
    cs_heightfield_minmax
    cs_heightfield_reduce
    cs_heightfield_normalize
    cs_overlay_max_elevation
)

function(compile_shader SHADER_NAME SHADER_TYPE RENDERER OUTPUT_DIR)
    if(${SHADER_TYPE} STREQUAL "VERTEX")
        set(TYPE_FLAG vertex)
    elseif(${SHADER_TYPE} STREQUAL "FRAGMENT")
        set(TYPE_FLAG fragment)
    elseif(${SHADER_TYPE} STREQUAL "COMPUTE")
        set(TYPE_FLAG compute)
    endif()

    if(${RENDERER} STREQUAL "dx11")
        set(PLATFORM_NAME windows)
        set(PROFILE_VERSION s_5_0)
    elseif(${RENDERER} STREQUAL "glsl")
        if(WIN32)
            set(PLATFORM_NAME windows)
        elseif(APPLE)
            set(PLATFORM_NAME osx)
        else()
            set(PLATFORM_NAME linux)
        endif()
        if(${SHADER_TYPE} STREQUAL "COMPUTE")
            set(PROFILE_VERSION 430)
        elseif(APPLE)
            set(PROFILE_VERSION 410)
        else()
            set(PROFILE_VERSION 440)
        endif()
    elseif(${RENDERER} STREQUAL "spirv")
        if(WIN32)
            set(PLATFORM_NAME windows)
        elseif(APPLE)
            set(PLATFORM_NAME osx)
        else()
            set(PLATFORM_NAME linux)
        endif()
        set(PROFILE_VERSION spirv)
    elseif(${RENDERER} STREQUAL "metal")
        set(PLATFORM_NAME osx)
        set(PROFILE_VERSION metal)
    endif()

    set(INPUT_FILE ${SHADER_SOURCE_DIR}/${SHADER_NAME}.sc)
    set(OUTPUT_BIN ${OUTPUT_DIR}/${RENDERER}/${SHADER_NAME}.bin)
    file(MAKE_DIRECTORY ${OUTPUT_DIR}/${RENDERER})

    if(NOT ${SHADER_TYPE} STREQUAL "COMPUTE")
        set(VARYINGDEF_ARG --varyingdef "${VARYING_DEF}")
    else()
        set(VARYINGDEF_ARG "")
    endif()

    add_custom_command(
        OUTPUT ${OUTPUT_BIN}
        COMMAND $<TARGET_FILE:shaderc>
            -f "${INPUT_FILE}"
            -o "${OUTPUT_BIN}"
            --type ${TYPE_FLAG}
            --platform ${PLATFORM_NAME}
            --profile ${PROFILE_VERSION}
            ${INCLUDE_ARGS}
            ${VARYINGDEF_ARG}
            $<$<CONFIG:Debug>:--debug>
            $<$<CONFIG:Release>:-O 3>
        DEPENDS ${INPUT_FILE} ${VARYING_DEF} ${SHADER_DEPENDS} shaderc
        COMMENT "Compiling ${SHADER_TYPE} shader ${SHADER_NAME} for ${RENDERER}"
        VERBATIM
    )

    set(${SHADER_NAME}_${RENDERER}_OUTPUTS ${OUTPUT_BIN} PARENT_SCOPE)
endfunction()

if(WIN32)
    set(RENDERERS dx11 glsl spirv)
elseif(APPLE)
    set(RENDERERS metal glsl spirv)
else()
    set(RENDERERS glsl spirv)
endif()

set(ALL_SHADER_OUTPUTS "")
foreach(SHADER ${VERTEX_SHADERS})
    foreach(RENDERER ${RENDERERS})
        compile_shader(${SHADER} VERTEX ${RENDERER} ${SHADER_OUTPUT_DIR})
        list(APPEND ALL_SHADER_OUTPUTS ${${SHADER}_${RENDERER}_OUTPUTS})
    endforeach()
endforeach()
foreach(SHADER ${FRAGMENT_SHADERS})
    foreach(RENDERER ${RENDERERS})
        compile_shader(${SHADER} FRAGMENT ${RENDERER} ${SHADER_OUTPUT_DIR})
        list(APPEND ALL_SHADER_OUTPUTS ${${SHADER}_${RENDERER}_OUTPUTS})
    endforeach()
endforeach()
foreach(SHADER ${COMPUTE_SHADERS})
    foreach(RENDERER ${RENDERERS})
        if(NOT (${RENDERER} STREQUAL "glsl" AND APPLE))
            compile_shader(${SHADER} COMPUTE ${RENDERER} ${SHADER_OUTPUT_DIR})
            list(APPEND ALL_SHADER_OUTPUTS ${${SHADER}_${RENDERER}_OUTPUTS})
        endif()
    endforeach()
endforeach()

add_custom_target(engine_shaders ALL DEPENDS ${ALL_SHADER_OUTPUTS})

add_library(engine_terrain_assets STATIC
    terrain/heightfield_asset.cpp
)

target_include_directories(engine_terrain_assets PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/external/bgfx.cmake/bgfx/include
    ${CMAKE_SOURCE_DIR}/external/bgfx.cmake/bgfx/3rdparty
    ${CMAKE_SOURCE_DIR}/external/bgfx.cmake/bx/include
    ${CMAKE_SOURCE_DIR}/external/bgfx.cmake/bimg/include
)

if(WIN32)
    target_include_directories(engine_terrain_assets PRIVATE
        ${CMAKE_SOURCE_DIR}/external/bgfx.cmake/bx/include/compat/msvc
    )
elseif(APPLE)
    target_include_directories(engine_terrain_assets PRIVATE
        ${CMAKE_SOURCE_DIR}/external/bgfx.cmake/bx/include/compat/osx
    )
endif()

target_link_libraries(engine_terrain_assets PUBLIC
    bx
    bimg
)
if(TARGET bimg_decode)
    target_link_libraries(engine_terrain_assets PUBLIC bimg_decode)
endif()

add_library(engine_core STATIC
    common/bgfx_utils.cpp
    terrain/render_device.cpp
    terrain/terrain_cpu_compute.cpp
    terrain/performance_monitor.cpp
    terrain/terrain_renderer.cpp
    terrain/terrain_renderer_simple.cpp
    terrain/terrain_renderer_overlay.cpp
    terrain/terrain_patch_tables.cpp
    terrain/render_capabilities.cpp
    terrain/terrain_uniforms.cpp
    quick/render_viewport_item.cpp
    quick/render_scene.cpp
    quick/readback_presenter.cpp
    quick/asset_fetch_service.cpp
)

target_include_directories(engine_core PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core/logger
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/external/bgfx.cmake/bgfx/include
    ${CMAKE_SOURCE_DIR}/external/bgfx.cmake/bgfx/3rdparty
    ${CMAKE_SOURCE_DIR}/external/bgfx.cmake/bx/include
    ${CMAKE_SOURCE_DIR}/external/bgfx.cmake/bimg/include
)

if(WIN32)
    target_include_directories(engine_core PRIVATE
        ${CMAKE_SOURCE_DIR}/external/bgfx.cmake/bx/include/compat/msvc
    )
elseif(APPLE)
    target_include_directories(engine_core PRIVATE
        ${CMAKE_SOURCE_DIR}/external/bgfx.cmake/bx/include/compat/osx
    )
endif()

target_compile_definitions(engine_core PUBLIC
    ENTRY_CONFIG_USE_NATIVE=0
    $<$<CONFIG:Debug>:BX_CONFIG_DEBUG=1>
    $<$<CONFIG:Release>:BX_CONFIG_DEBUG=0>
)

target_compile_definitions(engine_core PRIVATE __STDC_FORMAT_MACROS)

target_link_libraries(engine_core PUBLIC
    engine_terrain_assets
    Qt5::Core
    Qt5::Gui
    Qt5::Qml
    Qt5::Quick
    Qt5::QuickControls2
    Qt5::Network
    Qt5::OpenGL
    bgfx
    bx
    bimg
    spdlog::spdlog
    nlohmann_json::nlohmann_json
)
if(TARGET bimg_decode)
    target_link_libraries(engine_core PUBLIC bimg_decode)
endif()

if(UNIX AND NOT APPLE AND TARGET Qt5::X11Extras)
    target_link_libraries(engine_core PUBLIC Qt5::X11Extras)
    target_compile_definitions(engine_core PRIVATE ENGINE_HAS_QX11INFO=1)
elseif(UNIX AND NOT APPLE AND TARGET Qt5::GuiPrivate)
    target_link_libraries(engine_core PRIVATE Qt5::GuiPrivate)
    target_compile_definitions(engine_core PRIVATE ENGINE_HAS_QPA_NATIVE_INTERFACE=1)
endif()

set(ENGINE_MESHOPTIMIZER_TARGET "")
if(TARGET meshoptimizer)
    set(ENGINE_MESHOPTIMIZER_TARGET meshoptimizer)
else()
    file(GLOB ENGINE_MESHOPTIMIZER_SOURCES
        ${CMAKE_SOURCE_DIR}/external/bgfx.cmake/bgfx/3rdparty/meshoptimizer/src/*.cpp
        ${CMAKE_SOURCE_DIR}/external/bgfx.cmake/bgfx/3rdparty/meshoptimizer/src/*.h
    )
    set(ENGINE_MESHOPTIMIZER_INCLUDE_DIR
        ${CMAKE_SOURCE_DIR}/external/bgfx.cmake/bgfx/3rdparty
    )
endif()

if(ENGINE_MESHOPTIMIZER_TARGET)
    # Provided by older bgfx.cmake revisions.
elseif(ENGINE_MESHOPTIMIZER_SOURCES)
    add_library(engine_meshoptimizer STATIC ${ENGINE_MESHOPTIMIZER_SOURCES})
    target_include_directories(engine_meshoptimizer PUBLIC ${ENGINE_MESHOPTIMIZER_INCLUDE_DIR})
    set_target_properties(engine_meshoptimizer PROPERTIES
        CXX_STANDARD 14
        CXX_STANDARD_REQUIRED ON
    )
    set(ENGINE_MESHOPTIMIZER_TARGET engine_meshoptimizer)
else()
    message(FATAL_ERROR "engine_core requires meshoptimizer sources from bgfx.cmake")
endif()

target_link_libraries(engine_core PUBLIC ${ENGINE_MESHOPTIMIZER_TARGET})

add_dependencies(engine_core engine_shaders)

if(BUILD_TESTING)
    add_executable(engine_heightfield_asset_tests
        ${CMAKE_SOURCE_DIR}/tests/engine/heightfield_asset_tests.cpp
    )
    target_link_libraries(engine_heightfield_asset_tests PRIVATE
        engine_terrain_assets
    )
    add_test(NAME engine_heightfield_asset_tests COMMAND $<TARGET_FILE:engine_heightfield_asset_tests>)
endif()
