cmake_minimum_required(VERSION 3.28)

project(MotorheadModelViewer VERSION 0.1.0 LANGUAGES C CXX)

enable_language(RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MODELVIEWER_WARNINGS_AS_ERRORS
       "Treat Model Viewer compiler warnings as errors" ON)

include(cmake/CompilerWarnings.cmake)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(MODELVIEWER_SDL_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../../project/motorhead/third_party/sdl")
if(NOT EXISTS "${MODELVIEWER_SDL_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Motorhead SDL source was not found: ${MODELVIEWER_SDL_DIR}")
endif()
add_subdirectory("${MODELVIEWER_SDL_DIR}" third_party/sdl-build EXCLUDE_FROM_ALL)

add_subdirectory(src)
