add_executable(ModelViewer WIN32
    application.cpp
    core/json.cpp
    core/sha256.cpp
    formats/ai_route.cpp
    formats/bitmap_font.cpp
    formats/car_definition.cpp
    formats/collision_mesh.cpp
    formats/encoded_table.cpp
    formats/iff_image.cpp
    formats/model_geometry.cpp
    formats/texture_archive.cpp
    formats/tga_image.cpp
    formats/track_definition.cpp
    formats/track_world.cpp
    main.cpp
    services/asset_inventory.cpp
    services/texture_catalog.cpp
    windows/resources.rc
)

target_include_directories(ModelViewer PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(ModelViewer PRIVATE SDL3::SDL3-static)
if(WIN32)
    target_link_libraries(ModelViewer PRIVATE gdi32)
endif()

modelviewer_set_compiler_warnings(ModelViewer)
modelviewer_make_portable(ModelViewer)

set_target_properties(ModelViewer PROPERTIES
    OUTPUT_NAME "Modelviewer"
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/../../bin"
)
