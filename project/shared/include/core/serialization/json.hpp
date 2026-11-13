#pragma once

#include <string>
#include <string_view>

namespace mh::common {

[[nodiscard]] std::string json_string(std::string_view value);

} // namespace mh::common

