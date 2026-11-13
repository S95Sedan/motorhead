#pragma once
#include <filesystem>
#include <fstream>
namespace mh::native {
namespace fs=std::filesystem;
using fs::path;
using fs::exists;
using fs::is_regular_file;
using fs::file_size;
using fs::symlink_status;
using fs::directory_iterator;
using fs::recursive_directory_iterator;
using InputFile=std::ifstream;
inline const fs::path &asset_path(const fs::path &value) { return value; }
}
