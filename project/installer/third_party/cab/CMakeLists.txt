add_library(mh_cab STATIC
    src/cabinet.cpp
)

target_include_directories(mh_cab PUBLIC include)
target_link_libraries(mh_cab PUBLIC mh_core PRIVATE ZLIB::ZLIBSTATIC)
mh_set_project_warnings(mh_cab)
