if(WIN32 AND NOT MH_LINUX_VARIANT)
  enable_language(RC)
endif()

add_library(mh_core STATIC
    "${MH_SYSTEM_ROOT}/src/platform/system.cpp"
    core/crypto/sha256.cpp
    core/filesystem/atomic_file.cpp
    core/filesystem/case_insensitive.cpp
    core/logging/runtime_log.cpp
    core/serialization/json.cpp
)
target_include_directories(mh_core PUBLIC "${MH_INCLUDE_DIR}")
mh_set_project_warnings(mh_core)

add_library(mh_content STATIC
    content/catalog/content_inventory.cpp
    content/catalog/dependency_graph.cpp
    content/catalog/import_cache.cpp
    content/export/image_export.cpp
    content/formats/ai_driver_profile.cpp
    content/formats/ai_route.cpp
    content/formats/car_definition.cpp
    content/formats/col_collision_mesh.cpp
    content/formats/divi_video_archive.cpp
    content/formats/fnt_bitmap_font.cpp
    content/formats/iff_image.cpp
    content/formats/league_definition.cpp
    content/formats/lob_line_object.cpp
    content/formats/mde_replay.cpp
    content/formats/menu_sound_config.cpp
    content/formats/motion_path.cpp
    content/formats/myo_object_model.cpp
    content/formats/myw_world_model.cpp
    content/formats/pdi_archive.cpp
    content/formats/spr_sprite_archive.cpp
    content/formats/sprite_positions.cpp
    content/formats/tbl_envelope.cpp
    content/formats/texture_assets.cpp
    content/formats/tga_image.cpp
    content/formats/track_definition.cpp
    content/formats/unlock_catalog.cpp
)
target_include_directories(mh_content PUBLIC
    "${MH_INCLUDE_DIR}"
)
target_link_libraries(mh_content PUBLIC mh_core)
mh_set_project_warnings(mh_content)

add_library(mh_disc STATIC
    disc/cd_audio.cpp
    disc/cue_sheet.cpp
    disc/iso9660.cpp
    "${MH_PLATFORM_ROOT}/src/disc/mounted_cd_audio.cpp"
)
target_include_directories(mh_disc PUBLIC "${MH_INCLUDE_DIR}")
target_link_libraries(mh_disc PUBLIC mh_core)
mh_set_project_warnings(mh_disc)

add_library(mh_game STATIC
    game/debug/drivetrain_trace.cpp
    game/gameplay/cheats.cpp
    game/input/player_input.cpp
    game/physics/body_hull_contact.cpp
    game/physics/body_pose.cpp
    game/physics/body_response.cpp
    game/physics/collision.cpp
    game/physics/dynamic_vehicle_contact.cpp
    game/physics/simulation.cpp
    game/physics/vehicle_response.cpp
    game/physics/wheel_contact.cpp
    game/race/camera.cpp
    game/race/distance_cue.cpp
    game/race/drive_session.cpp
    game/race/race_state.cpp
    game/race/replay.cpp
    game/race/replay_analysis.cpp
    game/race/sky.cpp
    game/vehicle/drive.cpp
    game/vehicle/placement.cpp
    game/vehicle/presentation.cpp
    game/vehicle/runtime.cpp
    game/vehicle/scene.cpp
    game/vehicle/tuning.cpp
)
target_include_directories(mh_game PUBLIC "${MH_INCLUDE_DIR}")
mh_set_project_warnings(mh_game)

add_library(mh_game_content STATIC
    game/ai/control_import.cpp
    game/ai/route_import.cpp
    game/ai/vehicle_runtime.cpp
    game/physics/collision_import.cpp
    game/race/course_import.cpp
    game/race/ghost_import.cpp
    game/vehicle/import.cpp
    game/vehicle/motion_import.cpp
)
target_include_directories(mh_game_content PUBLIC "${MH_INCLUDE_DIR}")
target_link_libraries(mh_game_content PUBLIC mh_game PRIVATE mh_content)
mh_set_project_warnings(mh_game_content)

add_library(mh_network STATIC
    network/protocol.cpp
    network/session.cpp
    "${MH_SYSTEM_ROOT}/src/platform/native_socket.cpp"
)
target_include_directories(mh_network PUBLIC "${MH_INCLUDE_DIR}")
if(WIN32)
    target_link_libraries(mh_network PRIVATE ws2_32)
endif()
mh_set_project_warnings(mh_network)

add_library(mh_sdl_platform STATIC platform/sdl_input.cpp "${MH_PLATFORM_ROOT}/src/platform/player_input.cpp")
target_include_directories(mh_sdl_platform PUBLIC "${MH_INCLUDE_DIR}")
target_link_libraries(mh_sdl_platform PUBLIC mh_game SDL3::SDL3-static)
mh_set_project_warnings(mh_sdl_platform)

set(MH_MOTORHEAD_CONFIG_SOURCE
    "${MH_SHARED_ROOT}/assets/motorhead.cfg")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${MH_MOTORHEAD_CONFIG_SOURCE}")
set(MH_MOTORHEAD_CONFIG_HEADER
    "${CMAKE_CURRENT_BINARY_DIR}/generated/game_config_template.hpp")
file(READ "${MH_MOTORHEAD_CONFIG_SOURCE}" MH_MOTORHEAD_CONFIG_TEMPLATE)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
    ui/config/game_config_template.hpp.in
    "${MH_MOTORHEAD_CONFIG_HEADER}"
    @ONLY)
add_library(mh_ui STATIC
    "${MH_PLATFORM_ROOT}/src/platform/renderer_options.cpp"
    "${MH_MOTORHEAD_CONFIG_HEADER}"
    ui/config/game_config.cpp
    ui/config/personal_options.cpp
    ui/config/user_data.cpp
    ui/frontend/controller.cpp
    ui/frontend/renderer.cpp
)
target_include_directories(mh_ui
    PUBLIC "${MH_INCLUDE_DIR}"
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(mh_ui PUBLIC mh_content)
mh_set_project_warnings(mh_ui)

set(MH_MOTORHEAD_SOURCES
    app/main.cpp
    runtime/race_runtime.cpp
    renderer/software/software_renderer.cpp
    "${MH_PLATFORM_ROOT}/src/renderer/race_scene_renderer.cpp"
    "${MH_PLATFORM_ROOT}/src/platform/frontend.cpp"
    "${MH_PLATFORM_ROOT}/src/platform/renderer_support.cpp"
)
if(MH_LINUX_VARIANT)
  list(APPEND MH_MOTORHEAD_SOURCES "${MH_PLATFORM_ROOT}/src/renderer/hardware/opengl_renderer.cpp")
else()
  foreach(name d3d_dynamic d3d11_renderer d3d9_renderer d3d12_renderer glide_renderer)
    list(APPEND MH_MOTORHEAD_SOURCES "${MH_PLATFORM_ROOT}/src/renderer/hardware/${name}.cpp")
  endforeach()
  list(APPEND MH_MOTORHEAD_SOURCES "${MH_PLATFORM_ROOT}/src/resources/motorhead.rc")
endif()

function(mh_configure_motorhead_executable target)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${MH_PLATFORM_ROOT}/src")
    target_link_libraries(${target} PRIVATE
        SDL3::SDL3-static
        mh_sdl_platform
        mh_game_content
        mh_network
        mh_ui
        mh_content
        mh_disc
        mh_core
    )
    mh_set_project_warnings(${target})
    mh_make_portable_executable(${target})
    if(MINGW AND NOT MH_LINUX_VARIANT)
        target_link_options(${target} PRIVATE
            -Wl,--major-os-version,5,--minor-os-version,1
            -Wl,--major-subsystem-version,5,--minor-subsystem-version,1)
    endif()
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endfunction()

# The production preset builds this target as a 32-bit Windows executable so
# one Motorhead.exe runs on both 32-bit Windows and 64-bit Windows via WoW64.
if(MH_LINUX_VARIANT)
  add_executable(Motorhead ${MH_MOTORHEAD_SOURCES})
else()
  add_executable(Motorhead WIN32 ${MH_MOTORHEAD_SOURCES})
endif()
mh_configure_motorhead_executable(Motorhead)

if(MH_LINUX_VARIANT AND NOT WIN32)
  target_link_options(Motorhead PRIVATE -static-libstdc++)
endif()
if(MH_LINUX_VARIANT AND MH_BUILD_PLATFORM_CHECKS)
  set(MH_PLATFORM_CHECK_SOURCE_DIR "" CACHE PATH "Directory containing optional platform verification sources")
  if(NOT EXISTS "${MH_PLATFORM_CHECK_SOURCE_DIR}/verify.cpp")
    message(FATAL_ERROR "Set MH_PLATFORM_CHECK_SOURCE_DIR to the development platform verification sources")
  endif()
  add_executable(SteamdeckVerify "${MH_PLATFORM_CHECK_SOURCE_DIR}/verify.cpp"
    "${MH_PLATFORM_CHECK_SOURCE_DIR}/input_verify.cpp" "${MH_PLATFORM_CHECK_SOURCE_DIR}/graphics_verify.cpp")
  target_link_libraries(SteamdeckVerify PRIVATE mh_sdl_platform mh_ui)
  mh_make_portable_executable(SteamdeckVerify)
endif()
