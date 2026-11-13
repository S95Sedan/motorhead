#include <core/json.hpp>

#include <array>
#include <cstdint>

namespace mh::common {

std::string json_string(const std::string_view value) {
    constexpr std::array<char, 16> hex{
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
    };

    std::string result;
    result.reserve(value.size() + 2U);
    result.push_back('"');
    for (const unsigned char byte : value) {
        switch (byte) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (byte < 0x20U) {
                result += "\\u00";
                result.push_back(hex[(byte >> 4U) & 0x0fU]);
                result.push_back(hex[byte & 0x0fU]);
            } else {
                result.push_back(static_cast<char>(byte));
            }
            break;
        }
    }
    result.push_back('"');
    return result;
}

} // namespace mh::common

