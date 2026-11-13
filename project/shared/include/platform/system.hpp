#pragma once
#include <ctime>
#include <filesystem>
namespace mh::platform {
std::tm local_time(std::time_t time);
void replace_file(const std::filesystem::path &source, const std::filesystem::path &destination);
void flush_file(const std::filesystem::path &path);
void initialize_application(const char *executable);
}
