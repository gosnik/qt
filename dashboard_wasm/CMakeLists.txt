cmake_minimum_required(VERSION 3.24)
project(qtmem_dashboard LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    message(FATAL_ERROR "Configure qtmem/dashboard_wasm with the Emscripten toolchain via emcmake cmake.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
set(IMGUI_BACKENDS_DIR ${IMGUI_DIR}/backends)
set(JSON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json)
set(DIST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/web/dist)
set(WEB_STATIC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/web/static)

file(MAKE_DIRECTORY ${DIST_DIR})

add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_spectrum.cpp
    ${IMGUI_DIR}/imgui_stacklayout.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp
    ${IMGUI_BACKENDS_DIR}/imgui_impl_sdl2.cpp
    ${IMGUI_BACKENDS_DIR}/imgui_impl_opengl3.cpp
)

target_include_directories(imgui
    PUBLIC
        ${IMGUI_DIR}
        ${IMGUI_BACKENDS_DIR}
        ${IMGUI_DIR}/misc/cpp
)

target_compile_definitions(imgui PUBLIC IMGUI_IMPL_OPENGL_ES3 IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_compile_options(imgui PRIVATE "-sUSE_SDL=2")

add_executable(qtmem_dashboard
    src/main.cpp
)

set_target_properties(qtmem_dashboard PROPERTIES
    OUTPUT_NAME index
    SUFFIX ".js"
    RUNTIME_OUTPUT_DIRECTORY ${DIST_DIR}
)

target_include_directories(qtmem_dashboard
    PRIVATE
        ${IMGUI_DIR}
        ${IMGUI_BACKENDS_DIR}
        ${IMGUI_DIR}/misc/cpp
        ${JSON_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(qtmem_dashboard PRIVATE imgui)
target_compile_definitions(qtmem_dashboard PRIVATE IMGUI_IMPL_OPENGL_ES3)
target_compile_options(qtmem_dashboard PRIVATE "-sUSE_SDL=2")

if(EXISTS ${WEB_STATIC_DIR})
    target_link_options(qtmem_dashboard PRIVATE
        "--preload-file"
        ${WEB_STATIC_DIR}@/static
    )
endif()

target_link_options(qtmem_dashboard PRIVATE
    "-sUSE_SDL=2"
    "-sUSE_WEBGL2=1"
    "-sFULL_ES3=1"
    "-sFETCH=1"
    "-sALLOW_MEMORY_GROWTH=1"
    "-sNO_EXIT_RUNTIME=1"
    "-sASSERTIONS=1"
    "SHELL:-sEXPORTED_RUNTIME_METHODS=['UTF8ToString','stringToUTF8','lengthBytesUTF8']"
    "SHELL:-sEXPORTED_FUNCTIONS=['_malloc','_free','_main']"
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_link_options(qtmem_dashboard PRIVATE "-sASSERTIONS=2")
endif()

# Emscripten uses the absolute output path as its preload package identity.
# Keep shipped loaders independent of the developer's checkout location.
set_property(TARGET qtmem_dashboard APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/normalize_bundle.cmake")
add_custom_command(TARGET qtmem_dashboard POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        "-DLOADER=$<TARGET_FILE:qtmem_dashboard>"
        "-DPACKAGE_PATH=${DIST_DIR}/index.data"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/scripts/normalize_bundle.cmake"
    VERBATIM
)
