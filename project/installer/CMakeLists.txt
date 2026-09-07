cmake_minimum_required(VERSION 3.28)

project(MotorheadInstaller VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MH_WARNINGS_AS_ERRORS "Treat installer warnings as errors" ON)
include(cmake/ProjectWarnings.cmake)

# Keep the game and installer as independent projects. Building the installer
# first updates Motorhead, then embeds that exact release executable. Each
# nested build is incremental, so unchanged game sources do no compilation.
set(MOTORHEAD_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../motorhead")
set(MOTORHEAD_BINARY "${MOTORHEAD_SOURCE_DIR}/build/bin/Motorhead.exe")

add_custom_target(MotorheadExecutable ALL
    COMMAND "${CMAKE_COMMAND}" --preset windows-release
    COMMAND "${CMAKE_COMMAND}" --build --preset windows-release --parallel 1
    BYPRODUCTS "${MOTORHEAD_BINARY}"
    WORKING_DIRECTORY "${MOTORHEAD_SOURCE_DIR}"
    COMMENT "Updating the embedded 32-bit Motorhead executable"
    VERBATIM)

set(ZLIB_SOURCES
    third_party/zlib/adler32.c
    third_party/zlib/compress.c
    third_party/zlib/crc32.c
    third_party/zlib/deflate.c
    third_party/zlib/gzclose.c
    third_party/zlib/gzlib.c
    third_party/zlib/gzread.c
    third_party/zlib/gzwrite.c
    third_party/zlib/inflate.c
    third_party/zlib/infback.c
    third_party/zlib/inftrees.c
    third_party/zlib/inffast.c
    third_party/zlib/trees.c
    third_party/zlib/uncompr.c
    third_party/zlib/zutil.c)
add_library(installer_zlib STATIC ${ZLIB_SOURCES})
target_include_directories(installer_zlib PUBLIC third_party/zlib)

set(INSTALLER_PROJECT_NAME ${PROJECT_NAME})
set(INSTALLER_PROJECT_VERSION ${PROJECT_VERSION})
set(PROJECT_NAME unshield)
set(PROJECT_VERSION 1.6.2)
set(SIZE_FORMAT zi)
set(HAVE_INTTYPES_H 1)
set(HAVE_MEMORY_H 1)
set(HAVE_STDBOOL_H 1)
set(HAVE_STDINT_H 1)
set(HAVE_STDLIB_H 1)
set(HAVE_STRING_H 1)
set(HAVE_SYS_STAT_H 1)
set(HAVE_SYS_TYPES_H 1)
set(HAVE_UNISTD_H 1)
set(USE_OUR_OWN_MD5 1)
configure_file(
    third_party/unshield/lib/unshield_config.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/lib/unshield_config.h)
set(PROJECT_NAME ${INSTALLER_PROJECT_NAME})
set(PROJECT_VERSION ${INSTALLER_PROJECT_VERSION})

set(UNSHIELD_SOURCES
    third_party/unshield/lib/component.c
    third_party/unshield/lib/converter.c
    third_party/unshield/lib/directory.c
    third_party/unshield/lib/file.c
    third_party/unshield/lib/file_group.c
    third_party/unshield/lib/helper.c
    third_party/unshield/lib/libunshield.c
    third_party/unshield/lib/log.c
    third_party/unshield/lib/md5/md5c.c)
add_library(installer_unshield STATIC ${UNSHIELD_SOURCES})
target_include_directories(installer_unshield
    PUBLIC third_party/unshield/lib
    PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(installer_unshield PUBLIC installer_zlib)

add_executable(Installer WIN32
    src/main.cpp
    src/retail_install.cpp
    third_party/cab/src/cabinet.cpp
    ${MOTORHEAD_SOURCE_DIR}/src/core/crypto/sha256.cpp
    ${MOTORHEAD_SOURCE_DIR}/src/disc/cue_sheet.cpp
    ${MOTORHEAD_SOURCE_DIR}/src/disc/iso9660.cpp
    ${MOTORHEAD_SOURCE_DIR}/src/disc/cd_audio.cpp
    ${MOTORHEAD_SOURCE_DIR}/src/content/formats/tbl_envelope.cpp
    src/ui.cpp
    src/installer.rc)
# RC include dependencies are not discovered reliably by every Windows cross
# toolchain. Rebuild the installer whenever an embedded owned asset changes.
set_property(SOURCE src/installer.rc APPEND PROPERTY OBJECT_DEPENDS
             "${MOTORHEAD_BINARY}"
             "${CMAKE_CURRENT_SOURCE_DIR}/assets/motorhead.ico"
             "${CMAKE_CURRENT_SOURCE_DIR}/assets/motorhead_banner.bmp"
             "${CMAKE_CURRENT_SOURCE_DIR}/assets/Default.LGF"
             "${CMAKE_CURRENT_SOURCE_DIR}/assets/motorhead.cfg"
             "${CMAKE_CURRENT_SOURCE_DIR}/assets/MenuDial.spr"
             "${CMAKE_CURRENT_SOURCE_DIR}/assets/installer.manifest")
add_dependencies(Installer MotorheadExecutable)
target_include_directories(Installer PRIVATE
    ${MOTORHEAD_SOURCE_DIR}/include
    third_party/cab/include)
target_compile_definitions(Installer PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    WINVER=0x0501 _WIN32_WINNT=0x0501)
target_link_libraries(Installer PRIVATE installer_unshield comctl32 shell32
                                                ole32 comdlg32)
if(MINGW)
    target_link_options(Installer PRIVATE
        -municode
        -Wl,--major-os-version,5,--minor-os-version,1
        -Wl,--major-subsystem-version,5,--minor-subsystem-version,1)
endif()
mh_set_project_warnings(Installer)
mh_make_portable_executable(Installer)

set_target_properties(Installer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
install(TARGETS Installer RUNTIME DESTINATION .)
