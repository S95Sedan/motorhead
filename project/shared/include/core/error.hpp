#pragma once

#include <stdexcept>
#include <string>

namespace mh::common {

enum class ExitCode : int {
    success = 0,
    usage = 2,
    input = 3,
    format = 4,
    verification = 5,
    internal = 10,
};

class ToolError final : public std::runtime_error {
public:
    ToolError(ExitCode code, const std::string& message)
        : std::runtime_error(message), code_(code) {}

    [[nodiscard]] ExitCode code() const noexcept { return code_; }

private:
    ExitCode code_;
};

} // namespace mh::common
