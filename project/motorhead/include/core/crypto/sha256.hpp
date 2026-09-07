#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace mh::common {

class Sha256 final {
public:
    Sha256();

    void update(const std::uint8_t* data, std::size_t size);
    void update(std::string_view text);
    [[nodiscard]] std::array<std::uint8_t, 32> finish();
    [[nodiscard]] std::string finish_hex();

private:
    void transform();

    std::array<std::uint8_t, 64> buffer_{};
    std::array<std::uint32_t, 8> state_{};
    std::size_t buffered_ = 0;
    std::uint64_t total_bytes_ = 0;
    bool finished_ = false;
};

[[nodiscard]] std::string sha256(std::string_view text);
[[nodiscard]] std::string sha256_file(const std::filesystem::path& path);

} // namespace mh::common

