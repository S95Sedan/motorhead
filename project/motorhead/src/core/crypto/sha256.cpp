#include <core/error.hpp>
#include <core/crypto/sha256.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace mh::common {
namespace {

constexpr std::array<std::uint32_t, 64> round_constants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

constexpr std::uint32_t rotate_right(const std::uint32_t value, const unsigned bits) {
    return (value >> bits) | (value << (32U - bits));
}

} // namespace

Sha256::Sha256()
    : state_{
          0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
          0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
      } {}

void Sha256::update(const std::uint8_t* data, const std::size_t size) {
    if (finished_) {
        throw ToolError(ExitCode::internal, "SHA-256 cannot be updated after finish");
    }
    if (size > 0U && data == nullptr) {
        throw ToolError(ExitCode::internal, "SHA-256 received a null data pointer");
    }

    total_bytes_ += static_cast<std::uint64_t>(size);
    std::size_t offset = 0;
    while (offset < size) {
        const auto available = buffer_.size() - buffered_;
        const auto amount = std::min(available, size - offset);
        std::copy_n(data + offset, amount, buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_));
        buffered_ += amount;
        offset += amount;
        if (buffered_ == buffer_.size()) {
            transform();
            buffered_ = 0;
        }
    }
}

void Sha256::update(const std::string_view text) {
    update(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

void Sha256::transform() {
    std::array<std::uint32_t, 64> schedule{};
    for (std::size_t i = 0; i < 16U; ++i) {
        const auto offset = i * 4U;
        schedule[i] = (static_cast<std::uint32_t>(buffer_[offset]) << 24U)
            | (static_cast<std::uint32_t>(buffer_[offset + 1U]) << 16U)
            | (static_cast<std::uint32_t>(buffer_[offset + 2U]) << 8U)
            | static_cast<std::uint32_t>(buffer_[offset + 3U]);
    }
    for (std::size_t i = 16U; i < schedule.size(); ++i) {
        const auto s0 = rotate_right(schedule[i - 15U], 7U)
            ^ rotate_right(schedule[i - 15U], 18U)
            ^ (schedule[i - 15U] >> 3U);
        const auto s1 = rotate_right(schedule[i - 2U], 17U)
            ^ rotate_right(schedule[i - 2U], 19U)
            ^ (schedule[i - 2U] >> 10U);
        schedule[i] = schedule[i - 16U] + s0 + schedule[i - 7U] + s1;
    }

    auto a = state_[0];
    auto b = state_[1];
    auto c = state_[2];
    auto d = state_[3];
    auto e = state_[4];
    auto f = state_[5];
    auto g = state_[6];
    auto h = state_[7];

    for (std::size_t i = 0; i < schedule.size(); ++i) {
        const auto sum1 = rotate_right(e, 6U) ^ rotate_right(e, 11U) ^ rotate_right(e, 25U);
        const auto choice = (e & f) ^ ((~e) & g);
        const auto temporary1 = h + sum1 + choice + round_constants[i] + schedule[i];
        const auto sum0 = rotate_right(a, 2U) ^ rotate_right(a, 13U) ^ rotate_right(a, 22U);
        const auto majority = (a & b) ^ (a & c) ^ (b & c);
        const auto temporary2 = sum0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + temporary1;
        d = c;
        c = b;
        b = a;
        a = temporary1 + temporary2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

std::array<std::uint8_t, 32> Sha256::finish() {
    if (!finished_) {
        const auto message_bits = total_bytes_ * 8U;
        buffer_[buffered_++] = 0x80U;
        if (buffered_ > 56U) {
            std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_), buffer_.end(), 0U);
            transform();
            buffered_ = 0;
        }
        std::fill(
            buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_),
            buffer_.begin() + 56,
            0U);
        for (std::size_t i = 0; i < 8U; ++i) {
            buffer_[63U - i] = static_cast<std::uint8_t>(message_bits >> (i * 8U));
        }
        transform();
        buffered_ = 0;
        finished_ = true;
    }

    std::array<std::uint8_t, 32> digest{};
    for (std::size_t i = 0; i < state_.size(); ++i) {
        digest[i * 4U] = static_cast<std::uint8_t>(state_[i] >> 24U);
        digest[i * 4U + 1U] = static_cast<std::uint8_t>(state_[i] >> 16U);
        digest[i * 4U + 2U] = static_cast<std::uint8_t>(state_[i] >> 8U);
        digest[i * 4U + 3U] = static_cast<std::uint8_t>(state_[i]);
    }
    return digest;
}

std::string Sha256::finish_hex() {
    const auto digest = finish();
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto byte : digest) {
        output << std::setw(2) << static_cast<unsigned>(byte);
    }
    return output.str();
}

std::string sha256(const std::string_view text) {
    Sha256 hasher;
    hasher.update(text);
    return hasher.finish_hex();
}

std::string sha256_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw ToolError(ExitCode::input, "cannot open file for hashing: " + path.string());
    }

    Sha256 hasher;
    // Keep the streaming buffer comfortably below Windows' default stack
    // reserve. File size does not affect memory use.
    std::array<std::uint8_t, 64U * 1024U> buffer{};
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) {
            hasher.update(buffer.data(), static_cast<std::size_t>(count));
        }
    }
    if (!input.eof()) {
        throw ToolError(ExitCode::input, "failed while hashing file: " + path.string());
    }
    return hasher.finish_hex();
}

} // namespace mh::common
