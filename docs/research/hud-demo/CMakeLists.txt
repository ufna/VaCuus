cmake_minimum_required(VERSION 3.16)
project(vacuus_hud_demo CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# --- Paths to prebuilt dependencies -----------------------------------------
set(RMLUI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../bench/RmlUi" CACHE PATH "RmlUi source tree with build/ containing librmlui.a")
set(QJS_ROOT   "${CMAKE_CURRENT_SOURCE_DIR}/qjs"            CACHE PATH "quickjs-ng source tree with build/ containing libqjs.a")

set(RMLUI_LIB "${RMLUI_ROOT}/build/librmlui.a")
set(QJS_LIB   "${QJS_ROOT}/build/libqjs.a")
foreach(_lib ${RMLUI_LIB} ${QJS_LIB})
    if(NOT EXISTS ${_lib})
        message(FATAL_ERROR "Missing prebuilt library: ${_lib}")
    endif()
endforeach()

# --- System deps -------------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(SDL2_IMAGE REQUIRED IMPORTED_TARGET SDL2_image)
pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)

# --- Executable ---------------------------------------------------------------
add_executable(vacuus_hud
    src/main.cpp
    src/VacuusJs.cpp
    # RmlUi SDL2 + OpenGL3 backend, compiled directly from the RmlUi tree
    ${RMLUI_ROOT}/Backends/RmlUi_Backend_SDL_GL3.cpp
    ${RMLUI_ROOT}/Backends/RmlUi_Renderer_GL3.cpp
    ${RMLUI_ROOT}/Backends/RmlUi_Platform_SDL.cpp
)

target_include_directories(vacuus_hud PRIVATE
    ${RMLUI_ROOT}/Include
    ${RMLUI_ROOT}/Backends
    ${QJS_ROOT}
)

target_compile_definitions(vacuus_hud PRIVATE
    RMLUI_STATIC_LIB
    RMLUI_SDL_VERSION_MAJOR=2
)

target_link_libraries(vacuus_hud PRIVATE
    ${RMLUI_LIB}
    ${QJS_LIB}
    PkgConfig::SDL2
    PkgConfig::SDL2_IMAGE
    PkgConfig::FREETYPE
    ${CMAKE_DL_LIBS}
    pthread
    m
)
